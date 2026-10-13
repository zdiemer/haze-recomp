#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../port/haze_window_icon.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cstring>
#include <crtdbg.h>

static void verify(HMODULE module,const std::vector<unsigned char>& expected={}) {
    auto resource=FindResourceW(module,MAKEINTRESOURCEW(101),MAKEINTRESOURCEW(14));
    assert(resource);
    auto bytes=(const unsigned char*)LockResource(LoadResource(module,resource));
    assert(bytes && SizeofResource(module,resource)>=6+7*14);
    auto word=[](const unsigned char* p) { return unsigned(p[0])+(unsigned(p[1])<<8); };
    assert(word(bytes)==0 && word(bytes+2)==1 && word(bytes+4)==7);
    const int sizes[]={16,24,32,48,64,128,256};
    for (unsigned i=0;i<7;++i) {
        const auto entry=bytes+6+i*14;
        assert((entry[0] ? entry[0] : 256)==sizes[i] && (entry[1] ? entry[1] : 256)==sizes[i]);
        if (!expected.empty()) {
            assert(expected.size()>=6+7*16);
            const auto directory=expected.data()+6+i*16;
            uint32_t length=0,offset=0;
            std::memcpy(&length,directory+8,4); std::memcpy(&offset,directory+12,4);
            assert(offset<=expected.size() && length<=expected.size()-offset);
            auto layer=FindResourceW(module,MAKEINTRESOURCEW(word(entry+12)),MAKEINTRESOURCEW(3));
            assert(layer && SizeofResource(module,layer)==length);
            auto pixels=LockResource(LoadResource(module,layer));
            assert(pixels && !std::memcmp(pixels,expected.data()+offset,length));
        }
        auto icon=(HICON)LoadImageW(module,MAKEINTRESOURCEW(101),IMAGE_ICON,sizes[i],sizes[i],0);
        assert(icon); ICONINFO info{}; assert(GetIconInfo(icon,&info));
        BITMAP bitmap{}; assert(GetObjectW(info.hbmColor,sizeof(bitmap),&bitmap));
        assert(bitmap.bmWidth==sizes[i] && bitmap.bmHeight==sizes[i]);
        DeleteObject(info.hbmColor); DeleteObject(info.hbmMask); DestroyIcon(icon);
    }
}
int wmain(int argc,wchar_t** argv) {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT|_WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    verify(GetModuleHandleW(nullptr));
    std::vector<unsigned char> expected;
    for (int i=1;i<argc;++i) {
        if (!std::wcscmp(argv[i],L"--icon-file")) {
            assert(++i<argc); std::ifstream file(std::filesystem::path(argv[i]),std::ios::binary);
            assert(file); expected.assign(std::istreambuf_iterator<char>(file),{}); assert(!expected.empty()); continue;
        }
        auto module=LoadLibraryExW(argv[i],nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        assert(module); verify(module,expected); FreeLibrary(module);
    }
    auto window=CreateWindowExW(0,L"STATIC",L"Icon fixture",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    assert(window);
    const auto metric=(int(WINAPI*)(int,UINT))GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetSystemMetricsForDpi");
    assert(metric);
    for (UINT dpi:{96u,144u,192u,96u}) {
        haze_window_icon(window,dpi);
        for (int small=0;small<2;++small) {
            auto icon=(HICON)SendMessageW(window,WM_GETICON,small ? ICON_SMALL : ICON_BIG,0);
            assert(icon); ICONINFO info{}; assert(GetIconInfo(icon,&info));
            BITMAP bitmap{}; assert(GetObjectW(info.hbmColor,sizeof(bitmap),&bitmap));
            assert(bitmap.bmWidth==metric(small ? SM_CXSMICON : SM_CXICON,dpi));
            assert(bitmap.bmHeight==metric(small ? SM_CYSMICON : SM_CYICON,dpi));
            DeleteObject(info.hbmColor); DeleteObject(info.hbmMask);
        }
    }
    DestroyWindow(window);
    std::puts("Embedded icon layers decode at all seven sizes; window large/small icons installed.");
}
