#include "ppu_recomp.h"
#include "haze_menu.h"
#include "haze_audio_settings.h"
#include "haze_quit.h"
#include <cstring>
#include <cstdio>

void haze_native_menu_page(ppu_context*,uint32_t);

void haze_menu_page_loaded(ppu_context* ctx,uint32_t page) {
    if (!page || page>UINT32_MAX-0x28) return;
    // Supported BLUS30094 page activator. Page IDs are fixed 16-byte strings,
    // copied by the guest's ID constructor at 0x7555b0, not host pointers.
    const uint32_t code[]={0x2f840000,0x7c0802a6,0xf821ff41,0xfb010080,
        0xfb210088,0xfbc100b0,0xfbe100b8,0xfb410090};
    for (unsigned i=0;i<8;++i) if (vm_read32(0x319c68+i*4)!=code[i]) return;
    haze_quit_guest_page(ctx);
    haze_native_menu_page(ctx,page);
    char name[17]{};
    for (unsigned i=0;i<16;++i) name[i]=char(vm_read8(page+0x18+i));
    HazeMenuPage kind=HazeMenuPage::None;
    if (!std::strcmp(name,"sett_DC_page10")) kind=HazeMenuPage::Video;
    if (!std::strcmp(name,"sett_DC_page3")) kind=HazeMenuPage::Audio;
    if (!std::strcmp(name,"sett_DC_page2")) kind=HazeMenuPage::Controls;
    if (kind==HazeMenuPage::Audio && !haze_audio_guest_open(ctx)) {
        std::fprintf(stderr,"[menu] Audio profile unavailable; retaining retail page\n"); return;
    }
    if (kind!=HazeMenuPage::None)
        std::fprintf(stderr,"[menu] Guest page %s; PC provider=%s\n",name,haze_menu_request(kind) ? "yes":"no");
}
