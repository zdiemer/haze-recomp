#include "ppu_recomp.h"
#include "haze_quit.h"
#include "haze_audio_settings.h"
#include <atomic>
#include <mutex>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace quit_guest {
std::mutex mutex;
HazeQuitOffer offer;
std::atomic<bool> parked=false;
#ifdef _WIN32
uint64_t owner=0;
uint32_t page_context=0;
char last_page[17]{};
bool pointer(uint32_t p) { return p>=0x10000 && p<0xc0000000 && !(p&3); }
void observe() {
    char page[17]{};
    if (pointer(page_context)) {
        const auto count=vm_read32(page_context+0x50),stack=vm_read32(page_context+0x54);
        if (vm_read32(page_context+0x28c) && count && count<=64 && pointer(stack) && stack<=0xbffffc00)
            for (unsigned i=0;i<16;++i) page[i]=char(vm_read8(stack+(count-1)*16+i));
    }
    std::lock_guard lock(mutex);
    if (std::memcmp(page,last_page,16)) {
        std::memcpy(last_page,page,17); ++offer.generation;
    }
    offer.gameplay=!std::strcmp(page,"ingm_DC_page1");
    offer.available=offer.gameplay || !std::strcmp(page,"fefl_KS_page2");
}
#endif
}
HazeQuitOffer haze_quit_offer() {
    std::lock_guard lock(quit_guest::mutex); return quit_guest::offer;
}
bool haze_quit_guest_parked() { return quit_guest::parked.load(); }
void haze_quit_guest_page(ppu_context* ctx) {
#ifdef _WIN32
    if (!ctx || (quit_guest::owner && ctx->thread_id!=quit_guest::owner)) return;
    const auto manager=vm_read32(0xf48dd4);
    if (!quit_guest::pointer(manager)) return;
    const auto context=vm_read32(manager+0x48);
    if (!quit_guest::pointer(context)) return;
    // Called only by the audited page activation hook. Keep this context while
    // the GUI's temporary current-context slot is cleared between dispatches.
    quit_guest::owner=ctx->thread_id;
    if (context!=quit_guest::page_context) {
        std::lock_guard lock(quit_guest::mutex); ++quit_guest::offer.generation;
    }
    quit_guest::page_context=context; quit_guest::observe();
#else
    (void)ctx;
#endif
}
void haze_quit_guest_tick(ppu_context* ctx) {
#ifdef _WIN32
    if (!ctx || !quit_guest::owner || ctx->thread_id!=quit_guest::owner) return;
    quit_guest::observe();
    const auto offer=haze_quit_offer();
    const auto audio=haze_audio_state();
    if (!haze_quit_poll(offer.available ? offer.generation : 0,vm_read32(0x137a390)!=0,
        audio.phase==HazeAudioPhase::Queued || audio.phase==HazeAudioPhase::Saving)) return;
    // Hold the producer at the audited common frame boundary. Rendering and
    // the window thread continue; cancellation releases the unchanged frame.
    quit_guest::parked=true;
    for (;;) {
        const auto phase=haze_quit_state().phase;
        if (phase!=HazeQuitPhase::Ready && phase!=HazeQuitPhase::Stopping) break;
        Sleep(10);
    }
    quit_guest::parked=false;
#else
    (void)ctx;
#endif
}
