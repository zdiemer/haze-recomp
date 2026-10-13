#include "haze_input.h"
#include "haze_graphics_reload.h"
#include "haze_menu.h"
#include "haze_audio_settings.h"
#include "haze_mouse.h"
#ifdef _WIN32
#include "haze_prompts.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "cellPad.h"
#include "cellSail.h"
#include "haze_startup.h"
#include "haze_startup_prompt.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <atomic>
#include <mutex>

namespace {
std::atomic<HWND> window;
WNDPROC original_proc;
volatile LONG captured, wheel;
HazeMouseMotion mouse_motion;
POINT old_cursor;
bool enabled = true;
bool controller_enabled=true;
bool gamepad_initialized=false;
std::mutex settings_mutex;
HazeGamepadState controller_state;
bool controller_sample_focused=false;
std::atomic<int> menu_back_reports{0};
std::atomic<HazeInputFamily> last_family{HazeInputFamily::Keyboard};
char config_path[MAX_PATH]{};
ULONGLONG next_input_update=0, next_config_check=0;
FILETIME config_time{};
double sensitivity = 0.10;
bool invert_y;
HazeStartupSkip startup_skip;
ULONGLONG startup_epoch=0;
HazeStartupPrompt startup_prompt;
std::string skip_hint;
ULONGLONG next_hint_check=0;
HazeInputFamily hint_family=HazeInputFamily::Keyboard;
enum { KEY_HELD=1, KEY_PENDING=2 };
std::atomic<unsigned> message_keys[256];
std::atomic<bool> capture_click;
struct Binding { const char* name; const char* fallback; u16 button; int vk=0; };
Binding bindings[] = {
    {"Cross", "Space", CELL_PAD_CTRL_CROSS},
    {"Circle", "X", CELL_PAD_CTRL_CIRCLE},
    {"Square", "E", CELL_PAD_CTRL_SQUARE},
    {"Triangle", "R", CELL_PAD_CTRL_TRIANGLE},
    {"L1", "G", CELL_PAD_CTRL_L1}, {"R1", "Mouse1", CELL_PAD_CTRL_R1},
    {"L2", "Q", CELL_PAD_CTRL_L2}, {"R2", "F", CELL_PAD_CTRL_R2},
    {"L3", "Ctrl", CELL_PAD_CTRL_L3}, {"R3", "Mouse2", CELL_PAD_CTRL_R3},
    {"Start", "Enter", CELL_PAD_CTRL_START}, {"Select", "Tab", CELL_PAD_CTRL_SELECT},
    {"Up", "Up", CELL_PAD_CTRL_UP}, {"Down", "Down", CELL_PAD_CTRL_DOWN},
    {"Left", "Left", CELL_PAD_CTRL_LEFT}, {"Right", "Right", CELL_PAD_CTRL_RIGHT},
};
int movement[4] = {'W','S','A','D'};
int key_code(const char* name) {
    if (name[0] && !name[1]) return std::toupper((unsigned char)name[0]);
    const struct { const char* name; int vk; } names[] = {
        {"Space",VK_SPACE},{"Enter",VK_RETURN},{"Escape",VK_ESCAPE},{"Tab",VK_TAB},
        {"Shift",VK_SHIFT},{"Ctrl",VK_CONTROL},{"Alt",VK_MENU},
        {"Up",VK_UP},{"Down",VK_DOWN},{"Left",VK_LEFT},{"Right",VK_RIGHT},
        {"Mouse1",VK_LBUTTON},{"Mouse2",VK_RBUTTON},{"Mouse3",VK_MBUTTON},
        {"Mouse4",VK_XBUTTON1},{"Mouse5",VK_XBUTTON2},{"None",0},
    };
    for (const auto& n : names) if (!_stricmp(name,n.name)) return n.vk;
    fprintf(stderr,"[input] Unknown key '%s'; binding disabled\n",name);
    return 0;
}
bool down(int vk) {
    if (vk <= 0 || vk >= 256) return false;
    // Buffer an otherwise missed tap for one host report, rather than holding
    // released buttons for 120ms. The SDK caches that report between pad reads.
    const unsigned state=message_keys[vk].fetch_and(~unsigned(KEY_PENDING));
    return (state & (KEY_HELD|KEY_PENDING)) || (GetAsyncKeyState(vk) & 0x8000);
}
void clear_keys() {
    for (auto& key : message_keys) key=0;
    capture_click=false;
}
void record_key(int vk, bool held, ULONGLONG) {
    if (vk <= 0 || vk >= 256 || vk == VK_F1) return;
    unsigned previous=message_keys[vk].load();
    unsigned next;
    do {
        next=held ? previous | KEY_HELD | ((previous & KEY_HELD) ? 0u : KEY_PENDING)
                  : previous & ~unsigned(KEY_HELD);
    } while (!message_keys[vk].compare_exchange_weak(previous,next));
    if (held && !(previous & KEY_HELD) && enabled) last_family=HazeInputFamily::Keyboard;
}
void update_title() {
    SetWindowTextA(window, captured ? "Haze | Mouse captured | Esc: Pause | F1: Release mouse"
        : enabled ? "Haze | Click/F1: Capture mouse | Esc: Pause" : "Haze | Controller");
}
void clip_cursor() {
    RECT rect;
    GetClientRect(window,&rect);
    MapWindowPoints(window,nullptr,(POINT*)&rect,2);
    ClipCursor(&rect);
}
void capture(bool on) {
    if (on == (InterlockedCompareExchange(&captured,0,0) != 0)) return;
    if (on && GetForegroundWindow() != window) return;
    InterlockedExchange(&captured,on ? 1 : 0);
    mouse_motion.clear();
    InterlockedExchange(&wheel,0);
    if (on) {
        GetCursorPos(&old_cursor);
        SetCapture(window); clip_cursor();
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        ClipCursor(nullptr);
        if (GetCapture() == window) ReleaseCapture();
        while (ShowCursor(TRUE) < 0) {}
        if (GetForegroundWindow() == window) SetCursorPos(old_cursor.x,old_cursor.y);
    }
    update_title();
    fprintf(stderr,"[input] Mouse %s (F1 toggles capture)\n",on ? "captured" : "released");
}
LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETTEXT:
        if (lp && std::strstr((const char*)lp," | FPS:")) {
            char caption[512];
            snprintf(caption,sizeof(caption),"%s | %s | F6: Reload video | %s",(const char*)lp,
                captured ? "Esc: Pause | F1: Release mouse"
                         : enabled ? "Click/F1: Capture mouse | Esc: Pause" : "Controller",
                haze_graphics_notice().c_str());
            return CallWindowProc(original_proc,hwnd,msg,wp,(LPARAM)caption);
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_F6) { if (!(lp & (1ll<<30))) haze_graphics_request_reload(); return 0; }
        record_key((int)wp,true,GetTickCount64());
        if (!enabled) break;
        if (wp == VK_F1) { if (!(lp & (1ll<<30))) capture(!captured); return 0; }
        if (wp == VK_ESCAPE) { capture(false); return 0; } // Pause, rather than SDK's quit.
        {
            std::lock_guard<std::mutex> lock(settings_mutex);
            for (const auto& b : bindings)
                if (b.button==CELL_PAD_CTRL_START && b.vk==(int)wp) capture(false);
        }
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        record_key((int)wp,false,GetTickCount64()); break;
    case WM_LBUTTONDOWN:
        if (!enabled) break;
        last_family=HazeInputFamily::Keyboard;
        if (startup_skip.showing()) {
            record_key(VK_LBUTTON,true,GetTickCount64());
            return 0;
        }
        if (!captured && GetForegroundWindow() == hwnd) {
            capture_click=true;
            capture(true);
            return 0; // The click that captures must not fire the weapon.
        }
        record_key(VK_LBUTTON,true,GetTickCount64());
        break;
    case WM_LBUTTONUP:
        record_key(VK_LBUTTON,false,GetTickCount64());
        capture_click=false; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        record_key(VK_RBUTTON,msg==WM_RBUTTONDOWN,GetTickCount64()); break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        record_key(VK_MBUTTON,msg==WM_MBUTTONDOWN,GetTickCount64()); break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        record_key(HIWORD(wp)==XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2,
                   msg==WM_XBUTTONDOWN,GetTickCount64()); break;
    case WM_KILLFOCUS: case WM_CLOSE: case WM_DESTROY:
        clear_keys(); capture(false); break;
    case WM_CAPTURECHANGED:
        if (captured && (HWND)lp != hwnd) capture(false);
        break;
    case WM_MOVE: case WM_SIZE:
        if (captured) clip_cursor(); break;
    case WM_SETCURSOR:
        if (captured && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        break;
    case WM_MOUSEWHEEL:
        if (captured) { InterlockedExchange(&wheel,1); last_family=HazeInputFamily::Keyboard; }
        break;
    case WM_INPUT: {
        RAWINPUT input{}; UINT size = sizeof(input);
        if (captured && GetRawInputData((HRAWINPUT)lp,RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER)) == sizeof(input)
            && input.header.dwType == RIM_TYPEMOUSE && !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            mouse_motion.add(input.data.mouse.lLastX,input.data.mouse.lLastY);
            if (input.data.mouse.lLastX || input.data.mouse.lLastY) last_family=HazeInputFamily::Keyboard;
        }
        // DefWindowProc must see foreground raw input for system cleanup.
        return DefWindowProc(hwnd,msg,wp,lp);
    }
    }
    return CallWindowProc(original_proc,hwnd,msg,wp,lp);
}
int poll(u32 port, CellPadHostInput* input, void*) {
    if (port) return 0;
    std::lock_guard<std::mutex> lock(settings_mutex);
    ULONGLONG now = GetTickCount64();
    HWND foreground=GetForegroundWindow();
    const bool focused=foreground && foreground==window.load();
    if (haze_menu_active()) {
        *input=CellPadHostInput{0,128,128,128,128,0,0};
        clear_keys(); mouse_motion.clear(); InterlockedExchange(&wheel,0); return 1;
    }
    if (menu_back_reports.load()) {
        *input=CellPadHostInput{0,128,128,128,128,0,0}; clear_keys(); mouse_motion.clear();
        if (!focused) return 1;
        if (menu_back_reports==3) {
            bool held=controller_state.input.buttons || controller_state.input.lx!=128 || controller_state.input.ly!=128 ||
                controller_state.input.rx!=128 || controller_state.input.ry!=128;
            for (int key=1;key<256;++key) if (GetAsyncKeyState(key)&0x8000) held=true;
            if (!held) menu_back_reports=2;
            return 1;
        }
        input->buttons=CELL_PAD_CTRL_CIRCLE; --menu_back_reports; return 1;
    }
    auto finish=[&] {
        // A stale neutral controller snapshot from focus loss must not release
        // the transition gate before the window thread samples the held pad.
        const bool ready=focused && (!gamepad_initialized || controller_sample_focused);
        input->buttons=startup_skip.filter(now,input->buttons,ready,CELL_PAD_CTRL_CROSS);
        return 1;
    };
    if (!foreground || foreground != window.load()) {
        controller_sample_focused=false;
        mouse_motion.clear();
        InterlockedExchange(&wheel,0);
        clear_keys();
        return finish(); // Only lifecycle-owned automatic skipping may run unfocused.
    }
    if (controller_enabled && controller_state.connected) {
        *input=controller_state.input;
    }
    if (!enabled) return finish();
    // A rebound key may drive several buttons or movement. Consume its pending
    // tap once, then share the same snapshot across all bindings in this report.
    bool sampled[256]={}, states[256]={};
    const bool movie=startup_skip.showing();
    auto key_down=[&](int vk) {
        if (vk <= 0 || vk >= 256) return false;
        if (!sampled[vk]) { states[vk]=down(vk); sampled[vk]=true; }
        return states[vk];
    };
    for (const auto& b : bindings) {
        if (b.vk >= VK_LBUTTON && b.vk <= VK_XBUTTON2 && !captured && !movie) continue;
        if (b.vk == VK_LBUTTON && capture_click) continue;
        if (key_down(b.vk)) input->buttons |= b.button;
    }
    // Escape always pauses and releases mouse capture, even with Start rebound.
    if (key_down(VK_ESCAPE)) input->buttons |= CELL_PAD_CTRL_START;
    const int horizontal=(int)key_down(movement[3])-(int)key_down(movement[2]);
    const int vertical=(int)key_down(movement[1])-(int)key_down(movement[0]);
    if (horizontal) input->lx=(u8)(128+127*horizontal);
    if (vertical) input->ly=(u8)(128+127*vertical);
    if (captured) {
        if (InterlockedExchange(&wheel,0)) input->buttons |= CELL_PAD_CTRL_CIRCLE;
    }
    if ((input->buttons & CELL_PAD_CTRL_L2) && !input->l2) input->l2=255;
    if ((input->buttons & CELL_PAD_CTRL_R2) && !input->r2) input->r2=255;
    return finish();
}
void playback(u32 player, const char* uri, int started, void*) {
    const auto now=GetTickCount64();
    startup_skip.playback(player,uri,started!=0,now);
    if (HazeStartupSkip::known_video(uri))
        fprintf(stderr,"[startup] %s %s at +%llu ms: %s\n",
                HazeStartupSkip::opening_logo(uri) ? "logo" : "attract",
                started ? "start" : "stop",(unsigned long long)(now-startup_epoch),uri);
}
bool startup_setting(const char* name) {
    char value[32];
    GetPrivateProfileStringA("Startup",name,"1",value,sizeof(value),config_path);
    if (!strcmp(value,"0")) return false;
    if (strcmp(value,"1")) fprintf(stderr,"[startup] Invalid %s=%s; using 1\n",name,value);
    return true;
}
void load_keyboard_config() {
    const char* path=config_path;
    for (auto& b : bindings) {
        char value[32];
        GetPrivateProfileStringA("Bindings",b.name,b.fallback,value,sizeof(value),path);
        b.vk=key_code(value);
    }
    const char* movement_names[]={"Forward","Backward","StrafeLeft","StrafeRight"};
    const char* movement_defaults[]={"W","S","A","D"};
    for (int i=0;i<4;i++) {
        char value[32];
        GetPrivateProfileStringA("Bindings",movement_names[i],movement_defaults[i],value,sizeof(value),path);
        movement[i]=key_code(value);
    }
    char value[32];
    GetPrivateProfileStringA("Mouse","Sensitivity","0.10",value,sizeof(value),path);
    char* end=nullptr;
    double requested=strtod(value,&end);
    sensitivity=.10;
    if (end!=value && !*end && std::isfinite(requested) && requested > 0 && requested <= 10) sensitivity=requested;
    else fprintf(stderr,"[input] Invalid Mouse.Sensitivity=%s; using 0.10\n",value);
    invert_y=GetPrivateProfileIntA("Mouse","InvertY",0,path)!=0;
    const bool logos=startup_setting("AutoSkipLogos"), demo=startup_setting("AutoSkipAttract");
    startup_skip.configure(logos,demo);
    fprintf(stderr,"[startup] Automatic logos=%s, idle demo=%s; movies are not decoded. Confirm skips manually.\n",
            logos ? "on" : "off",demo ? "on" : "off");
    clear_keys(); mouse_motion.clear();
}
}
void haze_input_initialize() {
    startup_epoch=GetTickCount64();
    const char* mode=getenv("HAZE_INPUT");
    enabled=!(mode && !_stricmp(mode,"controller"));
    controller_enabled=!(mode && !_stricmp(mode,"keyboard"));
    cellSail_set_host_playback_callback(playback,nullptr);
    cellPad_set_host_input_callback(poll,nullptr);
    const char* config=getenv("HAZE_INPUT_CONFIG");
    GetFullPathNameA(config && *config ? config : "config/input.ini",MAX_PATH,config_path,nullptr);
    if ((!config || !*config) && GetFileAttributesA(config_path)==INVALID_FILE_ATTRIBUTES) {
        char executable[MAX_PATH]{};
        GetModuleFileNameA(nullptr,executable,MAX_PATH);
        if (const char* slash=std::strrchr(executable,'\\'))
            snprintf(config_path,sizeof(config_path),"%.*s\\config\\input.ini",int(slash-executable),executable);
    }
    haze_prompts_initialize(config_path);
    std::lock_guard<std::mutex> lock(settings_mutex);
    load_keyboard_config();
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExA(config_path,GetFileExInfoStandard,&attributes)) config_time=attributes.ftLastWriteTime;
    fprintf(stderr,"[input] %s input; config=%s (reloads while running)\n",
            !enabled ? "controller" : !controller_enabled ? "keyboard/mouse" : "automatic keyboard/mouse + controller",config_path);
}
void haze_input_attach(void* hwnd) {
    if (!hwnd) return;
    window=(HWND)hwnd;
    haze_video_menu_register();
    haze_audio_menu_register();
    if (controller_enabled) gamepad_initialized=haze_gamepad_initialize(config_path);
    RAWINPUTDEVICE device{0x01,0x02,0,window};
    if (!RegisterRawInputDevices(&device,1,sizeof(device))) {
        fprintf(stderr,"[input] Raw mouse registration failed: %lu\n",GetLastError()); return;
    }
    original_proc=(WNDPROC)SetWindowLongPtr(window,GWLP_WNDPROC,(LONG_PTR)window_proc);
    if (!original_proc) fprintf(stderr,"[input] Window input hook failed: %lu\n",GetLastError());
    update_title();
}
bool haze_input_take_mouse(double& x, double& y, double& speed, bool& invert) {
    std::lock_guard<std::mutex> lock(settings_mutex);
    mouse_motion.take(x,y);
    speed=sensitivity; invert=invert_y;
    return enabled && captured && !haze_menu_active() && !menu_back_reports && GetForegroundWindow()==window.load();
}
void haze_input_update() {
    const auto now=GetTickCount64();
    if (now<next_input_update) return;
    next_input_update=now+8;
    if (now>=next_config_check) {
        next_config_check=now+500;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (GetFileAttributesExA(config_path,GetFileExInfoStandard,&attributes) &&
            CompareFileTime(&config_time,&attributes.ftLastWriteTime)!=0) {
            std::lock_guard<std::mutex> lock(settings_mutex);
            load_keyboard_config();
            if (gamepad_initialized) haze_gamepad_configure(config_path);
            config_time=attributes.ftLastWriteTime;
            fprintf(stderr,"[input] Reloaded %s\n",config_path);
        }
    }
    if (gamepad_initialized) {
        const auto state=haze_gamepad_update();
        const bool focused=GetForegroundWindow()==window.load() || haze_menu_has_focus();
        if (focused && state.connected && (!enabled || last_family!=HazeInputFamily::Keyboard))
            last_family=state.family;
        if (focused && state.activity) { last_family=state.family; capture(false); }
        std::lock_guard<std::mutex> lock(settings_mutex);
        controller_state=state;
        controller_sample_focused=focused;
    }
    haze_prompts_update(last_family.load());
    if (haze_menu_active()) capture(false);
    haze_menu_update(window.load(),controller_state);
    if (startup_skip.showing()) {
        const auto family=last_family.load();
        if (now>=next_hint_check || family!=hint_family) {
            hint_family=family; next_hint_check=now+500;
            char value[128];
            const bool kb=family==HazeInputFamily::Keyboard;
            GetPrivateProfileStringA(kb ? "Bindings" : "ControllerBindings","Cross",
                                     kb ? "Space" : "south",value,sizeof(value),config_path);
            const char* label=value;
            if (!kb) {
                const char* positions[]={"south","east","west","north","start","back"};
                const char* xbox[]={"A","B","X","Y","Menu","View"};
                const char* ps[]={"Cross","Circle","Square","Triangle","Start / Options","Select / Share"};
                const char* sw[]={"B","A","Y","X","+","-"};
                for (int i=0;i<6;i++) if (!_stricmp(value,positions[i]))
                    label=family==HazeInputFamily::Xbox ? xbox[i] : family==HazeInputFamily::Switch ? sw[i] : ps[i];
            }
            skip_hint=!_stricmp(value,"none") ? "Skip is unbound. Enable automatic logo skipping in input.ini."
                : std::string("Press ")+label+" to skip (movie playback unavailable)";
        }
    }
    startup_prompt.update(window.load(),startup_skip.showing(),skip_hint);
}
HazeInputFamily haze_input_family() { return last_family.load(); }
void haze_input_menu_return() { menu_back_reports=3; }
unsigned haze_input_menu_button(int virtual_key) {
    std::lock_guard<std::mutex> lock(settings_mutex);
    last_family=HazeInputFamily::Keyboard;
    unsigned buttons=0;
    for (const auto& b:bindings) if (b.vk==virtual_key) buttons|=b.button;
    return buttons;
}
void haze_input_shutdown() {
    haze_menu_shutdown();
    startup_prompt.close();
    capture(false);
    haze_gamepad_shutdown(); gamepad_initialized=false;
}
#elif defined(__EMSCRIPTEN__)
#include "cellPad.h"
#include "../external/ps3recomp/libs/codec/cellSail.h"
#include "haze_startup.h"
#include "win32_compat.h"
#include <emscripten/emscripten.h>
#include <atomic>
namespace {
std::atomic<uint32_t> browser_buttons{0}, browser_axes{0x80808080};
std::atomic<bool> browser_mouse_captured{false};
HazeMouseMotion browser_mouse_motion;
HazeStartupSkip browser_startup;
int browser_poll(u32 port, CellPadHostInput* input, void*) {
    if (port) return 0;
    const uint32_t buttons = browser_buttons.load();
    const uint32_t axes = browser_axes.load();
    *input = {(u16)buttons, (u8)axes, (u8)(axes >> 8), (u8)(axes >> 16), (u8)(axes >> 24),
              (u8)((buttons & CELL_PAD_CTRL_L2) ? 255 : 0), (u8)((buttons & CELL_PAD_CTRL_R2) ? 255 : 0)};
    input->buttons=browser_startup.filter(GetTickCount64(),input->buttons,true,CELL_PAD_CTRL_CROSS);
    return 1;
}
void browser_playback(u32 player, const char* uri, int started, void*) {
    browser_startup.playback(player, uri, started != 0, GetTickCount64());
}
}
extern "C" EMSCRIPTEN_KEEPALIVE void haze_wasm_input(uint32_t buttons, uint32_t axes) {
    browser_buttons.store(buttons); browser_axes.store(axes);
}
extern "C" EMSCRIPTEN_KEEPALIVE void haze_wasm_mouse(int32_t x, int32_t y, int captured) {
    if (browser_mouse_captured.exchange(captured != 0) != (captured != 0) || !captured)
        browser_mouse_motion.clear();
    if (captured) browser_mouse_motion.add(x,y);
}
void haze_input_initialize() {
    cellPad_set_host_input_callback(browser_poll, nullptr);
    cellSail_set_host_playback_callback(browser_playback, nullptr);
}
void haze_input_attach(void*) {}
bool haze_input_take_mouse(double& x,double& y,double& speed,bool& invert) {
    browser_mouse_motion.take(x,y);
    speed=0.10; invert=false;
    return browser_mouse_captured.load();
}
#else
void haze_input_initialize() {}
void haze_input_attach(void*) {}
bool haze_input_take_mouse(double&,double&,double&,bool&) { return false; }
#endif

#ifndef _WIN32
void haze_input_update() {}
void haze_input_shutdown() {}
HazeInputFamily haze_input_family() { return HazeInputFamily::Keyboard; }
#endif
