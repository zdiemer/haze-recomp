#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <crtdbg.h>
#include <thread>
#include "../port/haze_frame_pacer_settings.h"
#include "../port/haze_display.cpp"
static ULONGLONG test_time=1000;
static HWND test_focus;
static ULONGLONG preview_clock() { return test_time; }
static HWND preview_focus() { return test_focus; }
#define GetTickCount64 preview_clock
#define GetForegroundWindow preview_focus
#include "../port/haze_graphics.cpp"
#include "../port/haze_graphics_live.cpp"
#undef GetTickCount64
#undef GetForegroundWindow
static bool display_active,display_begin_fail,display_restore_fail,display_partial_fail;
static unsigned display_keeps,display_reverts;
bool haze_display_pending() { return display_active; }
bool haze_display_can_keep() { return display_active && !display_partial_fail && !display_restore_fail; }
bool haze_display_begin(void*,const char*,const char*,const char*,std::string& error) {
    // Real Win32/DXGI calls can synchronously reenter read-only window caption
    // code. It must not deadlock, and a second writer must be refused.
    auto snapshot=haze_graphics_state(); (void)haze_graphics_notice(); std::string nested;
    assert(!haze_graphics_apply(snapshot.configured,true,nested));
    if (display_begin_fail) { error="Injected display failure"; display_active=display_partial_fail; return false; }
    display_active=true; return true;
}
bool haze_display_finish(bool keep,std::string& error) {
    if (!keep && display_restore_fail) { error="Injected restore failure"; return false; }
    if (keep) ++display_keeps; else ++display_reverts;
    display_active=false; return true;
}
static bool backend_fail;
static unsigned commits,cancels;
static haze_video_live active_video,pending_video;
extern "C" int haze_video_prepare(const haze_video_live* s,char* error,unsigned size) {
    if (backend_fail) { snprintf(error,size,"Injected allocation failure"); return 0; }
    pending_video=*s; return 1;
}
extern "C" void haze_video_finish(int commit) {
    if (commit) { active_video=pending_video; ++commits; } else ++cancels;
}
static void clear_options() {
    for (const auto& o:rsx_graphics_options) _putenv_s(o.environment,"");
}
static void config(const char* text) {
    FILE* f=std::fopen("local/test-graphics.ini","wb"); assert(f);
    std::fputs(text,f); std::fclose(f);
}
int main() {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT|_WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
    clear_options(); _putenv_s("HAZE_GRAPHICS_CONFIG","local/test-graphics.ini");
    config("\xef\xbb\xbf[graphics]\r\nrender_resolution=1777x997\nanti_aliasing=fxaa\nanisotropic_filtering=16\nshadow_resolution=4096\nmip_lod_bias=-0.5\nsharpening=0.3\nexposure=0.5\nvsync=on\nframe_limit=60\n");
    _putenv_s("RSX_MAX_FPS","0"); assert(haze_graphics_initialize());
    assert(!std::strcmp(std::getenv("RSX_MAX_FPS"),"0"));
    assert(haze_native_pacer_selected(nullptr,std::getenv("RSX_MAX_FPS")));
    assert(!std::strcmp(std::getenv("RSX_RENDER_RESOLUTION"),"1777x997"));
    assert(!std::strcmp(std::getenv("RSX_ANISOTROPY"),"16"));
    // Exercise the actual INI -> runtime environment -> pacing selection path.
    clear_options(); config("[graphics]\nframe_limit=60\nvsync=off\n");
    assert(haze_graphics_initialize());
    assert(haze_native_pacer_selected(nullptr,std::getenv("RSX_MAX_FPS")));
    assert(!haze_native_pacer_selected("0",std::getenv("RSX_MAX_FPS")));
    clear_options(); config("[graphics]\nframe_limit=30\nvsync=on\n");
    assert(haze_graphics_initialize());
    assert(!haze_native_pacer_selected(nullptr,std::getenv("RSX_MAX_FPS")));
    const char* bad[]={"[graphics]\nframe_limit=60\nsharpening=nan\n",
        "[graphics]\nframe_limit=60\nframe_limit=30\n", "[unknown]\nframe_limit=60\n",
        "[graphics]\ndraw_distance=100\n", "[graphics]\nanti_aliasing=msaa\n",
        "[graphics]\nanisotropic_filtering=3\n", "[graphics]\nshadow_resolution=4095\n",
        "[graphics]\nrender_resolution=8193x720\n", "[graphics]\nexposure=inf\n",
        "[graphics]\nframe_limit=-1\n", "[graphics]\nmip_lod_bias=3\n",
        "[graphics]\ndisplay_mode=exclusive\n", "[graphics]\nwindow_size=native\n",
        "[graphics]\nmonitor=0\n", "[graphics]\nmonitor=1.5\n"};
    for (auto text:bad) {
        clear_options(); config(text); assert(!haze_graphics_initialize());
        assert(!std::getenv("RSX_MAX_FPS")); // Failed file never partially applies.
    }
    clear_options(); config("[graphics]\nframe_limit=60\n");
    _putenv_s("RSX_SHARPENING","2"); assert(!haze_graphics_initialize());
    assert(!std::getenv("RSX_MAX_FPS"));
    haze_display_defaults detected{}; assert(haze_display_detect("primary",detected));
    auto dimensions=[](unsigned w,unsigned h) { return std::to_string(w)+"x"+std::to_string(h); };
    auto check_defaults=[&](const char* monitor="primary") {
        assert(std::getenv("RSX_RENDER_RESOLUTION")==dimensions(detected.render_width,detected.render_height));
        assert(std::getenv("RSX_WINDOW_SIZE")==dimensions(detected.window_width,detected.window_height));
        assert(!std::strcmp(std::getenv("RSX_SHADOW_RESOLUTION"),"native"));
        assert(!std::strcmp(std::getenv("RSX_WINDOW_MODE"),"borderless"));
        for (const auto& o:rsx_graphics_options)
            if(!automatic_size(o)) assert(!std::strcmp(std::getenv(o.environment),!std::strcmp(o.key,"monitor") ? monitor : fallback(o)));
    };
    clear_options(); config("[graphics]\n"); assert(haze_graphics_initialize()); check_defaults();
    clear_options(); config("[graphics]\nrender_resolution=auto\nwindow_size=auto\nmonitor=1\n");
    assert(haze_display_detect("1",detected)); assert(haze_graphics_initialize()); check_defaults("1");
    clear_options(); config("[graphics]\nrender_resolution=auto\nwindow_size=auto\n");
    _putenv_s("RSX_RENDER_RESOLUTION","1920x1080"); _putenv_s("RSX_WINDOW_SIZE","960x540");
    assert(haze_graphics_initialize());
    assert(!std::strcmp(std::getenv("RSX_RENDER_RESOLUTION"),"1920x1080"));
    assert(!std::strcmp(std::getenv("RSX_WINDOW_SIZE"),"960x540"));
    clear_options(); config("[graphics]\nrender_resolution=native\nwindow_size=800x450\n");
    _putenv_s("RSX_RENDER_RESOLUTION","auto"); _putenv_s("RSX_WINDOW_SIZE","auto");
    assert(haze_display_detect("primary",detected)); assert(haze_graphics_initialize()); check_defaults();
    clear_options(); config("[graphics]\nrender_resolution=automatic\n"); assert(!haze_graphics_initialize());
    assert(!std::getenv("RSX_MAX_FPS"));
    clear_options(); config("[graphics]\nmonitor=16\n");
    haze_display_defaults unused{};
    if(!haze_display_detect("16",unused)) {
        assert(!haze_graphics_initialize()); assert(!std::getenv("RSX_MAX_FPS"));
    }
    // An absent optional config uses the same defaults as an empty INI.
    clear_options(); _putenv_s("HAZE_GRAPHICS_CONFIG","");
    char cwd[MAX_PATH]; assert(GetCurrentDirectoryA(sizeof(cwd),cwd));
    assert(SetCurrentDirectoryA("local")); assert(haze_graphics_initialize()); check_defaults();
    assert(!haze_native_pacer_selected(nullptr,std::getenv("RSX_MAX_FPS")));
    assert(SetCurrentDirectoryA(cwd));
    clear_options(); _putenv_s("HAZE_GRAPHICS_CONFIG","local/nonexistent-graphics.ini");
    assert(!haze_graphics_initialize());
    clear_options(); _putenv_s("HAZE_GRAPHICS_CONFIG","local/test-graphics.ini");
    config("[graphics]\nframe_limit=30\n");
    _putenv_s("RSX_EXPOSURE","0.25"); assert(haze_graphics_initialize());
    auto before=haze_graphics_state();
    assert(before.overrides.size()==1 && before.overrides.at("exposure")=="0.25");
    std::string error;
    for (int iteration=0;iteration<3;++iteration) for (const auto* quality:{"Low","Medium","High"}) {
        auto requested=before.configured; assert(haze_video::preset(requested,quality));
        requested["frame_limit"]="60"; requested["exposure"]="1";
        assert(haze_graphics_apply(requested,true,error));
        auto current=haze_graphics_state();
        assert(current.configured==requested && current.effective.at("exposure")=="0.25");
        assert(current.effective.at("frame_limit")=="60" && haze_live_frame_limit==60);
        assert(active_video.exposure==.25f && active_video.frame_limit==60);
        assert(haze_video::preset_name(current.configured)==quality);
        std::ifstream saved("local/test-graphics.ini"); haze_video::Values loaded;
        assert(haze_video::parse(saved,loaded,error) && loaded==requested);
        saved.close();
        assert(haze_video::preset_name(loaded)==quality);
        assert(haze_graphics_apply(before.configured,true,error));
        assert(haze_graphics_state().restart.empty()); // Restore auto/native startup values.
    }
    auto unchanged=haze_graphics_state(); unsigned count=commits;
    std::string old_file; assert(read_text(unchanged.path,old_file));
    auto rejected=unchanged.configured; rejected["exposure"]="nan";
    assert(!haze_graphics_apply(rejected,true,error) && commits==count);
    rejected=unchanged.configured; rejected.erase("vsync");
    assert(!haze_graphics_apply(rejected,true,error) && commits==count);
    backend_fail=true; rejected=unchanged.configured; haze_video::preset(rejected,"High");
    assert(!haze_graphics_apply(rejected,true,error) && commits==count);
    std::string after_file; assert(read_text(unchanged.path,after_file) && after_file==old_file);
    backend_fail=false;
    // An external editor's later change must survive a stale menu's Apply.
    config("[graphics]\nframe_limit=24\n");
    assert(!haze_graphics_apply(rejected,true,error) && commits==count && cancels==1);
    assert(haze_graphics_state().configured==unchanged.configured);
    haze_graphics_request_reload(); haze_graphics_update();
    assert(haze_graphics_state().effective.at("frame_limit")=="24");
    assert(haze_graphics_state().overrides.size()==1); // Own startup env writes are not overrides.
    const auto now=haze_graphics_state();
    // A locked destination exercises failure after renderer preparation.
    HANDLE locked=CreateFileA(now.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    assert(locked!=INVALID_HANDLE_VALUE);
    assert(!haze_graphics_apply(rejected,true,error) && cancels==2);
    CloseHandle(locked);
    assert(haze_graphics_state().configured==now.configured);
    auto custom=now.configured; haze_video::preset(custom,"High"); custom["sharpening"]="0.150";
    assert(haze_video::preset_name(custom)=="High"); custom["sharpening"]="0.2";
    assert(haze_video::preset_name(custom)=="Custom");
    // Timed display transaction uses the actual settings persistence code;
    // window/DXGI and the clock are injected separately from the state machine.
    HWND window=CreateWindowExW(0,L"STATIC",L"Graphics preview owner",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    assert(window); test_focus=window;
    const auto baseline=haze_graphics_state();
    std::string baseline_file; assert(read_text(baseline.path,baseline_file));
    auto staged=baseline.configured; staged["display_mode"]="windowed"; staged["window_size"]="960x540"; staged["frame_limit"]="60";
    auto start_preview=[&] {
        assert(haze_graphics_preview(window,staged,true,error));
        const auto p=haze_graphics_state(); assert(p.display_preview && p.display_seconds==15 && !p.display_restore_failed);
        assert(p.configured==baseline.configured && p.effective.at("window_size")=="960x540" && p.effective.at("frame_limit")==baseline.effective.at("frame_limit"));
        std::string text; assert(read_text(baseline.path,text) && text==baseline_file); return p.display_preview;
    };
    unsigned revision=start_preview();
    assert(!haze_graphics_preview(window,staged,true,error));
    assert(!haze_graphics_apply(staged,true,error));
    assert(!haze_graphics_confirm(revision+1,true,error) && display_active);
    haze_graphics_request_reload(); haze_graphics_update(); assert(haze_graphics_state().display_preview==revision);
    test_time+=14999; haze_graphics_update(); assert(haze_graphics_state().display_seconds==1);
    ++test_time; haze_graphics_update(); assert(!haze_graphics_state().display_preview && !display_active);
    assert(haze_graphics_state().effective==baseline.effective);
    revision=start_preview(); test_time+=15000;
    assert(!haze_graphics_confirm(revision,true,error) && !display_active); // Keep exactly at deadline loses
    revision=start_preview(); test_focus=nullptr; haze_graphics_update(); assert(!display_active); test_focus=window;
    revision=start_preview(); test_focus=nullptr;
    assert(!haze_graphics_confirm(revision,true,error) && !display_active); test_focus=window;
    revision=start_preview(); assert(haze_graphics_confirm(revision,false,error) && !display_active);
    revision=start_preview();
    bool wrong_thread_accepted=true;
    std::thread wrong_thread([&] { std::string detail; wrong_thread_accepted=haze_graphics_confirm(revision,true,detail); }); wrong_thread.join();
    assert(!wrong_thread_accepted && display_active);
    locked=CreateFileA(baseline.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr); assert(locked!=INVALID_HANDLE_VALUE);
    assert(!haze_graphics_confirm(revision,true,error) && !display_active); CloseHandle(locked);
    assert(haze_graphics_state().configured==baseline.configured && haze_graphics_state().effective==baseline.effective);
    revision=start_preview(); config("[graphics]\nframe_limit=25\n");
    assert(!haze_graphics_confirm(revision,true,error) && !display_active);
    std::string changed_file; assert(read_text(baseline.path,changed_file) && changed_file.find("25")!=std::string::npos);
    config(baseline_file.c_str());
    revision=start_preview(); backend_fail=true;
    assert(!haze_graphics_confirm(revision,true,error) && !display_active); backend_fail=false;
    revision=start_preview(); display_restore_fail=true;
    assert(!haze_graphics_confirm(revision,false,error));
    assert(haze_graphics_state().display_restore_failed && haze_graphics_state().display_preview==revision);
    assert(!haze_graphics_confirm(revision,true,error));
    display_restore_fail=false; assert(haze_graphics_confirm(revision,false,error));
    display_begin_fail=true;
    assert(!haze_graphics_preview(window,staged,true,error) && !haze_graphics_state().display_preview);
    display_partial_fail=true;
    assert(!haze_graphics_preview(window,staged,true,error));
    revision=haze_graphics_state().display_preview; assert(revision && haze_graphics_state().display_restore_failed);
    assert(!haze_graphics_confirm(revision,true,error));
    display_begin_fail=display_partial_fail=false; assert(haze_graphics_confirm(revision,false,error));
    revision=start_preview(); const unsigned kept_before=display_keeps;
    assert(haze_graphics_confirm(revision,true,error) && display_keeps==kept_before+1 && !display_active);
    const auto committed=haze_graphics_state();
    assert(!committed.display_preview && committed.configured==staged && committed.effective.at("window_size")=="960x540" && committed.effective.at("frame_limit")=="60");
    assert(std::find(committed.restart.begin(),committed.restart.end(),"display_mode")==committed.restart.end());
    assert(read_text(baseline.path,changed_file) && changed_file==haze_video::serialize(staged));
    assert(!haze_graphics_confirm(revision,false,error)); // old callback cannot undo a committed change
    auto quality_only=staged; quality_only["frame_limit"]="30";
    assert(haze_graphics_preview(window,quality_only,true,error) && !haze_graphics_state().display_preview);
    // An explicit launch override persists the requested preference but does
    // not preview a display mode that cannot become effective this session.
    clear_options(); config("[graphics]\n"); _putenv_s("RSX_WINDOW_MODE","windowed"); assert(haze_graphics_initialize());
    auto overridden=haze_graphics_state().configured; overridden["display_mode"]="fullscreen";
    assert(haze_graphics_preview(window,overridden,true,error));
    assert(!haze_graphics_state().display_preview && haze_graphics_state().configured.at("display_mode")=="fullscreen" && haze_graphics_state().effective.at("display_mode")=="windowed");
    DestroyWindow(window); std::remove("local/test-graphics.ini");
    std::puts("Graphics configuration: INI, automatic monitor defaults, missing optional file, overrides, BOM/CRLF and atomic rejection passed.");
}
