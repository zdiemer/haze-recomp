#include "haze_display.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "rsx_render_resolution.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace {
BOOL CALLBACK collect_monitor(HMONITOR monitor,HDC,LPRECT,LPARAM data) {
    MONITORINFOEXA info{}; info.cbSize=sizeof(info);
    if (GetMonitorInfoA(monitor,&info))
        reinterpret_cast<std::vector<MONITORINFOEXA>*>(data)->push_back(info);
    return TRUE;
}
bool select_monitor(const char* choice, std::vector<MONITORINFOEXA>& monitors, size_t& selected) {
    if (!EnumDisplayMonitors(nullptr,nullptr,collect_monitor,reinterpret_cast<LPARAM>(&monitors)) || monitors.empty()) {
        std::fprintf(stderr,"[display] Cannot enumerate monitors\n"); return false;
    }
    std::sort(monitors.begin(),monitors.end(),[](const auto& a,const auto& b){return std::strcmp(a.szDevice,b.szDevice)<0;});
    selected=0;
    if (!choice || !std::strcmp(choice,"primary")) {
        for(size_t i=0;i<monitors.size();++i) if(monitors[i].dwFlags&MONITORINFOF_PRIMARY) selected=i;
    } else {
        char* end=nullptr;
        const long number=std::strtol(choice,&end,10);
        if(*end || number<1 || static_cast<size_t>(number)>monitors.size()) {
            std::fprintf(stderr,"[display] Monitor %s unavailable (%zu connected)\n",choice,monitors.size()); return false;
        }
        selected=number-1;
    }
    return true;
}
void fit_window(LONG work_width, LONG work_height, LONG frame_width, LONG frame_height,
                unsigned& width, unsigned& height) {
    // Reserve space for decorations and retain the game's 16:9 aspect ratio.
    const LONG available_width=(std::max)(1L,work_width-frame_width);
    const LONG available_height=(std::max)(1L,work_height-frame_height);
    width=static_cast<unsigned>((std::min)({1280L,available_width,available_height*16/9}));
    width=(std::max)(1u,width);
    height=(std::max)(1u,width*9/16);
}
}
void haze_display_prepare() {
    // The renderer/window thread must use physical monitor pixels, including
    // when other monitors use a different desktop scaling percentage.
    using SetContext=DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto set_context=reinterpret_cast<SetContext>(GetProcAddress(GetModuleHandleA("user32.dll"),"SetThreadDpiAwarenessContext"));
    if (set_context) set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
bool haze_display_detect(const char* choice, haze_display_defaults& defaults) {
    // Restore the caller's DPI context after querying physical desktop bounds.
    using SetContext=DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto user=GetModuleHandleA("user32.dll");
    auto set_context=reinterpret_cast<SetContext>(GetProcAddress(user,"SetThreadDpiAwarenessContext"));
    const auto previous=set_context ? set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
    std::vector<MONITORINFOEXA> monitors;
    size_t selected=0;
    const bool found=select_monitor(choice,monitors,selected);
    if(found) {
        const auto& monitor=monitors[selected];
        defaults.render_width=static_cast<unsigned>((std::min)(8192L,monitor.rcMonitor.right-monitor.rcMonitor.left));
        defaults.render_height=static_cast<unsigned>((std::min)(8192L,monitor.rcMonitor.bottom-monitor.rcMonitor.top));
        // A hidden per-monitor-aware window provides the selected display's
        // effective DPI even when the process itself has no DPI manifest.
        using GetDpi=UINT (WINAPI*)(HWND);
        auto get_dpi=reinterpret_cast<GetDpi>(GetProcAddress(user,"GetDpiForWindow"));
        UINT dpi=96;
        if(get_dpi) {
            auto probe=CreateWindowExA(0,"STATIC","",WS_OVERLAPPEDWINDOW,
                monitor.rcMonitor.left,monitor.rcMonitor.top,1,1,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);
            if(!probe) {
                std::fprintf(stderr,"[display] Cannot query monitor DPI\n");
                if(previous) set_context(previous);
                return false;
            }
            dpi=get_dpi(probe);
            DestroyWindow(probe);
        }
        using AdjustDpi=BOOL (WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT);
        auto adjust_dpi=reinterpret_cast<AdjustDpi>(GetProcAddress(user,"AdjustWindowRectExForDpi"));
        RECT frame{};
        const bool adjusted=adjust_dpi ? adjust_dpi(&frame,WS_OVERLAPPEDWINDOW,FALSE,0,dpi)!=FALSE :
            AdjustWindowRectEx(&frame,WS_OVERLAPPEDWINDOW,FALSE,0)!=FALSE;
        if(!adjusted) { if(previous) set_context(previous); return false; }
        fit_window(monitor.rcWork.right-monitor.rcWork.left,monitor.rcWork.bottom-monitor.rcWork.top,
            frame.right-frame.left,frame.bottom-frame.top,defaults.window_width,defaults.window_height);
        std::fprintf(stderr,"[display] auto monitor=%zu desktop=%ux%u window=%ux%u\n",selected+1,
            defaults.render_width,defaults.render_height,defaults.window_width,defaults.window_height);
    }
    if(previous) set_context(previous);
    return found;
}
bool haze_display_configure(void* window,const char* mode,const char* choice,const char* window_size,bool activate) {
    HWND hwnd=static_cast<HWND>(window);
    if (!IsWindow(hwnd) || GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId() || !mode ||
        (std::strcmp(mode,"windowed") && std::strcmp(mode,"borderless") && std::strcmp(mode,"fullscreen"))) return false;
    rsx_render_resolution size;
    if(rsx_render_resolution_parse(&size,1280,720,window_size)<0) return false;
    std::vector<MONITORINFOEXA> monitors;
    size_t selected=0;
    if(!select_monitor(choice,monitors,selected)) return false;
    for(size_t i=0;i<monitors.size();++i)
        std::fprintf(stderr,"[display] monitor %zu: %s %ldx%ld%s\n",i+1,monitors[i].szDevice,
            monitors[i].rcMonitor.right-monitors[i].rcMonitor.left,
            monitors[i].rcMonitor.bottom-monitors[i].rcMonitor.top,
            monitors[i].dwFlags&MONITORINFOF_PRIMARY ? " primary":"");
    const auto& monitor=monitors[selected];
    const bool decorated=!std::strcmp(mode,"windowed");
    const LONG_PTR style=decorated ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    RECT target=monitor.rcMonitor;
    if(decorated) {
        RECT previous{}; if (!GetWindowRect(hwnd,&previous)) return false;
        if(!SetWindowPos(hwnd,nullptr,
            (monitor.rcMonitor.left+monitor.rcMonitor.right-(previous.right-previous.left))/2,
            (monitor.rcMonitor.top+monitor.rcMonitor.bottom-(previous.bottom-previous.top))/2,
            0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE)) return false;
        RECT frame{0,0,static_cast<LONG>(size.host_w),static_cast<LONG>(size.host_h)};
        using GetDpi=UINT (WINAPI*)(HWND);
        using AdjustDpi=BOOL (WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT);
        auto user=GetModuleHandleA("user32.dll");
        auto get_dpi=reinterpret_cast<GetDpi>(GetProcAddress(user,"GetDpiForWindow"));
        auto adjust_dpi=reinterpret_cast<AdjustDpi>(GetProcAddress(user,"AdjustWindowRectExForDpi"));
        if(!(get_dpi && adjust_dpi ? adjust_dpi(&frame,static_cast<DWORD>(style),FALSE,0,get_dpi(hwnd)) :
            AdjustWindowRectEx(&frame,static_cast<DWORD>(style),FALSE,0))) return false;
        // Keep the title bar reachable when a requested client exceeds the
        // work area; client size is still exactly the requested pixel size.
        const LONG w=frame.right-frame.left,h=frame.bottom-frame.top;
        target.left=monitor.rcWork.left+(std::max)(0L,(monitor.rcWork.right-monitor.rcWork.left-w)/2);
        target.top=monitor.rcWork.top+(std::max)(0L,(monitor.rcWork.bottom-monitor.rcWork.top-h)/2);
        target.right=target.left+w; target.bottom=target.top+h;
    }
    SetLastError(0);
    if(!SetWindowLongPtrA(hwnd,GWL_STYLE,style|WS_VISIBLE) && GetLastError()) return false;
    if(!SetWindowPos(hwnd,HWND_NOTOPMOST,target.left,target.top,target.right-target.left,target.bottom-target.top,
        SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_SHOWWINDOW)) return false;
    if(!decorated && activate) { ShowWindow(hwnd,SW_SHOW); SetForegroundWindow(hwnd); }
    RECT client{}; GetClientRect(hwnd,&client);
    std::fprintf(stderr,"[display] mode=%s monitor=%zu client=%ldx%ld\n",mode,selected+1,client.right,client.bottom);
    return true;
}
bool haze_display_apply(void* window) {
    const char* mode=std::getenv("RSX_WINDOW_MODE");
    return haze_display_configure(window,mode ? mode : "windowed",std::getenv("RSX_MONITOR"),std::getenv("RSX_WINDOW_SIZE"),true);
}
#else
bool haze_display_detect(const char*, haze_display_defaults& defaults) {
    defaults={1280,720,1280,720}; return true;
}
void haze_display_prepare() {}
bool haze_display_apply(void*) { return true; }
bool haze_display_configure(void*,const char*,const char*,const char*,bool) { return false; }
#endif
