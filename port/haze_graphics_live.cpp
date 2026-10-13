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
std::atomic_flag operation=ATOMIC_FLAG_INIT;
struct Operation {
    bool acquired=!operation.test_and_set();
    ~Operation() { if (acquired) operation.clear(); }
};
struct Preview {
    HazeGraphicsState before;
    haze_video::Values configured;
    bool persist=false,failed=false;
    unsigned revision=0;
#ifdef _WIN32
    HWND window=nullptr;
    DWORD thread=0;
    ULONGLONG deadline=0;
#endif
} preview;
unsigned next_preview=0;
bool display_key(const std::string& key) { return key=="display_mode" || key=="monitor" || key=="window_size"; }
void publish(HazeGraphicsState value) {
    std::lock_guard<std::mutex> lock(state_mutex); state=std::move(value);
}
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
namespace {
bool target_values(const HazeGraphicsState& before,const haze_video::Values& configured,bool display,haze_video::Values& target,std::string& error) {
    error.clear();
    if (!haze_video::validate(configured,error)) return false;
    if (configured.size()!=haze_video::defaults().size()) { error="Incomplete video settings."; return false; }
    target=configured;
    for (const auto& [key,value]:before.overrides) target[key]=value;
    haze_display_defaults monitor{};
    if (!haze_display_detect(target.at("monitor").c_str(),monitor)) {
        error="The selected display is unavailable."; return false;
    }
    if (display && target.at("window_size")=="auto")
        target["window_size"]=std::to_string(monitor.window_width)+"x"+std::to_string(monitor.window_height);
    return true;
}
// The operation guard serializes writers, while readers only hold state_mutex
// for snapshots. Never hold that mutex through window/DXGI calls: synchronous
// window messages can query the caption's graphics notice on this same thread.
bool apply_values(const haze_video::Values& configured,bool persist,bool display,std::string& error) {
    auto result=haze_graphics_state(); haze_video::Values target;
    if (!target_values(result,configured,display,target,error)) return false;
    std::vector<std::string> restart;
    for (const auto& [key,value]:target) if (haze_video::restart_required(key) && !(display && display_key(key))) {
        // Startup resolves auto dimensions. Unchanged auto values continue to
        // describe the existing monitor selection; do not falsely flag them.
        if (value!=result.effective.at(key) && value!=startup_requested.at(key)) restart.push_back(key);
        target[key]=result.effective.at(key);
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
        if (!save(result.path,serialized,error)) { haze_video_finish(0); return false; }
    }
    haze_video_finish(1);
    haze_live_frame_limit=static_cast<int>(live.frame_limit);
    result.configured=configured; result.effective=std::move(target); result.restart=std::move(restart);
    if (persist) source_text=std::move(serialized); else read_text(result.path,source_text);
    result.notice=result.restart.empty() ? "Video settings applied" : "Video settings applied; some changes need restart";
    if (!result.overrides.empty()) result.notice+="; launch overrides active";
    std::fprintf(stderr,"[graphics] %s; preset=%s\n",result.notice.c_str(),haze_video::preset_name(configured).c_str());
    for (const auto& [key,value]:result.effective)
        std::fprintf(stderr,"[graphics] effective %s=%s%s\n",key.c_str(),value.c_str(),result.overrides.count(key) ? " (launch override)":"");
    publish(std::move(result));
    return true;
#else
    error="Live video settings require the native renderer."; return false;
#endif
}
bool revert_preview(const std::string& notice,std::string& error) {
    if (!haze_display_finish(false,error)) {
        preview.failed=true;
        auto result=haze_graphics_state(); result.display_restore_failed=true; result.display_seconds=0;
        result.notice=error; publish(std::move(result)); return false;
    }
    auto result=preview.before; result.notice=notice;
    preview={}; publish(std::move(result)); return true;
}
}
bool haze_graphics_apply(const haze_video::Values& configured,bool persist,std::string& error) {
    Operation lock;
    if (!lock.acquired) { error="Another video change is in progress."; return false; }
    if (preview.revision) { error="Confirm or revert the current display preview first."; return false; }
    return apply_values(configured,persist,false,error);
}
bool haze_graphics_preview(void* window,const haze_video::Values& configured,bool persist,std::string& error) {
    Operation lock;
    if (!lock.acquired) { error="Another video change is in progress."; return false; }
    if (preview.revision) { error="Confirm or revert the current display preview first."; return false; }
#ifdef _WIN32
    HWND hwnd=static_cast<HWND>(window);
    if (!IsWindow(hwnd) || GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId()) { error="Video settings must be applied on the game window thread."; return false; }
    const auto before=haze_graphics_state(); haze_video::Values target;
    if (!target_values(before,configured,true,target,error)) return false;
    bool changed=false;
    for (const auto* key:{"display_mode","monitor","window_size"}) if (target.at(key)!=before.effective.at(key)) changed=true;
    if (!changed) return apply_values(configured,persist,true,error);
    preview.before=before; preview.configured=configured; preview.persist=persist;
    preview.thread=GetCurrentThreadId(); preview.window=hwnd;
    const bool started=haze_display_begin(window,target.at("display_mode").c_str(),target.at("monitor").c_str(),target.at("window_size").c_str(),error);
    if (!started && !haze_display_pending()) { preview={}; return false; }
    preview.revision=++next_preview; if (!preview.revision) preview.revision=++next_preview;
    preview.failed=!started; preview.deadline=GetTickCount64()+15000;
    auto result=before; result.display_preview=preview.revision; result.display_restore_failed=!started; result.display_seconds=started ? 15 : 0;
    if (started) for (const auto* key:{"display_mode","monitor","window_size"}) result.effective[key]=target.at(key);
    result.notice=started ? "Previewing display. Keep within 15 seconds to save all changes." : error;
    publish(std::move(result)); return started;
#else
    (void)window; (void)configured; (void)persist; error="Display previews require the native Windows renderer."; return false;
#endif
}
bool haze_graphics_confirm(unsigned revision,bool keep,std::string& error) {
    Operation lock; error.clear();
    if (!lock.acquired) { error="Another video change is in progress."; return false; }
    if (!revision || revision!=preview.revision) { error="This display confirmation is no longer current."; return false; }
#ifdef _WIN32
    if (GetCurrentThreadId()!=preview.thread) { error="Display confirmation must run on the game window thread."; return false; }
    const bool expired=GetTickCount64()>=preview.deadline;
    const HWND foreground=GetForegroundWindow();
    const bool focused=foreground==preview.window || (foreground && GetAncestor(foreground,GA_ROOTOWNER)==preview.window);
    const bool inactive=!focused || IsIconic(preview.window) || !IsWindow(preview.window);
    if (!keep || expired || inactive) {
        const bool restored=revert_preview(expired ? "Display preview timed out. Previous settings restored." : "Display changes reverted. Settings were not saved.",error);
        if (keep && (expired || inactive) && restored) { error="Display confirmation expired or lost focus. Previous settings restored."; return false; }
        return restored;
    }
    if (preview.failed || !haze_display_can_keep()) { error="The display transition failed. Revert before continuing."; return false; }
    haze_video::Values checked;
    const auto visible=haze_graphics_state();
    bool unchanged=target_values(visible,preview.configured,true,checked,error);
    if (unchanged) for (const auto* key:{"display_mode","monitor","window_size"})
        if (checked.at(key)!=visible.effective.at(key)) { unchanged=false; error="The display changed during preview. Retry the settings change."; }
    if (!unchanged) {
        const auto failure=error; std::string rollback;
        if (!revert_preview(failure+" Display changes reverted.",rollback)) error=failure+" "+rollback;
        return false;
    }
    if (!apply_values(preview.configured,preview.persist,true,error)) {
        const auto failure=error; std::string rollback;
        if (!revert_preview(failure+" Display changes reverted.",rollback)) error=failure+" "+rollback;
        return false;
    }
    // Same guarded window-thread operation: this only discards the valid
    // snapshot and cannot touch resources or files after persistence succeeds.
    if (!haze_display_finish(true,error)) {
        preview.failed=true; auto result=haze_graphics_state(); result.display_restore_failed=true;
        result.notice=error; publish(std::move(result)); return false;
    }
    preview={}; auto result=haze_graphics_state(); result.display_preview=0; result.display_seconds=0; result.display_restore_failed=false;
    publish(std::move(result)); return true;
#else
    return false;
#endif
}
void haze_graphics_request_reload() { reload_requested=true; }
void haze_graphics_update() {
#ifdef _WIN32
    const auto pending_state=haze_graphics_state();
    if (pending_state.display_preview && !pending_state.display_restore_failed) {
        // This update is driven by the render loop, independently of menu
        // visibility. Losing focus or minimizing must never leave a bad mode.
        const HWND foreground=GetForegroundWindow();
        const bool focused=foreground==preview.window || (foreground && GetAncestor(foreground,GA_ROOTOWNER)==preview.window);
        std::string error;
        const auto now=GetTickCount64();
        if (now>=preview.deadline || !focused || IsIconic(preview.window) || !IsWindow(preview.window)) {
            if (!haze_graphics_confirm(pending_state.display_preview,false,error)) std::fprintf(stderr,"[graphics] Display revert failed: %s\n",error.c_str());
        } else {
            auto result=pending_state;
            result.display_seconds=unsigned((preview.deadline-now+999)/1000);
            publish(std::move(result));
        }
    }
#endif
    if (!reload_requested.exchange(false)) return;
    const auto current=haze_graphics_state();
    std::ifstream file(current.path); haze_video::Values requested; std::string error;
    if (!file) error="Could not open video settings.";
    else if (haze_video::parse(file,requested,error) && haze_graphics_apply(requested,false,error)) return;
    std::lock_guard<std::mutex> lock(state_mutex); state.notice=error;
    std::fprintf(stderr,"[graphics] Apply failed: %s\n",error.c_str());
}
