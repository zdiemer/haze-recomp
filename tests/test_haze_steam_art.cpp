#include "../port/haze_steam_art.cpp"
#include <cassert>
#include <fstream>
#include <cstdio>
namespace fs=std::filesystem;
static std::string bytes(const fs::path& path) { std::ifstream f(path,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
static void put(const fs::path& path,const std::string& value) { std::ofstream f(path,std::ios::binary); f<<value; }
static uint32_t be32(const std::string& text,size_t offset) {
    uint32_t v=0; for (unsigned i=0;i<4;++i) v=(v<<8)|static_cast<unsigned char>(text.at(offset+i)); return v;
}
static void dimensions(const fs::path& path,unsigned width,unsigned height) {
    const auto data=bytes(path); assert(data.substr(0,8)==std::string("\x89PNG\r\n\x1a\n",8));
    assert(be32(data,16)==width && be32(data,20)==height);
}
int main(int argc,char** argv) {
    if (argc==3 && std::string(argv[1])=="--preview") {
        const auto config=fs::absolute("local/steam-art-preview"); fs::create_directories(config);
        const auto result=haze_steam_artwork(config,1234567890,fs::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2]))));
        std::printf("Artwork preview: %u installed, %u preserved. %s\n",result.installed,result.preserved,result.warning.c_str());
        return result.warning.empty() ? 0 : 1;
    }
    const auto root=fs::absolute("local/steam-art-test-"+std::to_string(GetCurrentProcessId()));
    const auto config=root/L"config",source=root/L"disc"; fs::create_directories(config); fs::create_directories(source);
    steam_art::ComScope com; steam_art::ComPtr<IWICImagingFactory> factory;
    steam_art::check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    auto logo=steam_art::canvas(20,10,true);
    for (unsigned y=4;y<6;++y) for (unsigned x=5;x<15;++x) { const auto i=(y*20+x)*4; logo.pixels[i+2]=128; logo.pixels[i+3]=128; }
    steam_art::encode(factory.Get(),logo,source/L"ICON0.PNG");
    auto background=steam_art::canvas(32,16); for (size_t i=0;i<background.pixels.size();i+=4) background.pixels[i+2]=255;
    steam_art::encode(factory.Get(),background,source/L"PIC0.PNG"); steam_art::encode(factory.Get(),background,source/L"PIC1.PNG");
    auto character=steam_art::canvas(10,20); for (size_t i=0;i<character.pixels.size();i+=4) character.pixels[i]=255;
    steam_art::encode(factory.Get(),character,source/L"PIC2.PNG");
    const auto result=haze_steam_artwork(config,0x80000042u,source);
    assert(result.installed==4 && result.preserved==0 && result.warning.empty());
    const auto grid=config/L"grid"; const auto prefix=std::to_wstring(0x80000042u);
    dimensions(grid/(prefix+L"p.png"),600,900); dimensions(grid/(prefix+L".png"),920,430);
    dimensions(grid/(prefix+L"_hero.png"),1920,620); dimensions(grid/(prefix+L"_logo.png"),10,2);
    const auto hero=steam_art::load(factory.Get(),grid/(prefix+L"_hero.png"));
    // Red 2:1 source fits into the 1920x620 canvas at 1240x620, centered with
    // black side bars. Exact sampled pixels check proportions and premultiply.
    assert(hero.pixels[2]==0 && hero.pixels[(310*1920+960)*4+2]==255);
    const auto saved_logo=steam_art::load(factory.Get(),grid/(prefix+L"_logo.png"));
    assert(saved_logo.pixels[2]>=127 && saved_logo.pixels[2]<=128 && saved_logo.pixels[3]==128);
    const auto cover_before=bytes(grid/(prefix+L"p.png"));
    const auto repeat=haze_steam_artwork(config,0x80000042u,source);
    assert(repeat.installed==0 && repeat.preserved==4 && repeat.warning.empty() && bytes(grid/(prefix+L"p.png"))==cover_before);
    // Any existing extension and the older 64-bit ID naming are customization.
    const uint32_t second_id=0x80000043u; const auto second=std::to_wstring(second_id);
    const auto legacy=std::to_wstring((uint64_t(second_id)<<32)|0x02000000u);
    put(grid/(second+L"p.JPG"),"custom cover"); put(grid/(legacy+L"_hero.png"),"custom legacy hero");
    put(grid/L"other-game.png","unrelated");
    const auto custom=haze_steam_artwork(config,second_id,source);
    assert(custom.installed==2 && custom.preserved==2 && custom.warning.empty());
    assert(bytes(grid/(second+L"p.JPG"))=="custom cover" && !fs::exists(grid/(second+L"p.png")));
    assert(bytes(grid/(legacy+L"_hero.png"))=="custom legacy hero" && !fs::exists(grid/(second+L"_hero.png")));
    assert(bytes(grid/L"other-game.png")=="unrelated");
    const auto missing=haze_steam_artwork(config,0x80000044u,root/L"missing");
    assert(!missing.warning.empty() && !missing.installed);
    for (const auto& file:fs::directory_iterator(grid)) assert(file.path().extension()!=L".tmp");
    fs::remove_all(root);
    std::puts("Steam artwork: local PNG decoding, alpha, aspect-preserving composition, dimensions, custom/legacy preservation, retry and missing-source checks passed.");
}
