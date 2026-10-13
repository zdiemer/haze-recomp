#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <fstream>
#include <iterator>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
static ULONGLONG ticks=0;
static ULONGLONG test_time() { return ticks; }
#define GetTickCount64 test_time
#include "../port/haze_prompts.cpp"
static rsx_live_texture_revision_fn registered_revision;
static rsx_live_texture_pixels_fn registered_pixels;
extern "C" void rsx_live_draw_set_texture_override(rsx_live_texture_revision_fn r,rsx_live_texture_pixels_fn p,void*) {
    registered_revision=r; registered_pixels=p;
}
static void refresh(HazeInputFamily family) { ticks+=501; haze_prompts_update(family); }
static void write_setting(const char* section,const char* key,const char* value) {
    assert(WritePrivateProfileStringA(section,key,value,config.c_str()));
    // Force distinguishable timestamps even on coarse filesystems.
    fs::last_write_time(config,fs::file_time_type::clock::now()+std::chrono::seconds(ticks));
}
static bool empty_slot(const Atlas& atlas,int slot) {
    for (int y=slot/6*60;y<slot/6*60+60;y++) for (int x=slot%6*64;x<slot%6*64+64;x++)
        if (atlas[(size_t(y)*width+x)*4+3]) return false;
    return true;
}
int main(int argc,char** argv) {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT);
    assert(argc>=2);
    const fs::path repo=fs::absolute(argv[1]);
    const auto temp=repo/"local/prompt-test";
    fs::create_directories(temp);
    const auto ini=temp/"input.ini";
    { std::ofstream out(ini); out << "[Prompts]\nDirectory=" << (repo/"assets/input-prompts").string() << '\n'; }
    haze_prompts_initialize(ini.string().c_str());
    assert(registered_revision && registered_pixels);
    assert(published && !empty_slot(*published,12));
    const auto keyboard=published;
    // Filtering a transparent blue neighbor must not darken or tint red edges.
    Image edge{2,1,{255,0,0,255,0,0,255,0}};
    Atlas filtered{};
    place(filtered,edge,0,0,0,1,1);
    assert(filtered[0]==255 && filtered[1]==0 && filtered[2]==0 && filtered[3]==128);
    assert(registered_revision(0x85,384,240,nullptr)==0); // Unrelated RGBA.
    assert(registered_revision(0x88,64,64,nullptr)==0);
    assert(registered_revision(0xa8,384,240,nullptr)>0); // Linear BC3.
    assert(registered_revision(0x88,384,240,nullptr)>0);
    std::vector<u8> unrelated(92160);
    assert(!registered_pixels(0x88,384,240,unrelated.data(),unrelated.size(),nullptr));
    auto rev=generation.load();
    refresh(HazeInputFamily::Keyboard); assert(generation==rev); // Stable cache.
    for (auto family:{HazeInputFamily::Xbox,HazeInputFamily::PlayStation,HazeInputFamily::Switch}) {
        refresh(family); assert(published && *published!=*keyboard);
        for (int i=0;i<24;i++) assert(!empty_slot(*published,i));
    }
    assert(pad_icon(HazeInputFamily::Switch,"south")=="switch_button_b");
    assert(pad_icon(HazeInputFamily::Xbox,"start")=="xbox_button_menu");
    assert(pad_icon(HazeInputFamily::Xbox,"back")=="xbox_button_view");
    assert(pad_icon(HazeInputFamily::PlayStation,"start")=="playstation3_button_start");
    // The title's {E} token addresses Start (slot 8), and must refresh while
    // the title remains open, without changing any other keyboard binding.
    refresh(HazeInputFamily::Keyboard);
    const auto enter_start=published;
    write_setting("Bindings","Start","J"); refresh(HazeInputFamily::Keyboard);
    assert(published && !empty_slot(*published,8) && *published!=*enter_start);
    for (int y=0;y<int(height);y++) for (int x=0;x<int(width);x++) {
        if (x>=128 && x<192 && y>=60 && y<120) continue;
        for (int c=0;c<4;c++) assert((*published)[(y*width+x)*4+c]==(*enter_start)[(y*width+x)*4+c]);
    }
    write_setting("Bindings","Start","None"); refresh(HazeInputFamily::Keyboard);
    assert(published && empty_slot(*published,8));
    write_setting("Bindings","Start","Enter"); refresh(HazeInputFamily::Keyboard);
    assert(published && *published==*enter_start);
    for (auto family:{HazeInputFamily::Xbox,HazeInputFamily::PlayStation,HazeInputFamily::Switch}) {
        write_setting("ControllerBindings","Start","start"); refresh(family);
        const auto original_start=published;
        write_setting("ControllerBindings","Start","north"); refresh(family);
        assert(published && !empty_slot(*published,8) && *published!=*original_start);
    }
    write_setting("ControllerBindings","Start","start");
    write_setting("Bindings","Cross","None"); refresh(HazeInputFamily::Keyboard);
    assert(published && empty_slot(*published,12));
    write_setting("Bindings","Cross","Mouse2"); refresh(HazeInputFamily::Keyboard);
    assert(published && !empty_slot(*published,12) && *published!=*keyboard);
    write_setting("ControllerBindings","Cross","north"); refresh(HazeInputFamily::Xbox);
    const auto rebound=published; assert(rebound);
    write_setting("ControllerBindings","Cross","south"); refresh(HazeInputFamily::Xbox);
    assert(published && *published!=*rebound);
    // Copy a complete user icon pack so the vendored files remain untouched.
    const auto pack=temp/"icons";
    fs::create_directories(pack);
    fs::copy(repo/"assets/input-prompts/keyboard",pack/"keyboard",fs::copy_options::recursive|fs::copy_options::overwrite_existing);
    write_setting("Prompts","Directory",pack.string().c_str()); refresh(HazeInputFamily::Keyboard);
    const auto before=published; assert(before);
    const auto icon=pack/"keyboard/mouse_right.png";
    const auto preserved_time=fs::last_write_time(icon);
    fs::copy_file(pack/"keyboard/keyboard_a.png",icon,fs::copy_options::overwrite_existing);
    fs::last_write_time(icon,preserved_time); // Windows copies often preserve it.
    refresh(HazeInputFamily::Keyboard); assert(published && *published!=*before);
    { std::ofstream out(icon,std::ios::binary|std::ios::trunc); out << "malformed PNG"; }
    fs::last_write_time(icon,fs::file_time_type::clock::now()+std::chrono::seconds(ticks));
    refresh(HazeInputFamily::Keyboard); assert(!published);
    fs::copy_file(repo/"assets/input-prompts/keyboard/mouse_right.png",icon,fs::copy_options::overwrite_existing);
    refresh(HazeInputFamily::Keyboard); assert(published);
    fs::remove(icon);
    refresh(HazeInputFamily::Keyboard); assert(!published);
    fs::copy_file(repo/"assets/input-prompts/keyboard/mouse_right.png",icon);
    refresh(HazeInputFamily::Keyboard); assert(published);
    write_setting("Prompts","Enabled","0"); refresh(HazeInputFamily::Keyboard); assert(!published);
    write_setting("Prompts","Enabled","1"); refresh(HazeInputFamily::Keyboard); assert(published);
    // Optional retail fixture stays local and is never committed.
    if (argc>=3) {
        std::ifstream stream(argv[2],std::ios::binary);
        std::vector<u8> source{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
        assert(source.size()==92160);
        assert(registered_pixels(0x88,384,240,source.data(),source.size(),nullptr));
        source[0]^=1;
        assert(!registered_pixels(0x88,384,240,source.data(),source.size(),nullptr));
        puts("Retail fingerprint: exact original accepted; one-byte change rejected.");
    }
    puts("Prompt PNG decoding, all families, rebinds, cache revision, file hot reload and malformed fallback passed.");
}
