// Actual bridge/provider against a bounded guest-memory fixture and audited
// call seams. Real guest callbacks and disk persistence are native-run checks.
#include "ppu_recomp.h"
#include <cassert>
#include <cstring>
#include <vector>
#include "../port/haze_audio_guest.cpp"
#include "../port/haze_save_status.cpp"
#include "../port/haze_audio_menu.cpp"
static std::vector<uint8_t> memory(0x1380000);
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*)=nullptr;
static size_t host_offset(uint64_t p) { return size_t(p>=0x40000000 ? p-0x40000000 : p); }
extern "C" uint8_t vm_read8(uint64_t p) { return memory.at(host_offset(p)); }
extern "C" uint32_t vm_read32(uint64_t p) { return uint32_t(vm_read8(p))<<24 | uint32_t(vm_read8(p+1))<<16 | uint32_t(vm_read8(p+2))<<8 | vm_read8(p+3); }
extern "C" void vm_write32(uint64_t p,uint32_t v) { for (unsigned i=0;i<4;++i) memory.at(host_offset(p+i))=uint8_t(v>>(24-i*8)); }
static constexpr uint32_t test_manager=0x40020000,test_context=0x40021000,test_profile=0x40022000,test_stack=0x40023000;
static constexpr uint32_t test_widgets[]={0x40024000,0x40025000,0x40026000};
static constexpr uint32_t test_subtitles=0x40028000,test_options=0x40029000;
static unsigned applies,restores;
static bool launch_failure;
static std::array<int,3> mixed;
void func_00311248(ppu_context* ctx) {
    auto name=ctx->gpr[4]; ctx->gpr[3]=0;
    for (int i=0;i<3;++i) if (name==names[i]) ctx->gpr[3]=test_widgets[i];
    if (name==subtitle_name) ctx->gpr[3]=test_subtitles;
    ctx->gpr[4]=0xbadc0de;
}
void func_004D11F0(ppu_context* ctx) { ctx->gpr[3]=0x27000; }
void func_004E6F20(ppu_context* ctx) { ctx->gpr[3]=test_profile; }
void func_00100828(ppu_context* ctx) {
    for (int i=0;i<3;++i) vm_write32(test_widgets[i]+0x154,ctx->gpr[6] ? 70 : vm_read32(test_profile+fields[i]));
    vm_write32(test_subtitles+0x170,ctx->gpr[6] ? 1 : ((vm_read32(test_profile+0xd0)&0x20)!=0));
}
void func_00100230(ppu_context* ctx) {
    assert(ctx->gpr[3]==subtitle_name && ctx->gpr[4]<=1);
    vm_write32(test_subtitles+0x170,(uint32_t)ctx->gpr[4]);
}
void func_00314380(ppu_context*) { ++restores; }
void func_000FFF20(ppu_context* ctx) {
    for (int i=0;i<3;++i) if (ctx->gpr[3]==names[i]) vm_write32(test_widgets[i]+0x154,(uint32_t)ctx->gpr[4]);
    haze_audio_guest_tick(ctx); // Nested calls must not consume the request again.
}
void func_004D2F90(ppu_context*) { for (int i=0;i<3;++i) mixed[i]=(int)vm_read32(test_profile+fields[i]); }
void func_00100300(ppu_context* ctx) {
    ++applies;
    for (int i=0;i<3;++i) vm_write32(test_profile+fields[i],vm_read32(test_widgets[i]+0x154));
    vm_write32(test_profile+0xd0,(vm_read32(test_profile+0xd0)&~0x20u)|(vm_read32(test_subtitles+0x170) ? 0x20u : 0));
    func_004D2F90(ctx);
    if (!launch_failure) { haze_save_begin(0xe71ac8); assert(haze_save_is_settings()); }
}
static HazeMenuProvider registered;
void haze_menu_register(HazeMenuPage page,HazeMenuProvider value) { assert(page==HazeMenuPage::Audio); registered=std::move(value); }
int main() {
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
    for (const auto& check:checks) for (unsigned i=0;i<4;++i) vm_write32(check[0]+i*4,check[i+1]);
    vm_write32(0xf48dd4,test_manager); vm_write32(test_manager+0x48,test_context);
    vm_write32(test_context+0x50,1); vm_write32(test_context+0x54,test_stack);
    std::memcpy(memory.data()+host_offset(test_stack),"sett_DC_page3",13);
    vm_write32(0x27000+0xc0,test_profile);
    vm_write32(test_subtitles+0x160,test_options);
    vm_write32(test_options+4,0); vm_write32(test_options+0x4c+4,1);
    vm_write32(test_profile+0xd0,0x40000); // Preserve unrelated voice flag.
    for (int i=0;i<3;++i) { vm_write32(test_widgets[i]+0x154,80); vm_write32(test_profile+fields[i],80); }
    ppu_context ctx{}; ctx.thread_id=7; ctx.gpr[1]=0xf00000; ctx.lr=123; ctx.fpr[1]=1.25; ctx.gpr[4]=0xabcdef;
    const auto original=ctx;
    assert(haze_audio_guest_open(&ctx)); assert(!std::memcmp(&ctx,&original,sizeof ctx));
    auto initial=haze_audio_state(); assert(initial.ready && initial.values[0]==80 && initial.defaults[0]==70);
    haze_audio_menu_register(); registered.begin(); registered.change("music","55");
    registered.change("subtitles","1"); registered.change("subtitles","3");
    assert(registered.rows()[3].value=="1");
    registered.change("effects","-5"); registered.change("dialogue","12garbage");
    assert(registered.rows()[0].value=="55" && registered.rows()[1].value=="80" && registered.rows()[2].value=="80");
    std::string message;
    assert(registered.apply(message)==HazeMenuApply::Pending);
    ctx.thread_id=8; haze_audio_guest_tick(&ctx); assert(applies==0); ctx.thread_id=7;
    vm_write32(0x137a390,1); haze_audio_guest_tick(&ctx); assert(applies==0); vm_write32(0x137a390,0);
    haze_audio_guest_tick(&ctx); assert(applies==1 && mixed[0]==55 && !std::memcmp(&ctx,&original,sizeof ctx));
    assert(vm_read32(test_profile+0xd0)==0x40020);
    assert(registered.poll(message)==HazeMenuApply::Pending);
    auto ticket=haze_save_status().ticket; haze_save_finished(ticket,0);
    // Another save starting before the next menu frame cannot erase our result.
    auto other=haze_save_begin(0xe71ac8); assert(!haze_save_is_settings()); haze_save_finished(other,0);
    haze_audio_guest_tick(&ctx); assert(registered.poll(message)==HazeMenuApply::Applied);
    registered.change("music","10"); registered.change("subtitles","0"); assert(registered.apply(message)==HazeMenuApply::Pending);
    haze_audio_guest_tick(&ctx); haze_save_finished(haze_save_status().ticket,(int32_t)0x8002b402);
    haze_audio_guest_tick(&ctx); assert(registered.poll(message)==HazeMenuApply::Failed);
    assert(mixed[0]==55 && vm_read32(test_profile+0xe8)==55 && vm_read32(test_widgets[0]+0x154)==55);
    assert(vm_read32(test_profile+0xd0)==0x40020 && vm_read32(test_subtitles+0x170)==1);
    registered.defaults(); assert(registered.rows()[0].value=="70");
    launch_failure=true; assert(registered.apply(message)==HazeMenuApply::Pending); haze_audio_guest_tick(&ctx);
    assert(registered.poll(message)==HazeMenuApply::Failed && mixed[0]==55); launch_failure=false;
    const auto count=applies;
    vm_write32(test_profile+0xe8,60);
    assert(registered.apply(message)==HazeMenuApply::Pending); haze_audio_guest_tick(&ctx);
    assert(applies==count && registered.poll(message)==HazeMenuApply::Failed && vm_read32(test_profile+0xe8)==60);
    vm_write32(test_profile+0xe8,55);
    assert(haze_audio_guest_open(&ctx)); assert(!haze_audio_apply({5,5,5},initial.generation,message));
    registered.begin(); registered.change("music","40");
    assert(registered.apply(message)==HazeMenuApply::Pending); haze_audio_guest_tick(&ctx);
    assert(!haze_audio_guest_open(&ctx)); // Do not replace an in-flight session.
    vm_write32(test_profile+0xe8,45); // An external change during the worker.
    haze_save_finished(haze_save_status().ticket,(int32_t)0x8002b402);
    haze_audio_guest_tick(&ctx);
    assert(registered.poll(message)==HazeMenuApply::Failed && vm_read32(test_profile+0xe8)==45);
    assert(!haze_audio_state().ready);
    vm_write32(test_widgets[0]+0x154,45);
    assert(haze_audio_guest_open(&ctx));
    vm_write32(0x100300,0); assert(!haze_audio_guest_open(&ctx));
    assert(!haze_audio_apply({5,5,5},haze_audio_state().generation,message));
    assert(!std::memcmp(&ctx,&original,sizeof ctx));
    std::puts("Audio bridge/provider: context preservation, staging, defaults, thread ownership, save completion and rollback passed.");
}
