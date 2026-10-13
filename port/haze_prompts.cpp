#include "haze_prompts.h"
#include "haze_input_source.h"
#include "rsx_live_draw.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
constexpr unsigned width=384, height=240;
using Atlas=std::array<u8,width*height*4>;
std::shared_ptr<const Atlas> published;
std::atomic<u64> generation{1};
std::string config;
HazeInputFamily current=HazeInputFamily::Keyboard;
bool first=true;
ULONGLONG next_check=0;
struct FileStamp {
    fs::file_time_type time;
    uintmax_t bytes=0;
    u64 hash=0;
    bool operator==(const FileStamp&) const=default;
};
std::vector<std::pair<fs::path,FileStamp>> watched;

std::string setting(const char* section,const char* key,const char* fallback) {
    char value[1024];
    GetPrivateProfileStringA(section,key,fallback,value,sizeof(value),config.c_str());
    std::string result=value;
    for (auto& c:result) c=(char)std::tolower((unsigned char)c);
    return result;
}
FileStamp stamp(const fs::path& path) {
    std::error_code ec;
    FileStamp value{fs::last_write_time(path,ec)};
    if (ec) return value;
    value.bytes=fs::file_size(path,ec);
    if (ec || value.bytes>16*1024*1024) return value;
    // Copy tools can preserve timestamps. Used 64px PNGs are tiny, so a
    // bounded content check twice a second also catches those replacements.
    std::ifstream stream(path,std::ios::binary);
    char buffer[4096];
    value.hash=14695981039346656037ull;
    while (stream.read(buffer,sizeof(buffer)) || stream.gcount())
        for (std::streamsize i=0;i<stream.gcount();i++) { value.hash^=(u8)buffer[i]; value.hash*=1099511628211ull; }
    return value;
}
bool descriptor(u32 format,u32 w,u32 h) {
    return w==width && h==height && (format&0x9f)==0x88; // BC3, linear or swizzled
}
u64 revision(u32 format,u32 w,u32 h,void*) {
    return descriptor(format,w,h) ? generation.load(std::memory_order_acquire) : 0;
}
u64 fingerprint(const u8* bytes,size_t count) {
    u64 hash=14695981039346656037ull;
    for (size_t i=0;i<count;i++) { hash^=bytes[i]; hash*=1099511628211ull; }
    return hash;
}
const u8* pixels(u32 format,u32 w,u32 h,const u8* source,size_t bytes,void*) {
    if (!descriptor(format,w,h) || bytes!=92160 || !source ||
        fingerprint(source,bytes)!=0x1679f0984072814bull) return nullptr;
    // Keep this snapshot alive through the renderer's immediate upload.
    thread_local std::shared_ptr<const Atlas> held;
    held=std::atomic_load(&published);
    static bool reported=false;
    if (held && !reported) {
        fprintf(stderr,"[prompts] Identified and replaced padbuttons atlas (complete BC3 fingerprint)\n");
        reported=true;
    }
    return held ? held->data() : nullptr;
}
std::string key_icon(std::string name) {
    if (name=="none" || name.empty()) return {};
    if (name.size()==1 && std::isalnum((unsigned char)name[0])) return "keyboard_"+name;
    if (name=="backspace" || name=="delete" || name=="insert" || name=="home" || name=="end" ||
        name=="f2" || name=="f3" || name=="f4" || name=="f5" || name=="f7" || name=="f8" || name=="f9" ||
        name=="f10" || name=="f11" || name=="f12") return "keyboard_"+name;
    const std::pair<const char*,const char*> aliases[]={
        {"space","keyboard_space"},{"enter","keyboard_enter"},{"escape","keyboard_escape"},
        {"tab","keyboard_tab"},{"shift","keyboard_shift"},{"ctrl","keyboard_ctrl"},{"alt","keyboard_alt"},
        {"up","keyboard_arrow_up"},{"down","keyboard_arrow_down"},
        {"left","keyboard_arrow_left"},{"right","keyboard_arrow_right"},
        {"pageup","keyboard_page_up"},{"pagedown","keyboard_page_down"},
        {"mouse1","mouse_left"},{"mouse2","mouse_right"},{"mouse3","mouse_scroll"},
        {"mouse4","mouse_side_back"},{"mouse5","mouse_side_forward"}};
    for (const auto& [from,to]:aliases) if (name==from) return to;
    return {}; // Same behavior as disabled unknown input bindings.
}
std::string pad_icon(HazeInputFamily family,std::string name) {
    if (name=="none" || name.empty()) return {};
    if (haze_raw_source(name).kind!=HazeRawSource::None) return family==HazeInputFamily::Xbox ? "controller_xboxseries" : family==HazeInputFamily::Switch ? "controller_switch_pro" : "controller_playstation3";
    if (name[0]=='-' || name[0]=='+') name.erase(0,1);
    if (name=="a") name="south";
    if (name=="b") name="east";
    if (name=="x") name="west";
    if (name=="y") name="north";
    const char* positions[]={"south","east","west","north","leftshoulder","rightshoulder",
        "lefttrigger","righttrigger","leftstick","rightstick","start","back","dpup","dpdown","dpleft","dpright"};
    const char* xbox[]={"button_a","button_b","button_x","button_y","lb","rb","lt","rt",
        "stick_l_press","stick_r_press","button_start","button_back","dpad_up","dpad_down","dpad_left","dpad_right"};
    const char* ps[]={"button_cross","button_circle","button_square","button_triangle","trigger_l1","trigger_r1","trigger_l2","trigger_r2",
        "stick_l_press","stick_r_press","3_button_start","3_button_select","dpad_up","dpad_down","dpad_left","dpad_right"};
    const char* sw[]={"button_b","button_a","button_y","button_x","button_l","button_r","button_zl","button_zr",
        "stick_l_press","stick_r_press","button_plus","button_minus","dpad_up","dpad_down","dpad_left","dpad_right"};
    const std::string prefix=family==HazeInputFamily::Xbox ? "xbox_" : family==HazeInputFamily::Switch ? "switch_" : "playstation_";
    for (int i=0;i<16;i++) if (name==positions[i]) {
        if (family==HazeInputFamily::PlayStation && (i==10 || i==11)) return "playstation"+std::string(ps[i]);
        return prefix+(family==HazeInputFamily::Xbox ? xbox[i] : family==HazeInputFamily::Switch ? sw[i] : ps[i]);
    }
    if (name=="leftx" || name=="lefty") return prefix+"stick_l";
    if (name=="rightx" || name=="righty") return prefix+"stick_r";
    // Less common SDL buttons still get an honest generic controller glyph.
    if (name=="guide" || name=="touchpad" ||
        (name.size()==5 && name.starts_with("misc") && name[4]>='1' && name[4]<='6') ||
        (name.size()==7 && name.starts_with("paddle") && name[6]>='1' && name[6]<='4'))
        return family==HazeInputFamily::Xbox ? "controller_xboxseries" : family==HazeInputFamily::Switch ? "controller_switch_pro" : "controller_playstation3";
    return {};
}
struct Image { unsigned w=0,h=0; std::vector<u8> rgba; };
bool decode(IWICImagingFactory* factory,const fs::path& path,Image& image) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)) ||
        FAILED(decoder->GetFrame(0,&frame)) || FAILED(frame->GetSize(&image.w,&image.h)) ||
        !image.w || !image.h || image.w>2048 || image.h>2048 ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) return false;
    image.rgba.resize(size_t(image.w)*image.h*4);
    return SUCCEEDED(converter->CopyPixels(nullptr,image.w*4,(UINT)image.rgba.size(),image.rgba.data()));
}
void place(Atlas& atlas,const Image& image,int slot,int ox=4,int oy=4,int bound_w=56,int bound_h=52) {
    // Retail UVs address this 6x4 layout. Keep a gutter to avoid bilinear bleed.
    const double scale=std::min(double(bound_w)/image.w,double(bound_h)/image.h);
    const int w=std::max(1,int(image.w*scale)),h=std::max(1,int(image.h*scale));
    const int x=(slot%6)*64+ox+(bound_w-w)/2, y=(slot/6)*60+oy+(bound_h-h)/2;
    for (int dy=0;dy<h;dy++) for (int dx=0;dx<w;dx++) {
        const double sx=std::clamp((dx+.5)*image.w/w-.5,0.0,double(image.w-1));
        const double sy=std::clamp((dy+.5)*image.h/h-.5,0.0,double(image.h-1));
        const unsigned ix=(unsigned)sx,iy=(unsigned)sy;
        const double fx=sx-ix,fy=sy-iy;
        double alpha=0,color[3]{};
        for (unsigned j=0;j<2;j++) for (unsigned i=0;i<2;i++) {
            const auto* source=image.rgba.data()+(size_t(std::min(iy+j,image.h-1))*image.w+std::min(ix+i,image.w-1))*4;
            const double weight=(i ? fx : 1-fx)*(j ? fy : 1-fy);
            alpha+=source[3]*weight;
            for (int c=0;c<3;c++) color[c]+=source[c]*source[3]*weight;
        }
        auto* target=atlas.data()+(size_t(y+dy)*width+x+dx)*4;
        for (int c=0;c<3;c++) target[c]=alpha>0 ? (u8)std::clamp(int(color[c]/alpha+.5),0,255) : 0;
        target[3]=(u8)std::clamp(int(alpha+.5),0,255);
    }
}
void rebuild(HazeInputFamily family) {
    watched.clear(); watched.emplace_back(config,stamp(config));
    std::shared_ptr<Atlas> atlas;
    if (GetPrivateProfileIntA("Prompts","Enabled",1,config.c_str())) {
        // Preserve path case; only binding names are case-insensitive.
        char directory[1024];
        GetPrivateProfileStringA("Prompts","Directory","../assets/input-prompts",directory,sizeof(directory),config.c_str());
        const fs::path root=fs::path(config).parent_path()/directory;
        const char* folder=family==HazeInputFamily::Keyboard ? "keyboard" : family==HazeInputFamily::Xbox ? "xbox" : family==HazeInputFamily::Switch ? "switch" : "playstation";
        std::array<std::string,24> icons;
        const char* names[]={"L2","R2","Select","Start","L1","R1","Cross","Square","Circle","Triangle","L3","R3","Left","Right","Up","Down"};
        const char* keyboard[]={"q","f","tab","enter","g","mouse1","space","e","x","r","ctrl","mouse2","left","right","up","down"};
        const char* gamepad[]={"lefttrigger","righttrigger","back","start","leftshoulder","rightshoulder","south","west","east","north","leftstick","rightstick","dpleft","dpright","dpup","dpdown"};
        const int slots[]={4,5,7,8,10,11,12,13,14,15,16,17,18,19,20,21};
        const bool kb=family==HazeInputFamily::Keyboard;
        for (int i=0;i<16;i++) {
            const auto value=setting(kb ? "Bindings" : "ControllerBindings",names[i],kb ? keyboard[i] : gamepad[i]);
            icons[slots[i]]=kb ? key_icon(value) : pad_icon(family,value);
        }
        if (kb) {
            // Compose movement from its four actual bindings, rather than a fixed WASD label.
            icons[0]="@movement"; icons[1]="mouse_move";
            icons[2]="@vertical";
            icons[3]="mouse_vertical";
            icons[6]="@horizontal";
            icons[9]="mouse_horizontal";
            icons[22]="keyboard_escape"; icons[23]="@dpad";
        } else {
            const std::string prefix=family==HazeInputFamily::Xbox ? "xbox_" : family==HazeInputFamily::Switch ? "switch_" : "playstation_";
            icons[0]=pad_icon(family,setting("Controller","MoveX","leftx"));
            icons[1]=pad_icon(family,setting("Controller","LookX","rightx"));
            icons[2]=pad_icon(family,setting("Controller","MoveY","lefty"));
            icons[3]=pad_icon(family,setting("Controller","LookY","righty"));
            icons[6]=icons[0]; icons[9]=icons[1];
            icons[22]=family==HazeInputFamily::Xbox ? "xbox_guide" : family==HazeInputFamily::Switch ? "switch_button_home" : "controller_playstation3";
            icons[23]=prefix+"dpad_none";
        }
        const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        ComPtr<IWICImagingFactory> factory;
        bool valid=SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
        atlas=std::make_shared<Atlas>();
        auto load=[&](const std::string& name,Image& image) {
            const auto path=root/folder/(name+".png");
            watched.emplace_back(path,stamp(path));
            if (!factory || !decode(factory.Get(),path,image)) {
                fprintf(stderr,"[prompts] Cannot decode %s; using original game prompts\n",path.string().c_str());
                return false;
            }
            return true;
        };
        for (int slot=0;slot<24;slot++) {
            if (icons[slot].empty()) continue;
            if (icons[slot][0]=='@') {
                const char* keys[]={"Forward","Backward","StrafeLeft","StrafeRight"};
                const char* defaults[]={"w","s","a","d"};
                int count=4;
                if (icons[slot]=="@dpad") {
                    keys[0]="Up"; keys[1]="Down"; keys[2]="Left"; keys[3]="Right";
                    defaults[0]="up"; defaults[1]="down"; defaults[2]="left"; defaults[3]="right";
                } else if (icons[slot]=="@vertical") count=2;
                else if (icons[slot]=="@horizontal") {
                    keys[0]="StrafeLeft"; keys[1]="StrafeRight";
                    defaults[0]="a"; defaults[1]="d"; count=2;
                }
                for (int i=0;i<count;i++) {
                    const auto name=key_icon(setting("Bindings",keys[i],defaults[i]));
                    if (name.empty()) continue;
                    Image image;
                    if (!load(name,image)) { valid=false; continue; }
                    // Actual directional bindings, including paired axis hints.
                    const bool horizontal=icons[slot]=="@horizontal";
                    const int w=count==4 ? 20 : 28, h=count==4 ? 24 : 28;
                    const int ox=count==4 ? (i==2 ? 2 : i==3 ? 42 : 22) : horizontal ? (i ? 34 : 2) : 18;
                    const int oy=count==4 ? (i==0 ? 2 : 32) : horizontal ? 16 : (i ? 32 : 0);
                    place(*atlas,image,slot,ox,oy,w,h);
                }
                continue;
            }
            Image image;
            if (load(icons[slot],image)) place(*atlas,image,slot); else valid=false;
        }
        factory.Reset();
        if (SUCCEEDED(initialized)) CoUninitialize();
        if (!valid) atlas.reset();
    }
    std::atomic_store(&published,std::shared_ptr<const Atlas>(atlas));
    const auto value=generation.fetch_add(1,std::memory_order_release)+1;
    fprintf(stderr,"[prompts] %s atlas %s; revision=%llu\n",
        family==HazeInputFamily::Keyboard ? "keyboard" : family==HazeInputFamily::Xbox ? "Xbox" : family==HazeInputFamily::Switch ? "Switch" : "PlayStation",
        atlas ? "ready" : "original fallback",(unsigned long long)value);
}
}
void haze_prompts_initialize(const char* path) {
    config=path; first=true; next_check=0;
    rsx_live_draw_set_texture_override(revision,pixels,nullptr);
    haze_prompts_update(HazeInputFamily::Keyboard);
}
void haze_prompts_update(HazeInputFamily family) {
    const auto now=GetTickCount64();
    bool changed=first || family!=current;
    if (now>=next_check) {
        next_check=now+500;
        for (const auto& [path,time]:watched) if (!(stamp(path)==time)) { changed=true; break; }
    }
    if (changed) { first=false; current=family; rebuild(family); }
}
static bool draw_icon(void* context,const std::shared_ptr<const Atlas>& atlas,int slot,int x,int y,int size) {
    if (!atlas || slot<0 || slot>=24) return false;
    std::array<unsigned char,64*60*4> bgra{};
    for (int py=0;py<60;++py) for (int px=0;px<64;++px) {
        const size_t source=((slot/6*60+py)*width+slot%6*64+px)*4;
        const size_t target=(py*64+px)*4;
        const unsigned alpha=(*atlas)[source+3];
        for (int channel=0;channel<3;++channel)
            bgra[target+channel]=static_cast<unsigned char>(((*atlas)[source+2-channel]*alpha+(channel==2 ? 18:20)*(255-alpha))/255);
    }
    BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth=64; bitmap.bmiHeader.biHeight=-60;
    bitmap.bmiHeader.biPlanes=1; bitmap.bmiHeader.biBitCount=32;
    bitmap.bmiHeader.biCompression=BI_RGB;
    StretchDIBits((HDC)context,x,y,size,size,0,0,64,60,bgra.data(),&bitmap,DIB_RGB_COLORS,SRCCOPY);
    return true;
}
bool haze_prompts_draw_icon(void* context,int slot,int x,int y,int size) {
    return draw_icon(context,std::atomic_load(&published),slot,x,y,size);
}
bool haze_prompts_draw_binding(void* context,HazeInputFamily family,const char* binding,int x,int y,int size) {
    if (!GetPrivateProfileIntA("Prompts","Enabled",1,config.c_str())) return false;
    std::string value=binding; for (auto& c:value) c=char(std::tolower((unsigned char)c));
    const auto icon=family==HazeInputFamily::Keyboard ? key_icon(value) : pad_icon(family,value);
    if (icon.empty()) return false;
    char directory[1024]; GetPrivateProfileStringA("Prompts","Directory","../assets/input-prompts",directory,sizeof(directory),config.c_str());
    const char* folder=family==HazeInputFamily::Keyboard ? "keyboard" : family==HazeInputFamily::Xbox ? "xbox" : family==HazeInputFamily::Switch ? "switch" : "playstation";
    const auto path=fs::path(config).parent_path()/directory/folder/(icon+".png");
    struct Glyph { std::shared_ptr<const Atlas> atlas; FileStamp file; ULONGLONG next=0; };
    static std::map<fs::path,Glyph> cache;
    if (cache.size()>64) cache.clear();
    auto& glyph=cache[path]; const auto now=GetTickCount64();
    if (now>=glyph.next) {
        glyph.next=now+500; const auto file=stamp(path);
        if (!glyph.atlas || !(glyph.file==file)) {
            glyph.file=file; glyph.atlas.reset();
            const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            ComPtr<IWICImagingFactory> factory; Image image;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))) && decode(factory.Get(),path,image)) {
                auto atlas=std::make_shared<Atlas>(); atlas->fill(0); place(*atlas,image,0); glyph.atlas=atlas;
            }
            factory.Reset(); if (SUCCEEDED(initialized)) CoUninitialize();
        }
    }
    return draw_icon(context,glyph.atlas,0,x,y,size);
}
