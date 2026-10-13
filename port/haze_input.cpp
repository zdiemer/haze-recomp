#include "haze_input.h"
#include "haze_graphics_reload.h"
#include "haze_menu.h"
#include "haze_audio_settings.h"
#include "haze_mouse.h"
#include "haze_capture.h"
#ifdef _WIN32
#include "haze_prompts.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "haze_window_icon.h"
#include "cellPad.h"
#include "cellSail.h"
#include "haze_startup.h"
#include "haze_startup_prompt.h"
#include "haze_input_key.h"
#include "haze_quit.h"
#include "haze_quit_menu.h"
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
std::atomic<bool> menu_return_pending{false};
std::atomic<bool> menu_pop{true};
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
std::atomic<bool> menu_active{false}, pause_requested{false};
ULONGLONG pause_request_time=0;
std::atomic<bool> suppress_mouse_buttons{false};
HazeCapturePolicy capture_policy;
std::mutex pointer_mutex;
HazeMenuPointer menu_pointer;
HazeMenuPointer menu_delivered;
float menu_press_x=0,menu_press_y=0;
bool menu_right_held=false;
POINT last_pointer{-1,-1};
void pointer_mode(bool active) {
    std::lock_guard lock(pointer_mutex);
    menu_pointer.active=active;
    if (!active) {
        menu_pointer.held=menu_pointer.pressed=menu_pointer.released=menu_pointer.back=false;
        menu_pointer.scroll=0;
        menu_right_held=false;
    }
}
void pointer_position(HWND hwnd) {
    POINT point; RECT client;
    if (!GetCursorPos(&point) || !ScreenToClient(hwnd,&point) ||
        !GetClientRect(hwnd,&client) || client.right<=0 || client.bottom<=0) return;
    std::lock_guard lock(pointer_mutex);
    menu_pointer.x=float(point.x)/client.right;
    menu_pointer.y=float(point.y)/client.bottom;
}
void pointer_event_position(HWND hwnd,LPARAM position) {
    RECT client;
    if (!GetClientRect(hwnd,&client) || client.right<=0 || client.bottom<=0) return;
    std::lock_guard lock(pointer_mutex);
    // Button coordinates belong to the queued event. GetCursorPos may already
    // be at the end of a drag when the window thread handles its press.
    menu_pointer.x=float(static_cast<short>(LOWORD(position)))/client.right;
    menu_pointer.y=float(static_cast<short>(HIWORD(position)))/client.bottom;
}
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
    const int code=haze_key_code(name);
    if (code>=0) return code;
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
    if (held && !(previous & KEY_HELD) && enabled) {
        last_family=HazeInputFamily::Keyboard;
        capture_policy.use_keyboard();
        if (vk>VK_XBUTTON2) pointer_mode(false);
    }
}
void update_title() {
    SetWindowTextA(window, captured ? "Haze | Mouse captured | Esc: Pause | F1: Release mouse"
        : enabled ? (menu_active ? "Haze | Mouse menu navigation | Right click: Back" :
            "Haze | Click/F1: Capture mouse | Esc: Pause") : "Haze | Controller");
}
void clip_cursor() {
    RECT rect;
    GetClientRect(window,&rect);
    MapWindowPoints(window,nullptr,(POINT*)&rect,2);
    ClipCursor(&rect);
}
void capture(bool on,bool restore_position=true) {
    if (on == (InterlockedCompareExchange(&captured,0,0) != 0)) return;
    if (on && (GetForegroundWindow() != window || menu_active || pause_requested || !enabled)) return;
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
        if (restore_position && GetForegroundWindow() == window) {
            // Restoring the cursor is not user mouse activity and must not
            // steal prompt/pointer ownership from a controller-opened menu.
            last_pointer=old_cursor;
            SetCursorPos(old_cursor.x,old_cursor.y);
        }
    }
    update_title();
    fprintf(stderr,"[input] Mouse %s (F1 toggles capture)\n",on ? "captured" : "released");
}
LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DPICHANGED: haze_window_icon(hwnd,LOWORD(wp)); break;
    case WM_SETTEXT:
        if (lp && std::strstr((const char*)lp," | FPS:")) {
            char caption[512];
            snprintf(caption,sizeof(caption),"%s | %s | F6: Reload video | %s",(const char*)lp,
                captured ? "Esc: Pause | F1: Release mouse"
                         : enabled ? (menu_active ? "Mouse menu navigation | Right click: Back" :
                             "Click/F1: Capture mouse | Esc: Pause") : "Controller",
                haze_graphics_notice().c_str());
            return CallWindowProc(original_proc,hwnd,msg,wp,(LPARAM)caption);
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (haze_quit_menu_key(int(wp),(lp&(1ll<<30))!=0)) { capture(false); return 0; }
        if (wp == VK_F6) { if (!(lp & (1ll<<30))) haze_graphics_request_reload(); return 0; }
        record_key((int)wp,true,GetTickCount64());
        if (!enabled) break;
        if (wp == VK_F1) {
            if (!(lp & (1ll<<30)) && !menu_active && !pause_requested) {
                capture_policy.focused=GetForegroundWindow()==hwnd;
                capture_policy.menu_state(menu_active);
                capture_policy.pause_pending=pause_requested;
                capture_policy.toggle(captured!=0);
                capture(!captured);
            }
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (!menu_active) { pause_request_time=GetTickCount64(); pause_requested=true; }
            capture(false); return 0;
        } // Pause, rather than SDK's quit.
        {
            std::lock_guard<std::mutex> lock(settings_mutex);
            for (const auto& b : bindings)
                if (b.button==CELL_PAD_CTRL_START && b.vk==(int)wp) {
                    if (!menu_active) { pause_request_time=GetTickCount64(); pause_requested=true; }
                    capture(false);
                }
        }
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        record_key((int)wp,false,GetTickCount64()); break;
    case WM_LBUTTONDOWN:
        if (!enabled) break;
        last_family=HazeInputFamily::Keyboard;
        capture_policy.use_keyboard();
        if (menu_active) {
            capture(false,false);
            SetCapture(hwnd); // Receive the end of a menu drag outside the window.
            pointer_event_position(hwnd,lp);
            std::lock_guard lock(pointer_mutex);
            menu_pointer.active=true;
            menu_pointer.held=menu_pointer.pressed=true;
            menu_press_x=menu_pointer.x; menu_press_y=menu_pointer.y;
            suppress_mouse_buttons=true;
            return 0;
        }
        if (startup_skip.showing()) {
            record_key(VK_LBUTTON,true,GetTickCount64());
            return 0;
        }
        if (!captured && GetForegroundWindow() == hwnd) {
            capture_click=true;
            capture_policy.manual_release=false;
            capture(true);
            return 0; // The click that captures must not fire the weapon.
        }
        record_key(VK_LBUTTON,true,GetTickCount64());
        break;
    case WM_LBUTTONUP:
        if (menu_active && enabled) {
            pointer_event_position(hwnd,lp);
            {
                std::lock_guard lock(pointer_mutex);
                menu_pointer.released=menu_pointer.held;
                menu_pointer.held=false;
            }
            if (!captured && GetCapture()==hwnd) ReleaseCapture();
            return 0;
        }
        record_key(VK_LBUTTON,false,GetTickCount64());
        capture_click=false; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        if (menu_active && enabled) {
            pointer_event_position(hwnd,lp);
            std::lock_guard lock(pointer_mutex);
            if (msg==WM_RBUTTONUP) menu_pointer.back=menu_right_held;
            menu_right_held=msg==WM_RBUTTONDOWN;
            if (menu_right_held) {
                menu_pointer.active=true;
                last_family=HazeInputFamily::Keyboard;
                capture_policy.use_keyboard();
            }
            suppress_mouse_buttons=true;
            return 0;
        }
        record_key(VK_RBUTTON,msg==WM_RBUTTONDOWN,GetTickCount64()); break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        record_key(VK_MBUTTON,msg==WM_MBUTTONDOWN,GetTickCount64()); break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        record_key(HIWORD(wp)==XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2,
                   msg==WM_XBUTTONDOWN,GetTickCount64()); break;
    case WM_CLOSE:
        clear_keys(); pointer_mode(false); capture(false);
        if (GetCapture()==hwnd) ReleaseCapture();
        suppress_mouse_buttons=true;
        if (haze_quit_window_close()) return 0;
        capture_policy.ready=false;
        break;
    case WM_KILLFOCUS: case WM_DESTROY:
        clear_keys(); pointer_mode(false); capture(false);
        if (GetCapture()==hwnd) ReleaseCapture();
        suppress_mouse_buttons=true;
        if (msg!=WM_KILLFOCUS) capture_policy.ready=false;
        break;
    case WM_CAPTURECHANGED:
        if (captured && (HWND)lp != hwnd) {
            capture_policy.manual_release=true;
            capture(false);
        } else if (!captured && (HWND)lp!=hwnd) {
            std::lock_guard lock(pointer_mutex);
            if (menu_pointer.held) {
                menu_pointer.held=menu_pointer.pressed=menu_pointer.released=false;
            }
        }
        break;
    case WM_MOVE: case WM_SIZE:
        if (captured) clip_cursor(); break;
    case WM_SETCURSOR:
        if (captured && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        if (enabled && menu_active && LOWORD(lp)==HTCLIENT) {
            SetCursor(LoadCursor(nullptr,IDC_ARROW)); return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        POINT position;
        if (!GetCursorPos(&position)) break;
        if (enabled && menu_active && GetForegroundWindow()==hwnd &&
            (position.x!=last_pointer.x || position.y!=last_pointer.y)) {
            pointer_event_position(hwnd,lp); pointer_mode(true);
            last_family=HazeInputFamily::Keyboard;
            capture_policy.use_keyboard();
        }
        last_pointer=position;
        break;
    }
    case WM_MOUSEWHEEL:
        if (enabled && menu_active) {
            pointer_position(hwnd);
            std::lock_guard lock(pointer_mutex);
            menu_pointer.active=true;
            menu_pointer.scroll+=GET_WHEEL_DELTA_WPARAM(wp);
            last_family=HazeInputFamily::Keyboard;
            capture_policy.use_keyboard();
            return 0;
        }
        if (captured) { InterlockedExchange(&wheel,1); last_family=HazeInputFamily::Keyboard; }
        break;
    case WM_INPUT: {
        RAWINPUT input{}; UINT size = sizeof(input);
        if (captured && !menu_active && !pause_requested && GetRawInputData((HRAWINPUT)lp,RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER)) == sizeof(input)
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
    if (haze_menu_active() || haze_quit_menu_active()) {
        *input=CellPadHostInput{0,128,128,128,128,0,0};
        clear_keys(); mouse_motion.clear(); InterlockedExchange(&wheel,0); return 1;
    }
    if (menu_return_pending.load()) {
        *input=CellPadHostInput{0,128,128,128,128,0,0}; clear_keys(); mouse_motion.clear();
        return 1; // Host polls cannot acknowledge delivery to the guest GUI.
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
    if (suppress_mouse_buttons) {
        bool held=false;
        for (int vk : {VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2})
            held|=(GetAsyncKeyState(vk)&0x8000)!=0;
        if (!held) suppress_mouse_buttons=false;
    }
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
        if (b.vk >= VK_LBUTTON && b.vk <= VK_XBUTTON2 &&
            ((!captured && !movie) || menu_active || pause_requested || (suppress_mouse_buttons && !movie))) continue;
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
    capture_policy.enabled=enabled;
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
    haze_window_icon(window);
    haze_video_menu_register();
    haze_audio_menu_register();
    haze_controls_menu_register();
    haze_steam_menu_register();
    if (controller_enabled) gamepad_initialized=haze_gamepad_initialize(config_path);
    RAWINPUTDEVICE device{0x01,0x02,0,window};
    if (!RegisterRawInputDevices(&device,1,sizeof(device))) {
        fprintf(stderr,"[input] Raw mouse registration failed: %lu\n",GetLastError()); return;
    }
    original_proc=(WNDPROC)SetWindowLongPtr(window,GWLP_WNDPROC,(LONG_PTR)window_proc);
    if (!original_proc) fprintf(stderr,"[input] Window input hook failed: %lu\n",GetLastError());
    capture_policy.ready=original_proc!=nullptr;
    capture_policy.focused=GetForegroundWindow()==window.load();
    capture_policy.menu_state(menu_active);
    capture(capture_policy.wants_capture());
    update_title();
}
bool haze_input_take_mouse(double& x, double& y, double& speed, bool& invert) {
    std::lock_guard<std::mutex> lock(settings_mutex);
    mouse_motion.take(x,y);
    speed=sensitivity; invert=invert_y;
    return enabled && captured && !haze_menu_active() && !haze_quit_menu_active() && !menu_return_pending &&
        !menu_active && !pause_requested && GetForegroundWindow()==window.load();
}
void haze_input_update() {
    const auto now=GetTickCount64();
    if (now<next_input_update) return;
    next_input_update=now+8;
    // A pause key during loading may be ignored by the guest. Give the menu
    // time to open, but do not leave capture disabled forever if it does not.
    // Live menu ownership still gates both capture and camera independently.
    if (pause_requested && !menu_active && now-pause_request_time>=1000)
        pause_requested=false;
    if (now>=next_config_check) {
        next_config_check=now+500;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (GetFileAttributesExA(config_path,GetFileExInfoStandard,&attributes) &&
            CompareFileTime(&config_time,&attributes.ftLastWriteTime)!=0) {
            haze_input_reload();
        }
    }
    HazeGamepadState latest=controller_state;
    bool sample_focused=controller_sample_focused;
    if (gamepad_initialized) {
        latest=haze_gamepad_update();
        const auto& state=latest;
        const bool focused=GetForegroundWindow()==window.load() || haze_menu_has_focus() || haze_quit_menu_has_focus();
        if (focused && state.connected && (!enabled || last_family!=HazeInputFamily::Keyboard))
            last_family=state.family;
        if (focused && state.activity) {
            last_family=state.family; capture_policy.use_controller();
            pointer_mode(false); capture(false);
        }
        sample_focused=focused;
    }
    capture_policy.focused=GetForegroundWindow()==window.load();
    capture_policy.menu_state(menu_active);
    capture_policy.pause_pending=pause_requested;
    capture(capture_policy.wants_capture());
    haze_prompts_update(last_family.load());
    if (haze_menu_active() || haze_quit_menu_active() || haze_quit_offer().available) capture(false);
    // Give menu actions first ownership of a new controller press. Publishing
    // it to the guest before opening Quit could also activate the retail page.
    haze_menu_update(window.load(),latest);
    haze_quit_menu_update(window.load(),latest);
    {
        std::lock_guard<std::mutex> lock(settings_mutex);
        controller_state=latest; controller_sample_focused=sample_focused;
    }
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
void haze_input_menu_state(bool active) {
    if (active) pause_requested=false;
    else menu_return_pending=false; // The guest already left its menu stack.
    if (menu_active.exchange(active)==active) return;
    mouse_motion.clear();
    InterlockedExchange(&wheel,0);
    suppress_mouse_buttons=true;
    capture_click=false;
    for (int vk : {VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2})
        message_keys[vk]=0;
    std::lock_guard lock(pointer_mutex);
    menu_pointer.held=menu_pointer.pressed=menu_pointer.released=menu_pointer.back=false;
    menu_right_held=false;
    menu_pointer.scroll=0;
}
HazeMenuPointer haze_input_take_pointer() {
    std::scoped_lock lock(settings_mutex,pointer_mutex);
    if (haze_menu_active() || haze_quit_menu_active() || menu_return_pending) {
        // This consumer runs after the guest's normal GUI input update, unlike
        // host pad polls (which GetInfo can consume before GetData is used).
        // Discard queued pointer clicks as well as pad input while the PC page
        // owns focus, then deliver exactly one Back through the GUI's packet.
        menu_pointer={}; menu_delivered={}; menu_right_held=false;
        if (haze_menu_active() || haze_quit_menu_active() || GetForegroundWindow()!=window.load() ||
            (gamepad_initialized && !controller_sample_focused)) return {};
        bool held=controller_state.input.buttons || controller_state.input.lx!=128 || controller_state.input.ly!=128 ||
            controller_state.input.rx!=128 || controller_state.input.ry!=128;
        auto key_held=[](int key) {
            return key>0 && key<256 && (GetAsyncKeyState(key)&0x8000);
        };
        // Wait only for inputs that can act in the menu or game. Scanning all
        // Windows keys can strand Back on an unrelated lock/system key.
        for (int key : {VK_ESCAPE,VK_RETURN,VK_SPACE,VK_TAB,VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,
                        VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2})
            held|=key_held(key);
        for (const auto& binding:bindings) held|=key_held(binding.vk);
        for (int key:movement) held|=key_held(key);
        if (held) return {};
        menu_return_pending=false;
        if (!menu_pop) return {}; // Game control options retains its retail page.
        HazeMenuPointer back; back.active=true; back.back=true;
        return back;
    }
    auto result=menu_pointer;
    result.active=(result.active || menu_pointer.scroll!=0) && enabled && menu_active &&
        GetForegroundWindow()==window.load();
    if (menu_pointer.pressed) {
        // A slow guest frame may see a whole click or drag at once. Deliver
        // the original press first so sliders retain the correct drag origin,
        // then the current position/release on the following GUI update.
        result.x=menu_press_x; result.y=menu_press_y;
        result.held=true; result.released=result.back=false; result.scroll=0;
        menu_pointer.pressed=false;
        menu_delivered=result;
        return result;
    }
    if (result.released && menu_delivered.held &&
        (result.x!=menu_delivered.x || result.y!=menu_delivered.y)) {
        // Native sliders commit the delta from their last held update on
        // release. Preserve a final drag sample even if move/up arrived together.
        result.held=true; result.released=result.back=false; result.scroll=0;
        menu_delivered=result;
        return result;
    }
    result.scroll=menu_pointer.scroll>=WHEEL_DELTA ? 1 : menu_pointer.scroll<=-WHEEL_DELTA ? -1 : 0;
    menu_pointer.scroll-=result.scroll*WHEEL_DELTA;
    if (result.scroll) menu_pointer.active=false; // Keep wheel selection until the pointer moves.
    menu_pointer.pressed=menu_pointer.released=menu_pointer.back=false;
    menu_delivered=result;
    return result;
}
HazeInputFamily haze_input_family() { return last_family.load(); }
const char* haze_input_config_path() { return config_path; }
void haze_input_reload() {
    std::lock_guard<std::mutex> lock(settings_mutex);
    load_keyboard_config();
    if (gamepad_initialized) haze_gamepad_configure(config_path);
    controller_state={}; controller_sample_focused=false;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExA(config_path,GetFileExInfoStandard,&attributes)) config_time=attributes.ftLastWriteTime;
    fprintf(stderr,"[input] Reloaded %s\n",config_path);
}
void haze_input_menu_return() { menu_pop=true; menu_return_pending=true; }
void haze_input_menu_retail() { menu_pop=false; menu_return_pending=true; }
unsigned haze_input_menu_button(int virtual_key) {
    std::lock_guard<std::mutex> lock(settings_mutex);
    last_family=HazeInputFamily::Keyboard;
    unsigned buttons=0;
    for (const auto& b:bindings) if (b.vk==virtual_key) buttons|=b.button;
    return buttons;
}
void haze_input_shutdown() {
    haze_quit_menu_shutdown();
    haze_menu_shutdown(true);
    startup_prompt.close();
    capture_policy.ready=false;
    capture(false);
    if (GetCapture()==window.load()) ReleaseCapture();
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
void haze_input_menu_state(bool) {}
HazeMenuPointer haze_input_take_pointer() { return {}; }
void haze_input_update() {}
void haze_input_shutdown() {}
HazeInputFamily haze_input_family() { return HazeInputFamily::Keyboard; }
#endif
