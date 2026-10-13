// Actual guest-boundary bridge and save coordinator with bounded VM reads.
#include "ppu_recomp.h"
#include <cassert>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>
#include "../port/haze_quit_guest.cpp"
#include "../port/haze_quit.cpp"
#include "../port/haze_save_status.cpp"
static std::vector<uint8_t> memory(0x1380000);
static HazeAudioState audio;
HazeAudioState haze_audio_state() { return audio; }
static size_t offset(uint64_t p) { return size_t(p>=0x40000000 ? p-0x40000000 : p); }
extern "C" uint8_t vm_read8(uint64_t p) { return memory.at(offset(p)); }
extern "C" uint32_t vm_read32(uint64_t p) { return uint32_t(vm_read8(p))<<24 | uint32_t(vm_read8(p+1))<<16 | uint32_t(vm_read8(p+2))<<8 | vm_read8(p+3); }
extern "C" void vm_write32(uint64_t p,uint32_t value) { for (int i=0;i<4;++i) memory.at(offset(p+i))=uint8_t(value>>(24-i*8)); }
static void page(const char* name) {
    std::fill_n(memory.begin()+0x23000,16,0); std::memcpy(memory.data()+0x23000,name,std::strlen(name));
}
int main() {
    ppu_context ctx{}; ctx.thread_id=1;
    vm_write32(0xf48dd4,0x40020000); vm_write32(0x40020048,0x40021000);
    vm_write32(0x40021050,1); vm_write32(0x40021054,0x40023000); vm_write32(0x4002128c,1);
    page("fefl_KS_page2"); haze_quit_guest_page(&ctx);
    auto offer=haze_quit_offer(); assert(offer.available && !offer.gameplay && offer.generation);
    vm_write32(0x40020048,0); // Temporary current-context slot is normally cleared.
    haze_quit_guest_tick(&ctx); assert(haze_quit_offer().generation==offer.generation);
    assert(haze_quit_request(false,offer.generation)); auto generation=haze_quit_state().generation;
    assert(haze_quit_confirm(generation));
    audio.phase=HazeAudioPhase::Saving; haze_quit_guest_tick(&ctx); assert(!haze_quit_guest_parked());
    audio.phase=HazeAudioPhase::Ready; vm_write32(0x137a390,1); haze_quit_guest_tick(&ctx); assert(!haze_quit_guest_parked());
    vm_write32(0x137a390,0);
    auto other=ctx; other.thread_id=2; haze_quit_guest_tick(&other); assert(!haze_quit_guest_parked());
    const auto saved=ctx;
    std::thread guest([&] { haze_quit_guest_tick(&ctx); });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (!haze_quit_guest_parked() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    assert(haze_quit_guest_parked()); assert(haze_save_begin(0xe71ac8)==HAZE_SAVE_REJECTED);
    assert(haze_quit_cancel(generation)); guest.join();
    assert(!haze_quit_guest_parked() && !std::memcmp(&saved,&ctx,sizeof ctx));
    auto ticket=haze_save_begin(0xe71ac8); assert(ticket!=HAZE_SAVE_REJECTED); haze_save_finished(ticket,0);
    page("ingm_DC_page1"); haze_quit_guest_tick(&ctx);
    assert(haze_quit_offer().gameplay && haze_quit_offer().generation!=offer.generation);
    offer=haze_quit_offer(); assert(haze_quit_request(true,offer.generation)); generation=haze_quit_state().generation;
    assert(haze_quit_confirm(generation));
    page("sett_DC_page2"); haze_quit_guest_tick(&ctx);
    assert(!haze_quit_offer().available && haze_quit_state().phase==HazeQuitPhase::Failed);
    assert(haze_quit_cancel(generation));
    vm_write32(0x4002128c,0); haze_quit_guest_tick(&ctx); assert(!haze_quit_offer().available);
    std::puts("Quit guest bridge: live main/pause identity, save/settings wait, owner-thread parking, cancellation and context preservation passed.");
}
