#include "ppu_recomp.h"
#include "haze_audio_settings.h"
#include "haze_save_status.h"
#include <atomic>
#include <mutex>
#include <cstdio>

namespace {
std::mutex state_mutex;
HazeAudioState state;
HazeAudioValues requested_values;
std::atomic<uint64_t> owner_thread{0};
#ifdef _WIN32
thread_local bool inside=false;
uint32_t manager=0,page_context=0,profile=0,player=0;
std::array<uint32_t,3> widgets{};
uint32_t subtitle_widget=0;
constexpr uint32_t subtitle_name=0xc44bb0;
uint64_t saving_ticket=0;
constexpr uint32_t names[]={0xc42d30,0xc42940,0xc42d20};
constexpr uint32_t fields[]={0xe8,0xf0,0xe4};
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
// Menu/profile objects also live in the guest's container allocations near
// 0x40000000. Bound every structure read within the 32-bit guest arena.
bool pointer(uint32_t value) { return value>=0x10000 && value<=UINT32_MAX-0x400 && !(value&3); }
uint64_t call(ppu_context* ctx,void(*function)(ppu_context*),uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
    // Borrow space below the current guest stack, not the runtime's shared
    // callback scratch address. Keep the original context and trampoline intact.
    const auto saved=*ctx; auto trampoline=g_trampoline_fn; g_trampoline_fn=nullptr;
    ctx->gpr[1]-=0x200; ctx->gpr[2]=0xe94ee0;
    ctx->gpr[3]=a; ctx->gpr[4]=b; ctx->gpr[5]=c; ctx->gpr[6]=d;
    function(ctx);
    while (g_trampoline_fn) { auto next=g_trampoline_fn; g_trampoline_fn=nullptr; next(ctx); }
    const auto result=ctx->gpr[3]; *ctx=saved; g_trampoline_fn=trampoline; return result;
}
bool supported() {
    const uint32_t checks[][5]={
        {0x311248,0xf821ff51,0x7c0802a6,0xfb810090,0x3b810070},
        {0xfff20,0xf821ff71,0x7c0802a6,0xfbc10080,0x83c28094},
        {0x100300,0x2f830000,0x7c0802a6,0xf821ff21,0xfbc100d0},
        {0x100230,0xf821ff61,0x7c0802a6,0xfbc10090,0x83c28094},
        {0x100828,0xf821fd21,0x7c0802a6,0xfbc102d0,0x83c28094},
        {0x4e6f20,0x5484103a,0x38840010,0x7c8407b4,0x7c63202e},
        {0x4d11f0,0x3c00abcd,0x2f840000,0x6000abcd,0x7c840278},
        {0x4d2f90,0xf821ff61,0x7c0802a6,0xfbc10088,0x83c281bc},
        {0x314380,0xf821ff61,0x7c0802a6,0xfb810080,0x7c9c2378}};
    for (const auto& check:checks) for (int i=0;i<4;++i)
        if (vm_read32(check[0]+uint32_t(i)*4)!=check[i+1]) {
            std::fprintf(stderr,"[audio-settings] Unsupported instruction at %08x\n",check[0]+uint32_t(i)*4);
            return false;
        }
    return true;
}
bool read_values(HazeAudioValues& values) {
    for (int i=0;i<3;++i) {
        if (!pointer(widgets[i])) return false;
        values[i]=(int32_t)vm_read32(widgets[i]+0x154);
        if (values[i]<0 || values[i]>100) return false;
    }
    if (!pointer(subtitle_widget)) return false;
    const auto index=vm_read32(subtitle_widget+0x170),entries=vm_read32(subtitle_widget+0x160);
    if (index>15 || !pointer(entries) || entries>UINT32_MAX-16*0x4c) return false;
    values[3]=(int32_t)vm_read32(entries+index*0x4c+4);
    if (values[3]!=0 && values[3]!=1) return false;
    return true;
}
HazeAudioValues profile_values() {
    return {(int)vm_read32(profile+fields[0]),(int)vm_read32(profile+fields[1]),
        (int)vm_read32(profile+fields[2]),int((vm_read32(profile+0xd0)&0x20)!=0)};
}
void set_widgets(ppu_context* ctx,const HazeAudioValues& values) {
    for (int i=0;i<3;++i) call(ctx,func_000FFF20,names[i],values[i]);
    call(ctx,func_00100230,subtitle_name,values[3]);
}
bool same_page() {
    if (vm_read32(0xf48dd4)!=manager || vm_read32(manager+0x48)!=page_context) return false;
    if (vm_read32(page_context+0x34)!=player) return false;
    const auto count=vm_read32(page_context+0x50),stack=vm_read32(page_context+0x54);
    if (!count || count>64 || !pointer(stack)) return false;
    const char id[16]="sett_DC_page3";
    for (unsigned i=0;i<16;++i) if (vm_read8(stack+(count-1)*16+i)!=(uint8_t)id[i]) return false;
    return true;
}
uint32_t find_profile(ppu_context* ctx) {
    if (!player) return (uint32_t)call(ctx,func_004E6F20,vm_read32(0xf48de0),0);
    const auto user=(uint32_t)call(ctx,func_004D11F0,vm_read32(0xf48c3c),player);
    return pointer(user) ? vm_read32(user+0xc0) : 0;
}
void finish(ppu_context* ctx,bool success,const char* message) {
    if (!same_page() || find_profile(ctx)!=profile) {
        std::lock_guard lock(state_mutex); state.ready=false; state.phase=HazeAudioPhase::Failed;
        state.message="Your active profile changed while saving. Reopen Audio settings."; return;
    }
    if (profile_values()!=requested_values) {
        std::lock_guard lock(state_mutex); state.ready=false; state.phase=HazeAudioPhase::Failed;
        state.message="Audio settings changed while saving. Reopen this page."; return;
    }
    HazeAudioValues original;
    { std::lock_guard lock(state_mutex); original=state.values; }
    if (!success) {
        for (int i=0;i<3;++i) {
            vm_write32(profile+fields[i],(uint32_t)original[i]);
        }
        vm_write32(profile+0xd0,(vm_read32(profile+0xd0)&~0x20u)|(original[3] ? 0x20u : 0));
        set_widgets(ctx,original);
        call(ctx,func_004D2F90,profile);
    }
    call(ctx,func_00314380,manager,0);
    std::lock_guard lock(state_mutex);
    if (success) state.values=requested_values;
    state.phase=success ? HazeAudioPhase::Applied : HazeAudioPhase::Failed;
    state.message=message;
    std::fprintf(stderr,"[audio-settings] %s\n",message);
}
#endif
}
HazeAudioState haze_audio_state() { std::lock_guard lock(state_mutex); return state; }
bool haze_audio_apply(const HazeAudioValues& values,uint64_t generation,std::string& message) {
    std::lock_guard lock(state_mutex);
    if (!state.ready || generation!=state.generation) { message="Reopen Audio settings to read your current profile."; return false; }
    if (state.phase==HazeAudioPhase::Queued || state.phase==HazeAudioPhase::Saving) { message="Audio settings are still saving."; return false; }
    for (int value:values) if (value<0 || value>100) { message="Audio volume must be between 0 and 100."; return false; }
    if (values[3]>1) { message="Choose On or Off for subtitles."; return false; }
    requested_values=values; state.phase=HazeAudioPhase::Queued;
    std::fprintf(stderr,"[audio-settings] Queued volumes %d/%d/%d, subtitles %d\n",values[0],values[1],values[2],values[3]);
    state.message=message="Saving audio settings..."; return true;
}
bool haze_audio_guest_open(ppu_context* ctx) {
#ifdef _WIN32
    if (inside) return false;
    {
        std::lock_guard lock(state_mutex);
        if (state.phase==HazeAudioPhase::Queued || state.phase==HazeAudioPhase::Saving) return false;
        state.ready=false;
    }
    auto unavailable=[](const char* reason) {
        std::fprintf(stderr,"[audio-settings] Cannot open: %s (manager=%08x context=%08x profile=%08x)\n",reason,manager,page_context,profile);
        return false;
    };
    if (!ctx || !ctx->thread_id) return unavailable("guest context");
    if (!supported()) return unavailable("guest instructions");
    inside=true;
    struct Guard { ~Guard() { inside=false; } } guard;
    manager=vm_read32(0xf48dd4);
    if (!pointer(manager)) return unavailable("menu manager");
    page_context=vm_read32(manager+0x48);
    if (!pointer(page_context)) return unavailable("page context");
    player=vm_read32(page_context+0x34);
    if (!same_page()) {
        const auto count=vm_read32(page_context+0x50),stack=vm_read32(page_context+0x54);
        char id[17]{};
        if (count && count<=64 && pointer(stack)) for (unsigned i=0;i<16;++i) id[i]=(char)vm_read8(stack+(count-1)*16+i);
        std::fprintf(stderr,"[audio-settings] Page stack count=%u base=%08x top=%s\n",count,stack,id);
        return unavailable("active page identity");
    }
    profile=find_profile(ctx);
    if (!pointer(profile)) return unavailable("profile");
    for (int i=0;i<3;++i) widgets[i]=(uint32_t)call(ctx,func_00311248,manager,names[i]);
    subtitle_widget=(uint32_t)call(ctx,func_00311248,manager,subtitle_name);
    HazeAudioValues values{},defaults{},restored{};
    if (!read_values(values)) return unavailable("audio slider values");
    // Let the original default-profile loader choose defaults, then restore
    // the current profile before exposing the native page. No profile writes.
    call(ctx,func_00100828,0,0,player,1);
    const bool defaults_ok=read_values(defaults);
    call(ctx,func_00100828,0,0,player,0);
    call(ctx,func_00314380,manager,0);
    if (!defaults_ok || !read_values(restored) || restored!=values) return unavailable("default profile restoration");
    {
        std::lock_guard lock(state_mutex);
        ++state.generation; state.ready=true; state.values=values; state.defaults=defaults;
        state.phase=HazeAudioPhase::Ready; state.message.clear();
    }
    owner_thread=ctx->thread_id;
    std::fprintf(stderr,"[audio-settings] Profile %08x, volumes %d/%d/%d, defaults %d/%d/%d, subtitles %d (default %d), thread %llu\n",
        profile,values[0],values[1],values[2],defaults[0],defaults[1],defaults[2],values[3],defaults[3],(unsigned long long)ctx->thread_id);
    return true;
#else
    (void)ctx; return false;
#endif
}
void haze_audio_guest_tick(ppu_context* ctx) {
#ifdef _WIN32
    if (inside || !ctx || ctx->thread_id!=owner_thread.load()) return;
    HazeAudioState snapshot;
    HazeAudioValues values;
    { std::lock_guard lock(state_mutex); snapshot=state; values=requested_values; }
    if (snapshot.phase!=HazeAudioPhase::Queued && snapshot.phase!=HazeAudioPhase::Saving) return;
    inside=true;
    struct Guard { ~Guard() { inside=false; } } guard;
    if (snapshot.phase==HazeAudioPhase::Saving) {
        const auto save=haze_save_status(saving_ticket);
        if (!save.ticket) {
            std::lock_guard lock(state_mutex); state.ready=false; state.phase=HazeAudioPhase::Failed;
            state.message="The save result is no longer available. Reopen Audio settings."; return;
        }
        if (!save.completed) return;
        finish(ctx,save.result==0,save.result==0 ? "Audio settings saved." : "Could not save your profile. Previous audio settings restored.");
        return;
    }
    const bool unchanged=same_page() && find_profile(ctx)==profile && profile_values()==snapshot.values;
    if (!unchanged) {
        std::lock_guard lock(state_mutex); state.phase=HazeAudioPhase::Failed;
        state.message="Audio settings changed elsewhere. Reopen this page."; return;
    }
    if (vm_read32(0x137a390) || haze_save_status().busy) return;
    const auto before=haze_save_status().ticket;
    set_widgets(ctx,values);
    {
        haze_save_settings_arm(true);
        struct SaveScope { ~SaveScope() { haze_save_settings_arm(false); } } save_scope;
        call(ctx,func_00100300,player);
    }
    const auto after=haze_save_status();
    if (after.ticket==before) { finish(ctx,false,"The game could not start saving. Previous audio settings restored."); return; }
    saving_ticket=after.ticket;
    { std::lock_guard lock(state_mutex); state.phase=HazeAudioPhase::Saving; }
    if (after.completed) finish(ctx,after.result==0,after.result==0 ? "Audio settings saved." : "Could not save your profile. Previous audio settings restored.");
#else
    (void)ctx;
#endif
}
