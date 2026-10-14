#include <cassert>
#include <cstring>
#include <vector>
#include "../port/haze_menu_guest.cpp"
std::vector<uint8_t> memory(0x320000);
HazeMenuPage received=HazeMenuPage::None;
unsigned requests;
bool haze_audio_guest_open(ppu_context*) { return true; }
void haze_quit_guest_page(ppu_context*) {}
void haze_native_menu_page(ppu_context*,uint32_t) {}
uint8_t vm_read8(uint64_t p) { return memory.at(size_t(p)); }
uint32_t vm_read32(uint64_t p) {
    return uint32_t(vm_read8(p))<<24 | uint32_t(vm_read8(p+1))<<16 | uint32_t(vm_read8(p+2))<<8 | vm_read8(p+3);
}
bool haze_menu_request(HazeMenuPage page) { received=page; ++requests; return true; }
int main() {
    const uint32_t code[]={0x2f840000,0x7c0802a6,0xf821ff41,0xfb010080,0xfb210088,0xfbc100b0,0xfbe100b8,0xfb410090};
    for (unsigned i=0;i<8;++i) for (unsigned j=0;j<4;++j) memory[0x319c68+i*4+j]=uint8_t(code[i]>>(24-j*8));
    for (auto [name,page]:{std::pair{"sett_DC_page10",HazeMenuPage::Video},std::pair{"sett_DC_page3",HazeMenuPage::Audio},std::pair{"sett_DC_page2",HazeMenuPage::Controls}}) {
        std::fill_n(memory.begin()+0x1018,16,0); std::memcpy(memory.data()+0x1018,name,std::strlen(name));
        haze_menu_page_loaded(nullptr,0x1000); assert(received==page);
    }
    assert(requests==3);
    std::memcpy(memory.data()+0x1018,"sett_DC_page20",14); haze_menu_page_loaded(nullptr,0x1000); assert(requests==3);
    haze_menu_page_loaded(nullptr,0); haze_menu_page_loaded(nullptr,UINT32_MAX); assert(requests==3);
    std::memcpy(memory.data()+0x1018,"sett_DC_page10",14); memory[0x319c68]=0;
    haze_menu_page_loaded(nullptr,0x1000); assert(requests==3);
    puts("Menu page hook: exact page identity, supported guest code and bounds passed.");
}
