#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include "../port/haze_frame_pacer_settings.h"
#include "../port/haze_display.cpp"
#include "../port/haze_graphics.cpp"
#include "../port/haze_graphics_live.cpp"
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
    std::remove("local/test-graphics.ini");
    std::puts("Graphics configuration: INI, automatic monitor defaults, missing optional file, overrides, BOM/CRLF and atomic rejection passed.");
}
