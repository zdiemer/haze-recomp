#include "haze_graphics.h"
#include "haze_video_backend.h"
#include "haze_frame_pacer_settings.h"
#include "haze_display.h"
#include <atomic>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <filesystem>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
std::mutex state_mutex;
HazeGraphicsState state;
std::string source_text;
haze_video::Values startup_requested;
std::atomic<bool> reload_requested;
bool read_text(const std::string& path,std::string& text) {
    std::ifstream in(path,std::ios::binary);
    if (!in) return false;
    text.assign(std::istreambuf_iterator<char>(in),{}); return !in.bad();
}
bool save(const std::string& path,const std::string& text,std::string& error) {
#ifdef _WIN32
    const auto destination=std::filesystem::path(path).wstring();
    const auto temporary=destination+L".tmp."+std::to_wstring(GetCurrentProcessId());
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) { error="Could not create a settings file. Check folder permissions."; return false; }
    DWORD written=0;
    bool ok=WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    // Recheck after staging. Never knowingly overwrite an external edit made
    // since this menu's values were loaded.
    std::string current;
    const bool exists=read_text(path,current);
    if ((exists && current!=source_text) || (!exists && (!source_text.empty() ||
        GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES))) {
        error="The settings file changed. Reload it before applying your edits."; ok=false;
    }
    if (ok) ok=MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if (!ok) {
        DeleteFileW(temporary.c_str());
        if (error.empty()) error="Could not save video settings. Previous settings retained.";
    }
    return ok;
#else
    (void)path; (void)text; error="Live video settings require the native renderer."; return false;
#endif
}
}
HazeGraphicsState haze_graphics_state() {
    std::lock_guard<std::mutex> lock(state_mutex); return state;
}
std::string haze_graphics_notice() {
    std::lock_guard<std::mutex> lock(state_mutex); return state.notice;
}
void haze_graphics_started(const haze_video::Values& configured,
    const haze_video::Values& effective,const haze_video::Values& overrides,const char* path) {
    std::lock_guard<std::mutex> lock(state_mutex);
    state={}; state.path=path; state.configured=haze_video::defaults();
    for (const auto& [key,value]:configured) state.configured[key]=value;
    state.effective=effective; state.overrides=overrides;
    startup_requested=state.configured;
    for (const auto& [key,value]:overrides) startup_requested[key]=value;
    source_text.clear(); read_text(path,source_text);
    haze_live_frame_limit=std::stoi(effective.at("frame_limit"));
}
bool haze_graphics_apply(const haze_video::Values& configured,bool persist,std::string& error) {
    error.clear();
    if (!haze_video::validate(configured,error)) return false;
    if (configured.size()!=haze_video::defaults().size()) { error="Incomplete video settings."; return false; }
    std::lock_guard<std::mutex> lock(state_mutex);
    auto target=configured;
    for (const auto& [key,value]:state.overrides) target[key]=value;
    haze_display_defaults monitor{};
    if (!haze_display_detect(target.at("monitor").c_str(),monitor)) {
        error="The selected display is unavailable."; return false;
    }
    std::vector<std::string> restart;
    for (const auto& [key,value]:target) if (haze_video::restart_required(key)) {
        // Startup resolves auto dimensions. Unchanged auto values continue to
        // describe the existing monitor selection; do not falsely flag them.
        if (value!=state.effective.at(key) && value!=startup_requested.at(key)) restart.push_back(key);
        target[key]=state.effective.at(key);
    }
#ifdef _WIN32
    haze_video_live live{};
    live.aa=target.at("anti_aliasing")=="fxaa";
    live.anisotropy=unsigned(std::stoi(target.at("anisotropic_filtering")));
    live.frame_limit=unsigned(std::stoi(target.at("frame_limit")));
    live.mip_bias=float(std::strtod(target.at("mip_lod_bias").c_str(),nullptr));
    live.sharpening=float(std::strtod(target.at("sharpening").c_str(),nullptr));
    live.exposure=float(std::strtod(target.at("exposure").c_str(),nullptr));
    live.vsync=target.at("vsync")=="on" ? 1 : target.at("vsync")=="off" ? 0 : -1;
    char detail[256]{};
    if (!haze_video_prepare(&live,detail,sizeof detail)) { error=detail; return false; }
    std::string serialized;
    if (persist) {
        serialized=haze_video::serialize(configured);
        if (!save(state.path,serialized,error)) { haze_video_finish(0); return false; }
    }
    haze_video_finish(1);
    haze_live_frame_limit=static_cast<int>(live.frame_limit);
    state.configured=configured; state.effective=std::move(target); state.restart=std::move(restart);
    if (persist) source_text=std::move(serialized); else read_text(state.path,source_text);
    state.notice=state.restart.empty() ? "Video settings applied" : "Video settings applied; some changes need restart";
    if (!state.overrides.empty()) state.notice+="; launch overrides active";
    std::fprintf(stderr,"[graphics] %s; preset=%s\n",state.notice.c_str(),haze_video::preset_name(configured).c_str());
    for (const auto& [key,value]:state.effective)
        std::fprintf(stderr,"[graphics] effective %s=%s%s\n",key.c_str(),value.c_str(),state.overrides.count(key) ? " (launch override)":"");
    return true;
#else
    error="Live video settings require the native renderer."; return false;
#endif
}
void haze_graphics_request_reload() { reload_requested=true; }
void haze_graphics_update() {
    if (!reload_requested.exchange(false)) return;
    const auto current=haze_graphics_state();
    std::ifstream file(current.path); haze_video::Values requested; std::string error;
    if (!file) error="Could not open video settings.";
    else if (haze_video::parse(file,requested,error) && haze_graphics_apply(requested,false,error)) return;
    std::lock_guard<std::mutex> lock(state_mutex); state.notice=error;
    std::fprintf(stderr,"[graphics] Apply failed: %s\n",error.c_str());
}
