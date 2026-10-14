#include <cassert>
#include <vector>
#include "haze_splash_lift.inc"
#include "../port/haze_startup_guest.cpp"
#include "../port/haze_startup.h"

namespace {
std::vector<uint8_t> memory(0x650000);
HazeStartupSkip policy;
bool texture_ready=true;
uint64_t ticks=0;
constexpr uint32_t scene=0x2000, slides=0x3000;
}
extern "C" { PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*)=nullptr; }
uint8_t vm_read8(uint64_t a) { return memory.at(size_t(a)); }
uint32_t vm_read32(uint64_t a) {
    return uint32_t(vm_read8(a))<<24 | uint32_t(vm_read8(a+1))<<16 |
           uint32_t(vm_read8(a+2))<<8 | vm_read8(a+3);
}
uint64_t vm_read64(uint64_t a) { return uint64_t(vm_read32(a))<<32 | vm_read32(a+4); }
void vm_write32(uint64_t a,uint32_t v) { for (unsigned i=0;i<4;++i) memory.at(size_t(a+i))=uint8_t(v>>(24-8*i)); }
void vm_write64(uint64_t a,uint64_t v) { vm_write32(a,uint32_t(v>>32)); vm_write32(a+4,uint32_t(v)); }
void func_006A6B98(ppu_context* ctx) { ctx->gpr[3]=texture_ready; }
bool haze_input_splash(uint32_t s,int slide) { return policy.splash(s,slide,ticks); }
void haze_input_splash_end(uint32_t s) { policy.splash_end(s); }
void texture(uint32_t handle,const char* name) {
    std::strcpy(reinterpret_cast<char*>(memory.data()+0x4000),name);
    haze_startup_texture_loaded(handle,0x4000);
}
void prepare(bool automatic) {
    policy.splash_end(scene); policy.filter(ticks,0,true,1);
    policy.configure(automatic,false);
    std::fill(memory.begin(),memory.end(),0);
    constexpr uint32_t code[]={0xf821ff71,0x7c0802a6,0xdbe10088,0xffe00890,
        0xf80100a0,0x80030040,0xfbc10078,0x2f800001};
    for (unsigned i=0;i<8;++i) vm_write32(0x6407c0+i*4,code[i]);
    vm_write32(0x9000-0x7de0,0x9000); // Test TOC/table; final fade duration = .5.
    vm_write32(0x9000-0x7f78,0x3f000000);
    vm_write32(scene+0x20,UINT32_MAX); vm_write32(scene+0x2c,2); vm_write32(scene+0x30,slides);
    for (unsigned i=0;i<2;++i) {
        vm_write32(slides+i*32+0x10,10+i);
        vm_write32(slides+i*32+0x14,0x3f800000); // One-second fade.
        vm_write32(slides+i*32+0x18,0x40800000); // Four-second hold.
    }
    texture(10,"misctex/loading/legal_eng"); texture(11,"misctex/loading/esrb_eng");
    texture_ready=true;
}
bool tick(double delta=1.0/60) {
    ppu_context ctx{}; ctx.gpr[1]=0x6000; ctx.gpr[2]=0x9000;
    ctx.gpr[3]=scene; ctx.fpr[1]=delta; ctx.lr=0x1234;
    ctx.gpr[29]=29; ctx.gpr[30]=30; ctx.gpr[31]=31; ctx.fpr[31]=31.5;
    func_006407C0(&ctx); ticks+=16;
    assert(ctx.gpr[1]==0x6000 && ctx.lr==0x1234 && ctx.gpr[29]==29 &&
           ctx.gpr[30]==30 && ctx.gpr[31]==31 && ctx.fpr[31]==31.5);
    return ctx.gpr[3]!=0;
}
int main() {
    prepare(true); assert(!tick()); // Initialization selects fade-in.
    texture_ready=false;
    for (int i=0;i<5000;++i) assert(!tick()); // No timeout bypass of loading.
    assert(vm_read32(scene+0x40)==1 && vm_read32(scene+0x20)==UINT32_MAX);
    texture_ready=true;
    for (int i=0;i<4;++i) assert(!tick());
    assert(tick() && !policy.showing());
    assert(!policy.filter(ticks,1,true,1)); // Held skip cannot reach title.
    assert(!policy.filter(ticks,0,true,1));
    assert(policy.filter(ticks,2,true,1)==2);
    prepare(false); assert(!tick()); assert(!tick());
    assert(vm_read32(scene+0x40)==1); // Normal presentation still waits.
    assert(!policy.filter(ticks,1,true,1)); assert(!tick());
    assert(vm_read32(scene+0x40)==2); assert(!tick());
    assert(vm_read32(scene+0x40)==1); assert(!tick());
    assert(vm_read32(scene+0x20)==0); // Second slide needs another press.
    policy.filter(ticks,1,true,1); assert(!tick()); assert(!tick()); assert(tick());
    prepare(false);
    int frames=0; while (!tick() && ++frames<700) {}
    assert(frames>600 && frames<650); // Both normal durations plus fade-out.
    prepare(true); texture(11,"misctex/loading/demo_screen1_hd_english");
    for (int i=0;i<10;++i) assert(!tick());
    assert(!policy.showing() && vm_read32(scene+0x40)==1); // Demo outro untouched.
    prepare(true); vm_write32(0x6407c0,0);
    assert(!haze_startup_splash_tick(scene) && !policy.showing());
    prepare(true); vm_write32(scene+0x30,UINT32_MAX-16);
    assert(!haze_startup_splash_tick(scene) && !policy.showing());
    puts("Actual guest splash update preserves loading, normal timing, registers, title input and unrelated slides.");
}
