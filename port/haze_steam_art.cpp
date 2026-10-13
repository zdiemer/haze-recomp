#include "haze_steam.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace steam_art {
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
struct ComScope {
    HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
void check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("Windows could not decode or encode the local artwork (HRESULT "+std::to_string(uint32_t(result))+")."); }
struct Picture { UINT width=0,height=0; std::vector<unsigned char> pixels; };
Picture canvas(UINT w,UINT h,bool transparent=false) {
    Picture p{w,h,std::vector<unsigned char>(size_t(w)*h*4,0)};
    if (!transparent) for (size_t i=3;i<p.pixels.size();i+=4) p.pixels[i]=255;
    return p;
}
Picture load(IWICImagingFactory* factory,const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path,ec)) throw std::runtime_error("Local artwork is missing: "+path.filename().string()+". Check the game-data folder in the launcher.");
    ComPtr<IWICBitmapDecoder> decoder; check(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder));
    ComPtr<IWICBitmapFrameDecode> frame; check(decoder->GetFrame(0,&frame));
    UINT w=0,h=0; check(frame->GetSize(&w,&h));
    if (!w || !h || w>8192 || h>8192 || uint64_t(w)*h>16*1024*1024)
        throw std::runtime_error("Local artwork dimensions are unsupported.");
    ComPtr<IWICFormatConverter> converter; check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    auto result=canvas(w,h,true); check(converter->CopyPixels(nullptr,w*4,UINT(result.pixels.size()),result.pixels.data())); return result;
}
Picture trim(const Picture& input) {
    UINT left=input.width,top=input.height,right=0,bottom=0;
    for (UINT y=0;y<input.height;++y) for (UINT x=0;x<input.width;++x) if (input.pixels[(size_t(y)*input.width+x)*4+3]) {
        left=std::min(left,x); top=std::min(top,y); right=std::max(right,x+1); bottom=std::max(bottom,y+1);
    }
    if (right<=left || bottom<=top) throw std::runtime_error("Local artwork is completely transparent.");
    auto result=canvas(right-left,bottom-top,true);
    for (UINT y=0;y<result.height;++y)
        std::copy_n(input.pixels.data()+((size_t(y+top)*input.width+left)*4),result.width*4,result.pixels.data()+size_t(y)*result.width*4);
    return result;
}
// Uniform scaling only. Letterbox the original composition inside its allotted
// rectangle; never stretch or crop the user's disc artwork.
void composite(IWICImagingFactory* factory,Picture& destination,const Picture& source,UINT x,UINT y,UINT w,UINT h) {
    const double scale=std::min(double(w)/source.width,double(h)/source.height);
    const UINT scaled_w=std::max(1u,UINT(std::floor(source.width*scale))),scaled_h=std::max(1u,UINT(std::floor(source.height*scale)));
    if (x+w>destination.width || y+h>destination.height) throw std::runtime_error("Invalid artwork composition.");
    ComPtr<IWICBitmap> bitmap;
    check(factory->CreateBitmapFromMemory(source.width,source.height,GUID_WICPixelFormat32bppPBGRA,source.width*4,
        UINT(source.pixels.size()),const_cast<BYTE*>(source.pixels.data()),&bitmap));
    ComPtr<IWICBitmapScaler> scaler; check(factory->CreateBitmapScaler(&scaler));
    check(scaler->Initialize(bitmap.Get(),scaled_w,scaled_h,WICBitmapInterpolationModeFant));
    auto scaled=canvas(scaled_w,scaled_h,true);
    check(scaler->CopyPixels(nullptr,scaled_w*4,UINT(scaled.pixels.size()),scaled.pixels.data()));
    const auto left=x+(w-scaled_w)/2,top=y+(h-scaled_h)/2;
    for (UINT sy=0;sy<scaled_h;++sy) for (UINT sx=0;sx<scaled_w;++sx) {
        const auto src=&scaled.pixels[(size_t(sy)*scaled_w+sx)*4];
        auto dst=&destination.pixels[(size_t(sy+top)*destination.width+sx+left)*4];
        for (unsigned c=0;c<4;++c) dst[c]=static_cast<unsigned char>(std::min(255u,unsigned(src[c])+(unsigned(dst[c])*(255-src[3])+127)/255));
    }
}
void encode(IWICImagingFactory* factory,const Picture& picture,const fs::path& path) {
    // CREATE_NEW reserves this unique sibling before WIC opens it. The caller
    // deletes it on failure and moves it without replacement only after close.
    HANDLE reserve=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (reserve==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create a staged library artwork file.");
    CloseHandle(reserve);
    try {
        {
            ComPtr<IWICStream> stream; check(factory->CreateStream(&stream)); check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
            ComPtr<IWICBitmapEncoder> encoder; check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));
            check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
            ComPtr<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(&frame,nullptr)); check(frame->Initialize(nullptr));
            check(frame->SetSize(picture.width,picture.height));
            WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA; check(frame->SetPixelFormat(&format));
            if (format!=GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG encoder does not support transparent artwork.");
            // Our buffers are premultiplied. Let WIC unpremultiply for PNG.
            ComPtr<IWICBitmap> source;
            check(factory->CreateBitmapFromMemory(picture.width,picture.height,GUID_WICPixelFormat32bppPBGRA,picture.width*4,
                UINT(picture.pixels.size()),const_cast<BYTE*>(picture.pixels.data()),&source));
            ComPtr<IWICFormatConverter> converter; check(factory->CreateFormatConverter(&converter));
            check(converter->Initialize(source.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
            check(frame->WriteSource(converter.Get(),nullptr)); check(frame->Commit()); check(encoder->Commit());
        }
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if (file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot finish the staged library artwork file.");
        const bool flushed=FlushFileBuffers(file)!=FALSE; CloseHandle(file);
        if (!flushed) throw std::runtime_error("Cannot flush the staged library artwork file.");
    } catch (...) { DeleteFileW(path.c_str()); throw; }
}
bool exists(const fs::path& directory,const std::wstring& stem,const std::wstring& legacy) {
    for (const auto& file:fs::directory_iterator(directory)) {
        const auto name=file.path().stem().wstring();
        if (!_wcsicmp(name.c_str(),stem.c_str()) || !_wcsicmp(name.c_str(),legacy.c_str())) return true;
    }
    return false;
}
}

HazeSteamArtworkResult haze_steam_artwork(const std::filesystem::path& config_directory,uint32_t appid,const std::filesystem::path& source_directory) {
    using namespace steam_art;
    HazeSteamArtworkResult result;
    if (source_directory.empty()) { result.warning="Library artwork is unavailable: select the local game-data folder in the launcher."; return result; }
    try {
        ComScope com;
        if (FAILED(com.result) && com.result!=RPC_E_CHANGED_MODE) check(com.result);
        ComPtr<IWICImagingFactory> factory; check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
        const auto directory=config_directory/L"grid";
        std::error_code ec; fs::create_directory(directory,ec);
        const auto attributes=GetFileAttributesW(directory.c_str());
        if (ec || attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Steam's artwork folder is unavailable or redirected. Existing artwork was left unchanged.");
        const std::wstring id=std::to_wstring(appid),legacy=std::to_wstring((uint64_t(appid)<<32)|0x02000000u);
        const wchar_t* suffixes[]={L"p",L"",L"_hero",L"_logo"};
        static std::atomic<unsigned> sequence{0};
        for (unsigned kind=0;kind<4;++kind) {
            const std::wstring stem=id+suffixes[kind];
            if (exists(directory,stem,legacy+suffixes[kind])) { ++result.preserved; continue; }
            fs::path temporary;
            try {
                Picture art;
                if (kind==0) {
                    art=canvas(600,900);
                    composite(factory.Get(),art,trim(load(factory.Get(),source_directory/L"ICON0.PNG")),40,40,520,190);
                    composite(factory.Get(),art,trim(load(factory.Get(),source_directory/L"PIC2.PNG")),30,260,540,600);
                } else if (kind==1) {
                    art=canvas(920,430);
                    composite(factory.Get(),art,load(factory.Get(),source_directory/L"PIC0.PNG"),0,0,920,430);
                } else if (kind==2) {
                    art=canvas(1920,620);
                    composite(factory.Get(),art,load(factory.Get(),source_directory/L"PIC1.PNG"),0,0,1920,620);
                } else art=trim(load(factory.Get(),source_directory/L"ICON0.PNG"));
                const auto candidate=directory/(L".haze-art-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                    std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++sequence)+L".tmp");
                encode(factory.Get(),art,candidate); temporary=candidate;
                if (exists(directory,stem,legacy+suffixes[kind])) { ++result.preserved; }
                else if (MoveFileExW(temporary.c_str(),(directory/(stem+L".png")).c_str(),MOVEFILE_WRITE_THROUGH)) {
                    temporary.clear(); ++result.installed;
                } else if (exists(directory,stem,legacy+suffixes[kind])) ++result.preserved;
                else throw std::runtime_error("Cannot install one library artwork image. Retry after closing programs using the artwork folder.");
            } catch (const std::exception& e) { if (result.warning.empty()) result.warning=e.what(); }
            if (!temporary.empty()) DeleteFileW(temporary.c_str());
        }
    } catch (const std::exception& e) { result.warning=e.what(); }
    return result;
}
#else
HazeSteamArtworkResult haze_steam_artwork(const std::filesystem::path&,uint32_t,const std::filesystem::path&) {
    return {0,0,"Library artwork installation is available in the native Windows build."};
}
#endif
