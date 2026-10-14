#include "ppu_recomp.h"
#include "haze_input.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace {
// Only the audited legal/Haze and online-rating textures qualify. Demo outro
// slides use the same scene class, so class identity alone is insufficient.
std::unordered_map<uint32_t, bool> startup_textures;
bool startup_texture(uint32_t address) {
    if (!address || address>UINT32_MAX-64) return false;
    char name[64]{};
    for (unsigned i=0;i<sizeof(name)-1;++i) {
        name[i]=char(vm_read8(address+i));
        if (!name[i]) break;
    }
    for (const char* allowed : {"misctex/loading/legal_eng", "misctex/loading/legal_eng_sd",
            "misctex/loading/esrb_eng", "misctex/loading/esrb_eng_sd",
            "misctex/loading/esrb_fr", "misctex/loading/esrb_fr_sd"})
        if (!std::strcmp(name,allowed)) return true;
    return false;
}
bool supported(uint32_t scene) {
    constexpr uint32_t code[]={0xf821ff71,0x7c0802a6,0xdbe10088,0xffe00890,
        0xf80100a0,0x80030040,0xfbc10078,0x2f800001};
    for (unsigned i=0;i<8;++i) if (vm_read32(0x6407c0+i*4)!=code[i]) return false;
    if (!scene || scene>UINT32_MAX-0x44) return false;
    const uint32_t count=vm_read32(scene+0x2c), slides=vm_read32(scene+0x30);
    if (!count || count>2 || !slides || uint64_t(slides)+count*32>UINT32_MAX) return false;
    for (uint32_t i=0;i<count;++i) {
        const auto found=startup_textures.find(vm_read32(slides+i*32+0x10));
        if (found==startup_textures.end() || !found->second) return false;
    }
    return true;
}
uint32_t observed_scene=0;
int observed_slide=-2;
std::chrono::steady_clock::time_point slide_started;
}

// Called on the guest thread after a splash texture is loaded. Record every
// handle assignment, including non-startup slides, to handle texture-ID reuse.
void haze_startup_texture_loaded(uint32_t texture,uint32_t name) {
    if (texture!=UINT32_MAX) startup_textures[texture]=startup_texture(name);
}

bool haze_startup_splash_tick(uint32_t scene) {
    if (!supported(scene)) return false;
    const uint32_t state=vm_read32(scene+0x40);
    const int index=int32_t(vm_read32(scene+0x20));
    // State 1 fades in the next slide; state 2 holds that slide; state 3
    // performs the final fade. Keep a manual request latched for one slide.
    const int screen=state==1 ? index+1 : index;
    if (observed_scene!=scene || observed_slide!=screen) {
        const auto now=std::chrono::steady_clock::now();
        if (observed_scene==scene)
            std::fprintf(stderr,"[startup] Static slide %d presentation: %lld ms\n",observed_slide,
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now-slide_started).count());
        observed_scene=scene; observed_slide=screen; slide_started=now;
    }
    return haze_input_splash(scene,screen);
}

void haze_startup_splash_end(uint32_t scene) {
    if (observed_scene!=scene) return;
    const auto elapsed=std::chrono::steady_clock::now()-slide_started;
    std::fprintf(stderr,"[startup] Static slide %d presentation: %lld ms; splash complete\n",observed_slide,
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    haze_input_splash_end(scene);
    observed_scene=0; observed_slide=-2;
}
