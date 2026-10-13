#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <crtdbg.h>
#include <thread>
#include "../port/haze_display.cpp"
#include "../port/haze_display_preview.cpp"
static int requested_mode,active_mode,sets;
static bool refuse_exclusive,refuse_all;
extern "C" int haze_video_display_get(int* requested,int* active,char*,unsigned) {
    *requested=requested_mode; *active=active_mode; return 1;
}
extern "C" int haze_video_display_set(int requested,int active,char* error,unsigned capacity) {
    ++sets;
    if (refuse_all || (refuse_exclusive && active)) { snprintf(error,capacity,"Injected display failure"); return 0; }
    requested_mode=requested; active_mode=active; return 1;
}
static RECT rectangle(HWND window) { RECT value{}; assert(GetWindowRect(window,&value)); return value; }
static void same(RECT a,RECT b) {
    if (!EqualRect(&a,&b)) std::fprintf(stderr,"Rectangle expected %ld,%ld,%ld,%ld; actual %ld,%ld,%ld,%ld\n",a.left,a.top,a.right,a.bottom,b.left,b.top,b.right,b.bottom);
    assert(EqualRect(&a,&b));
}
int main() {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT|_WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
    haze_display_prepare();
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Owned display transition fixture",WS_OVERLAPPEDWINDOW,
        130,150,900,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr); assert(window);
    const auto original=rectangle(window); const auto style=GetWindowLongPtrW(window,GWL_STYLE),extended=GetWindowLongPtrW(window,GWL_EXSTYLE);
    std::string error;
    assert(!haze_display_begin(window,"bad","primary","960x540",error) && sets==0);
    assert(!haze_display_begin(window,"windowed","primary","bad",error) && sets==0);
    haze_display_defaults unused{};
    if (!haze_display_detect("16",unused)) assert(!haze_display_begin(window,"windowed","16","960x540",error) && sets==0);
    same(original,rectangle(window));
    for (int i=0;i<3;++i) {
        assert(haze_display_begin(window,"windowed","primary","960x540",error));
        RECT client{}; assert(GetClientRect(window,&client) && client.right==960 && client.bottom==540);
        assert(haze_display_pending());
        assert(!haze_display_begin(window,"borderless","primary","960x540",error));
        assert(haze_display_finish(false,error) && !haze_display_pending());
        same(original,rectangle(window));
        if (GetWindowLongPtrW(window,GWL_STYLE)!=style || GetWindowLongPtrW(window,GWL_EXSTYLE)!=extended)
            std::fprintf(stderr,"Restored style %llx/%llx expected %llx/%llx\n",(long long)GetWindowLongPtrW(window,GWL_STYLE),(long long)GetWindowLongPtrW(window,GWL_EXSTYLE),(long long)style,(long long)extended);
        assert(GetWindowLongPtrW(window,GWL_STYLE)==style && GetWindowLongPtrW(window,GWL_EXSTYLE)==extended);
        assert(haze_display_begin(window,"borderless","primary","960x540",error));
        MONITORINFO info{sizeof(info)}; assert(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTOPRIMARY),&info));
        same(info.rcMonitor,rectangle(window));
        assert(haze_display_finish(false,error)); same(original,rectangle(window));
    }
    refuse_exclusive=true;
    assert(!haze_display_begin(window,"fullscreen","primary","960x540",error) && !haze_display_pending());
    same(original,rectangle(window)); assert(!active_mode && error.find("Injected")!=error.npos);
    refuse_exclusive=false;
    assert(haze_display_begin(window,"fullscreen","primary","960x540",error) && active_mode && requested_mode);
    assert(haze_display_finish(false,error) && !active_mode && !requested_mode); same(original,rectangle(window));
    // Keeping a transition establishes the geometry captured by the next preview.
    assert(haze_display_begin(window,"windowed","primary","800x450",error));
    const auto kept=rectangle(window); assert(haze_display_finish(true,error)); same(kept,rectangle(window));
    assert(haze_display_begin(window,"borderless","primary","800x450",error));
    assert(haze_display_finish(false,error)); same(kept,rectangle(window));
    // A failed revert must remain observable and retain its snapshot for retry.
    assert(haze_display_begin(window,"borderless","primary","800x450",error));
    refuse_all=true; assert(!haze_display_finish(false,error) && haze_display_pending() && !error.empty());
    assert(!haze_display_finish(true,error) && haze_display_pending()); // cannot keep a partial failed rollback
    refuse_all=false; assert(haze_display_finish(false,error)); same(kept,rectangle(window));
    bool other_thread=true;
    std::thread wrong_thread([&] { std::string detail; other_thread=haze_display_begin(window,"borderless","primary","800x450",detail); }); wrong_thread.join();
    assert(!other_thread && !haze_display_pending());
    // Restoring a fullscreen preference while the app is unfocused defers its
    // exclusive reacquisition, matching the renderer's existing focus policy.
    requested_mode=1; active_mode=0;
    assert(haze_display_begin(window,"windowed","primary","960x540",error));
    assert(haze_display_finish(false,error) && requested_mode && !active_mode);
    requested_mode=0;
    ShowWindow(window,SW_MAXIMIZE); const auto maximized=rectangle(window);
    assert(IsZoomed(window));
    assert(haze_display_begin(window,"windowed","primary","960x540",error));
    assert(haze_display_finish(false,error) && IsZoomed(window)); same(maximized,rectangle(window));
    DestroyWindow(window);
    std::puts("Display preview: real window sizing/placement, repeated revert/keep, validation, overlap, failed restoration and thread ownership passed; DXGI seam injected.");
}
