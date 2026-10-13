#include "haze_display.h"
#include "haze_video_backend.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include "rsx_render_resolution.h"

namespace {
struct Snapshot {
    HWND window=nullptr;
    DWORD thread=0;
    LONG_PTR style=0,extended=0;
    RECT rect{};
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    int fullscreen_requested=0,fullscreen_active=0;
} previous;
bool pending=false;
bool preview_valid=false;
bool set_style(HWND window,int field,LONG_PTR value) {
    SetLastError(0); return SetWindowLongPtrW(window,field,value)!=0 || !GetLastError();
}
bool restore_window() {
    const auto& p=previous;
    if (!IsWindow(p.window)) return false;
    // Let SetWindowPlacement perform maximize/minimize rather than asserting
    // those style bits before Windows has actually restored their geometry.
    bool ok=set_style(p.window,GWL_STYLE,p.style&~(WS_MAXIMIZE|WS_MINIMIZE));
    ok=set_style(p.window,GWL_EXSTYLE,p.extended) && ok;
    ok=SetWindowPlacement(p.window,&p.placement)!=FALSE && ok;
    const bool normal=p.placement.showCmd!=SW_SHOWMAXIMIZED && p.placement.showCmd!=SW_SHOWMINIMIZED;
    // Let Windows' placement recovery choose a visible location if the old
    // monitor disappeared; do not force its now-offscreen rectangle back.
    const bool available=MonitorFromRect(&p.rect,MONITOR_DEFAULTTONULL)!=nullptr;
    ok=SetWindowPos(p.window,(p.extended&WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST,
        p.rect.left,p.rect.top,p.rect.right-p.rect.left,p.rect.bottom-p.rect.top,
        SWP_FRAMECHANGED|SWP_NOACTIVATE|((normal && available) ? 0 : SWP_NOMOVE|SWP_NOSIZE))!=FALSE && ok;
    // A hidden window can still have SW_SHOWNORMAL in WINDOWPLACEMENT.
    // SetWindowPlacement may show it; preserve its actual pre-preview visibility.
    if (!(p.style&WS_VISIBLE)) ShowWindow(p.window,SW_HIDE);
    return ok;
}
}
bool haze_display_pending() { return pending; }
bool haze_display_can_keep() { return pending && preview_valid && GetCurrentThreadId()==previous.thread && IsWindow(previous.window); }
bool haze_display_finish(bool keep,std::string& error) {
    error.clear();
    if (!pending) return true;
    if (GetCurrentThreadId()!=previous.thread) { error="Display confirmation must run on the window thread."; return false; }
    if (keep) {
        if (!preview_valid) { error="The display transition failed. Restore the previous display before continuing."; return false; }
        pending=false; preview_valid=false; previous={}; return true;
    }
    preview_valid=false;
    char detail[256]{};
    // Leave exclusive mode before moving the window back to its original
    // monitor. Continue geometry restoration even if the device was removed.
    const bool windowed=haze_video_display_set(0,0,detail,sizeof detail)!=0;
    const bool geometry=restore_window();
    const HWND focus=GetForegroundWindow();
    const bool focused=focus==previous.window || (focus && GetWindow(focus,GW_OWNER)==previous.window);
    const int exclusive=previous.fullscreen_active && focused && !IsIconic(previous.window);
    const bool fullscreen=windowed && haze_video_display_set(previous.fullscreen_requested,exclusive,detail,sizeof detail)!=0;
    if (!geometry || !fullscreen) {
        error=geometry ? detail : "Could not restore the previous window placement. Settings were not saved.";
        // Retain the snapshot so the caller can retry or report the failed
        // restoration. Do not advertise a successful revert on partial failure.
        return false;
    }
    pending=false; previous={}; return true;
}
bool haze_display_begin(void* window,const char* mode,const char* monitor,const char* size,std::string& error) {
    error.clear();
    if (pending) { error="Confirm or revert the current display change first."; return false; }
    HWND hwnd=static_cast<HWND>(window);
    if (!IsWindow(hwnd) || GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId()) {
        error="Display changes must run on the game window thread."; return false;
    }
    // Validate all requested inputs before touching the existing display.
    if (!mode || (std::strcmp(mode,"windowed") && std::strcmp(mode,"borderless") && std::strcmp(mode,"fullscreen"))) {
        error="Invalid display mode."; return false;
    }
    rsx_render_resolution dimensions;
    if (rsx_render_resolution_parse(&dimensions,1280,720,size)<0) { error="Invalid window dimensions."; return false; }
    haze_display_defaults detected{};
    if (!haze_display_detect(monitor,detected)) { error="The selected display is unavailable."; return false; }
    Snapshot snapshot; snapshot.window=hwnd; snapshot.thread=GetCurrentThreadId();
    snapshot.style=GetWindowLongPtrW(hwnd,GWL_STYLE); snapshot.extended=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    if (!GetWindowRect(hwnd,&snapshot.rect) || !GetWindowPlacement(hwnd,&snapshot.placement)) {
        error="Cannot preserve the current window placement."; return false;
    }
    char detail[256]{};
    if (!haze_video_display_get(&snapshot.fullscreen_requested,&snapshot.fullscreen_active,detail,sizeof detail)) { error=detail; return false; }
    previous=snapshot; pending=true; preview_valid=false;
    if (haze_video_display_set(0,0,detail,sizeof detail) && haze_display_configure(hwnd,mode,monitor,size,false) &&
        (std::strcmp(mode,"fullscreen") || haze_video_display_set(1,1,detail,sizeof detail))) { preview_valid=true; return true; }
    error=*detail ? detail : "Cannot apply the requested window placement.";
    std::string rollback;
    if (!haze_display_finish(false,rollback)) error+=" "+rollback;
    return false;
}
#else
bool haze_display_pending() { return false; }
bool haze_display_can_keep() { return false; }
bool haze_display_begin(void*,const char*,const char*,const char*,std::string& error) { error="Display changes require the native Windows renderer."; return false; }
bool haze_display_finish(bool,std::string&) { return true; }
#endif
