// Run the actual port mapper with controlled host keys, focus and mouse events.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <string>
#include <fstream>
static bool focused=true;
static bool keys[256];
static ULONGLONG ticks=100;
static unsigned graphics_reloads,overlay_cycles;
void haze_graphics_request_reload() { ++graphics_reloads; }
void haze_graphics_request_overlay_cycle() { ++overlay_cycles; }
std::string haze_graphics_notice() { return {}; }
static SHORT test_key(int vk) { return keys[vk] ? (SHORT)0x8000 : 0; }
static HWND test_foreground() { return focused ? (HWND)1 : nullptr; }
static DWORD test_owner(HWND hwnd,DWORD* pid) { *pid=hwnd ? GetCurrentProcessId() : 0; return 1; }
static ULONGLONG test_time() { ticks+=8; return ticks; }
static UINT forwarded_message;
static std::string forwarded_caption;
static RAWINPUT raw_packet;
static HWND mouse_capture;
static int cursor_count=0;
static unsigned capture_calls=0;
static POINT cursor_position{640,360};
static LONG client_width=1280,client_height=720;
static HWND test_capture(HWND h) { auto old=mouse_capture; mouse_capture=h; ++capture_calls; return old; }
static HWND test_get_capture() { return mouse_capture; }
static BOOL test_release_capture() { mouse_capture=nullptr; return TRUE; }
static int test_show_cursor(BOOL show) { return cursor_count+=show ? 1 : -1; }
static BOOL test_clip(const RECT*) { return TRUE; }
static BOOL test_cursor(POINT* point) { *point=cursor_position; return TRUE; }
static BOOL test_set_cursor(int x,int y) { cursor_position={x,y}; return TRUE; }
static BOOL test_client(HWND,RECT* r) { *r={0,0,client_width,client_height}; return TRUE; }
static BOOL test_screen_to_client(HWND,POINT*) { return TRUE; }
static int test_map(HWND,HWND,POINT*,UINT) { return 0; }
static BOOL test_title(HWND,LPCSTR) { return TRUE; }
static BOOL test_register(PCRAWINPUTDEVICE,UINT,UINT) { return TRUE; }
static LONG_PTR test_subclass(HWND,int,LONG_PTR) { return 1; }
static UINT test_raw_input(HRAWINPUT,UINT,LPVOID data,PUINT size,UINT) {
    assert(*size==sizeof(raw_packet));
    memcpy(data,&raw_packet,sizeof(raw_packet)); return sizeof(raw_packet);
}
static LRESULT test_default_proc(HWND,UINT,WPARAM,LPARAM) { return 0; }
static LRESULT test_window_proc(WNDPROC,HWND,UINT msg,WPARAM,LPARAM lp) {
    forwarded_message=msg;
    if (msg==WM_SETTEXT) forwarded_caption=(const char*)lp;
    return 0;
}
#define GetAsyncKeyState test_key
#define GetForegroundWindow test_foreground
#define GetWindowThreadProcessId test_owner
#define GetTickCount64 test_time
#undef CallWindowProc
#define CallWindowProc test_window_proc
#define GetRawInputData test_raw_input
#define SetCapture test_capture
#define GetCapture test_get_capture
#define ReleaseCapture test_release_capture
#define ShowCursor test_show_cursor
#define ClipCursor test_clip
#define GetCursorPos test_cursor
#define SetCursorPos test_set_cursor
#define GetClientRect test_client
#define ScreenToClient test_screen_to_client
#define MapWindowPoints test_map
#define SetWindowTextA test_title
#define RegisterRawInputDevices test_register
#undef SetWindowLongPtr
#define SetWindowLongPtr test_subclass
#undef DefWindowProc
#define DefWindowProc test_default_proc
#include "../port/haze_input.cpp"
extern "C" void cellPad_set_host_input_callback(CellPadHostInputCallback,void*) {}
extern "C" void cellSail_set_host_playback_callback(CellSailHostPlaybackCallback,void*) {}
static HazeGamepadState fake_pad;
bool haze_gamepad_initialize(const char*) { return true; }
void haze_gamepad_configure(const char*) {}
HazeGamepadState haze_gamepad_update() { return fake_pad; }
void haze_gamepad_shutdown() {}
void haze_prompts_initialize(const char*) {}
void haze_prompts_update(HazeInputFamily) {}
static bool native_menu_active;
bool haze_menu_active() { return native_menu_active; }
bool haze_menu_blocks_input() { return native_menu_active; }
bool haze_menu_capture_key(int) { return false; }
bool haze_menu_has_focus() { return false; }
void haze_menu_update(void*,const HazeGamepadState&) {}
static bool menu_exiting_process;
void haze_menu_shutdown(bool exiting_process) { menu_exiting_process=exiting_process; }
void haze_video_menu_register() {}
void haze_audio_menu_register() {}
void haze_controls_menu_register() {}
void haze_steam_menu_register() {}
void haze_quit_menu_register() {}
static bool quit_active=false,close_accepted=false;
bool haze_quit_menu_active() { return quit_active; }
bool haze_quit_menu_has_focus() { return false; }
bool haze_quit_menu_key(int,bool) { return false; }
void haze_quit_menu_update(void*,const HazeGamepadState&) {}
void haze_quit_menu_shutdown() {}
HazeQuitOffer haze_quit_offer() { return {}; }
bool haze_quit_window_close() { return close_accepted; }
static CellPadHostInput sample() {
    CellPadHostInput input{0,128,128,128,128,0,0};
    assert(poll(0,&input,nullptr)==1);
    return input;
}
int main() {
    window_proc((HWND)1,WM_KEYDOWN,VK_F6,0);
    window_proc((HWND)1,WM_KEYDOWN,VK_F6,1ll<<30);
    assert(graphics_reloads==1 && message_keys[VK_F6]==0);
    const auto captures_before_overlay=capture_calls;
    window_proc((HWND)1,WM_KEYDOWN,VK_F7,0);
    window_proc((HWND)1,WM_KEYDOWN,VK_F7,1ll<<30);
    assert(overlay_cycles==1 && message_keys[VK_F7]==0 && capture_calls==captures_before_overlay);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_CALL_REPORTFAULT);
    window=(HWND)1;
    for (auto& b : bindings) b.vk=key_code(b.fallback);
    assert(key_code("w")=='W'); assert(key_code("Mouse2")==VK_RBUTTON);
    assert(sample().lx==128);
    native_menu_active=true; keys['W']=keys[VK_SPACE]=keys[VK_LBUTTON]=true;
    auto blocked=sample(); assert(!blocked.buttons && blocked.lx==128 && blocked.ly==128);
    native_menu_active=false; keys[VK_NUMLOCK]=keys[VK_CAPITAL]=keys[VK_SCROLL]=true;
    haze_input_menu_return();
    assert(!sample().buttons && menu_return_pending);
    assert(!haze_input_take_pointer().back); // Closing keys must be released.
    keys['W']=keys[VK_SPACE]=keys[VK_LBUTTON]=false;
    for (int i=0;i<100;++i) assert(!sample().buttons);
    assert(menu_return_pending); // Arbitrary GetInfo/backend polls cannot lose Back.
    focused=false;
    assert(!haze_input_take_pointer().back && menu_return_pending);
    focused=true; gamepad_initialized=true; controller_sample_focused=false;
    assert(!haze_input_take_pointer().back && menu_return_pending);
    controller_sample_focused=true;
    auto back=haze_input_take_pointer();
    assert(back.active && back.back && !back.pressed && !back.held && !back.released);
    assert(!menu_return_pending && !haze_input_take_pointer().back);
    // Lock keys are unrelated to the close action and must not strand Back.
    assert(keys[VK_NUMLOCK] && keys[VK_CAPITAL] && keys[VK_SCROLL]);
    keys[VK_NUMLOCK]=keys[VK_CAPITAL]=keys[VK_SCROLL]=false;
    gamepad_initialized=false;
    haze_input_menu_return(); haze_input_menu_state(false);
    assert(!menu_return_pending && !haze_input_take_pointer().back);
    assert(!sample().buttons);
    keys[VK_SPACE]=true; haze_input_menu_retail();
    assert(!sample().buttons && menu_return_pending);
    assert(!haze_input_take_pointer().back && menu_return_pending);
    keys[VK_SPACE]=false;
    assert(!haze_input_take_pointer().back && !menu_return_pending);
    assert(!sample().buttons); // Retail handoff never injects Back.
    quit_active=true; keys['W']=keys[VK_SPACE]=true;
    menu_pointer.active=menu_pointer.released=menu_pointer.back=true;
    auto quit_pointer=haze_input_take_pointer();
    assert(!quit_pointer.active && !quit_pointer.released && !quit_pointer.back);
    blocked=sample(); assert(!blocked.buttons && blocked.ly==128);
    double quit_x,quit_y,quit_speed; bool quit_invert;
    assert(!haze_input_take_mouse(quit_x,quit_y,quit_speed,quit_invert));
    quit_active=false; keys['W']=keys[VK_SPACE]=false;
    haze_input_menu_retail(); assert(!sample().buttons && menu_return_pending);
    assert(!haze_input_take_pointer().back && !menu_return_pending);
    close_accepted=true; forwarded_message=0; window_proc(window,WM_CLOSE,0,0);
    assert(forwarded_message!=WM_CLOSE);
    quit_active=true;
    for (int i=0;i<12;++i) {
        const auto pause=sample();
        assert(pause.buttons==CELL_PAD_CTRL_START && pause.lx==128 && pause.ly==128);
    }
    assert(!sample().buttons); // One held Start burst, never repeated toggles.
    window_proc(window,WM_CLOSE,0,0); menu_active=true;
    assert(!sample().buttons && !quit_pause_polls); // Stop once a native menu opens.
    menu_active=false; window_proc(window,WM_CLOSE,0,0); quit_active=false;
    assert(!sample().buttons && !quit_pause_polls); // Cancel drops pending injection.
    close_accepted=false; window_proc(window,WM_CLOSE,0,0); assert(forwarded_message==WM_CLOSE);
    keys['W']=keys['D']=keys[VK_SPACE]=true;
    auto p=sample(); assert(p.lx==255 && p.ly==1 && (p.buttons&CELL_PAD_CTRL_CROSS));
    keys['S']=keys['A']=true;
    p=sample(); assert(p.lx==128 && p.ly==128);
    keys[VK_LBUTTON]=true;
    assert(!(sample().buttons&CELL_PAD_CTRL_R1)); // A UI click does not fire.
    InterlockedExchange(&captured,1);
    raw_packet.header.dwType=RIM_TYPEMOUSE;
    raw_packet.data.mouse.lLastX=8; raw_packet.data.mouse.lLastY=-8;
    window_proc(window,WM_INPUT,0,1);
    p=sample(); assert((p.buttons&CELL_PAD_CTRL_R1) && p.rx==128 && p.ry==128);
    sample(); // Pad polling cannot consume or convert camera motion.
    double dx,dy,speed; bool inverse;
    assert(haze_input_take_mouse(dx,dy,speed,inverse) && dx==8 && dy==-8);
    assert(haze_input_take_mouse(dx,dy,speed,inverse) && dx==0 && dy==0);
    raw_packet.data.mouse.usFlags=MOUSE_MOVE_ABSOLUTE;
    window_proc(window,WM_INPUT,0,1);
    assert(haze_input_take_mouse(dx,dy,speed,inverse) && dx==0 && dy==0);
    raw_packet.data.mouse.usFlags=0;
    keys[VK_RBUTTON]=true; keys[VK_CONTROL]=true; keys['R']=true;
    p=sample(); assert((p.buttons&CELL_PAD_CTRL_R3) && (p.buttons&CELL_PAD_CTRL_L3) &&
                       (p.buttons&CELL_PAD_CTRL_TRIANGLE));
    keys[VK_RBUTTON]=false; keys[VK_CONTROL]=false; keys['R']=false;
    p=sample(); assert(p.rx==128 && p.ry==128); // No residual camera drift.
    InterlockedExchange(&wheel,1);
    assert(sample().buttons&CELL_PAD_CTRL_CIRCLE);
    assert(!(sample().buttons&CELL_PAD_CTRL_CIRCLE));
    focused=false; mouse_motion.add(100,0);
    p=sample(); assert(!p.buttons && p.lx==128 && p.ly==128 && p.rx==128);
    focused=true; memset(keys,0,sizeof(keys));
    assert(sample().rx==128); // Focus loss discarded pending motion.
    assert(haze_input_take_mouse(dx,dy,speed,inverse) && dx==0 && dy==0);
    invert_y=true; mouse_motion.add(0,8);
    assert(sample().ry==128);
    assert(haze_input_take_mouse(dx,dy,speed,inverse) && dy==8 && inverse);
    CellPadHostInput other{}; assert(poll(1,&other,nullptr)==0);
    captured=0; invert_y=false;
    // Physical key state can lead queued window messages. Menus must not
    // report the physical tap and then replay its queued press as a second tap.
    haze_input_menu_state(true);
    for (int vk : {VK_UP,VK_DOWN,VK_SPACE,int('X')}) {
        clear_keys(); keys[vk]=true;
        assert(!sample().buttons);
        keys[vk]=false;
        assert(!sample().buttons);
        window_proc(window,WM_KEYDOWN,vk,0);
        window_proc(window,WM_KEYUP,vk,0);
        const auto tap=sample().buttons;
        assert(tap && !(tap&(tap-1)));
        assert(!sample().buttons);
    }
    // Hold stays continuous; Windows auto-repeat must not add press edges.
    window_proc(window,WM_KEYDOWN,VK_DOWN,0);
    for (int i=0;i<100;++i) {
        window_proc(window,WM_KEYDOWN,VK_DOWN,1ll<<30);
        assert(sample().buttons==CELL_PAD_CTRL_DOWN);
    }
    window_proc(window,WM_KEYUP,VK_DOWN,0);
    assert(!sample().buttons);
    // A key still held after a transition cannot reactivate the next menu.
    window_proc(window,WM_KEYDOWN,VK_SPACE,0);
    clear_keys();
    window_proc(window,WM_KEYDOWN,VK_SPACE,1ll<<30);
    assert(!sample().buttons);
    window_proc(window,WM_KEYUP,VK_SPACE,0);
    haze_input_menu_state(false); sample();
    // A quick tap between pad polls reaches one report, without stretching
    // a released confirm across subsequent menus.
    window_proc(window,WM_KEYDOWN,VK_SPACE,0);
    window_proc(window,WM_KEYUP,VK_SPACE,0);
    assert(sample().buttons&CELL_PAD_CTRL_CROSS);
    assert(!(sample().buttons&CELL_PAD_CTRL_CROSS));
    // Rebinding one key to multiple actions shares a single tap snapshot.
    const int circle_key=bindings[1].vk;
    bindings[1].vk=VK_SPACE;
    window_proc(window,WM_KEYDOWN,VK_SPACE,0);
    window_proc(window,WM_KEYUP,VK_SPACE,0);
    p=sample();
    assert((p.buttons & (CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE)) ==
           (CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE));
    assert(!(sample().buttons & (CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE)));
    bindings[1].vk=circle_key;
    // Long holds stay held; the release is not followed by a buffered echo.
    window_proc(window,WM_KEYDOWN,VK_SPACE,0);
    for (int i=0;i<80;++i) assert(sample().buttons&CELL_PAD_CTRL_CROSS);
    window_proc(window,WM_KEYUP,VK_SPACE,0);
    assert(!(sample().buttons&CELL_PAD_CTRL_CROSS));
    // A press sampled while held releases immediately, including an OS repeat.
    window_proc(window,WM_KEYDOWN,VK_SPACE,0);
    assert(sample().buttons&CELL_PAD_CTRL_CROSS);
    window_proc(window,WM_KEYDOWN,VK_SPACE,1ll<<30);
    assert(sample().buttons&CELL_PAD_CTRL_CROSS);
    window_proc(window,WM_KEYUP,VK_SPACE,0);
    assert(!(sample().buttons&CELL_PAD_CTRL_CROSS));
    // Separate quick taps still each produce a new press.
    window_proc(window,WM_KEYDOWN,VK_SPACE,0);
    window_proc(window,WM_KEYUP,VK_SPACE,0);
    assert(sample().buttons&CELL_PAD_CTRL_CROSS);
    assert(!(sample().buttons&CELL_PAD_CTRL_CROSS));
    window_proc(window,WM_KEYDOWN,'W',0);
    window_proc(window,WM_KILLFOCUS,0,0);
    assert(sample().ly==128); // Focus loss cannot leave a held message key.
    forwarded_message=0;
    window_proc(window,WM_KEYDOWN,VK_ESCAPE,0);
    assert(forwarded_message==0 && (sample().buttons&CELL_PAD_CTRL_START));
    window_proc(window,WM_KEYUP,VK_ESCAPE,0); ticks+=121;
    assert(!(sample().buttons&CELL_PAD_CTRL_START));
    haze_input_menu_state(true); haze_input_menu_state(false);
    sample(); // Guest pause/resume consumes the pending pause and release gate.
    window_proc(window,WM_SETTEXT,0,(LPARAM)"Haze | FPS: 30.00");
    assert(forwarded_caption.find("FPS: 30.00")!=std::string::npos &&
           forwarded_caption.find("F1: Capture mouse")!=std::string::npos);
    keys[VK_LBUTTON]=true; captured=1; capture_click=true;
    assert(!(sample().buttons&CELL_PAD_CTRL_R1));
    capture_click=false;
    assert(sample().buttons&CELL_PAD_CTRL_R1);
    memset(keys,0,sizeof(keys)); captured=0;
    invert_y=true; captured=1; mouse_motion.add(LONG_MIN,LONG_MAX);
    mouse_motion.add(LONG_MIN,LONG_MAX);
    assert(sample().ry==128);
    assert(haze_input_take_mouse(dx,dy,speed,inverse));
    assert(dx==double(LONG_MIN)*2 && dy==double(LONG_MAX)*2);
    // Menus reject both F1 capture and gameplay clicks, even during the delay
    // before the window thread releases capture. Release activates GUI once.
    haze_input_menu_state(true);
    mouse_motion.add(900,600);
    window_proc(window,WM_KEYDOWN,VK_F1,0);
    assert(!haze_input_take_mouse(dx,dy,speed,inverse));
    keys[VK_LBUTTON]=true;
    window_proc(window,WM_LBUTTONDOWN,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    assert(!(sample().buttons&CELL_PAD_CTRL_R1));
    auto pointer_packet=haze_input_take_pointer();
    assert(pointer_packet.active && pointer_packet.pressed && pointer_packet.held);
    window_proc(window,WM_LBUTTONUP,0,MAKELPARAM(cursor_position.x,cursor_position.y)); keys[VK_LBUTTON]=false;
    pointer_packet=haze_input_take_pointer();
    assert(pointer_packet.released && !pointer_packet.held);
    assert(!haze_input_take_pointer().released);
    cursor_position={320,180};
    window_proc(window,WM_LBUTTONDOWN,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    cursor_position={640,360};
    window_proc(window,WM_MOUSEMOVE,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    window_proc(window,WM_LBUTTONUP,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    pointer_packet=haze_input_take_pointer();
    assert(pointer_packet.pressed && pointer_packet.held && !pointer_packet.released);
    assert(pointer_packet.x==.25f && pointer_packet.y==.25f);
    pointer_packet=haze_input_take_pointer();
    assert(pointer_packet.held && !pointer_packet.released && pointer_packet.x==.5f);
    pointer_packet=haze_input_take_pointer();
    assert(pointer_packet.released && !pointer_packet.held && pointer_packet.x==.5f);
    assert(!haze_input_take_pointer().released);
    haze_input_menu_state(false); sample();
    captured=1; // The window update recaptures when gameplay resumes.
    assert(haze_input_take_mouse(dx,dy,speed,inverse) && dx==0 && dy==0);
    // Keyboard navigation cancels pointer ownership without stale activation.
    haze_input_menu_state(true);
    // The desktop cursor has already reached a different row while older
    // movement messages are still queued. Hover must publish the current XY
    // immediately, including when no more physical movement follows.
    cursor_position={960,540};
    window_proc(window,WM_MOUSEMOVE,0,MAKELPARAM(320,180));
    pointer_packet=haze_input_take_pointer();
    assert(pointer_packet.active && pointer_packet.x==.75f && pointer_packet.y==.75f);
    window_proc(window,WM_MOUSEMOVE,0,MAKELPARAM(640,360));
    window_proc(window,WM_MOUSEMOVE,0,MAKELPARAM(960,540));
    pointer_packet=haze_input_take_pointer();
    assert(pointer_packet.active && pointer_packet.x==.75f && pointer_packet.y==.75f);
    window_proc(window,WM_LBUTTONDOWN,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    window_proc(window,WM_KEYDOWN,VK_DOWN,0);
    assert(!haze_input_take_pointer().active);
    window_proc(window,WM_MOUSEMOVE,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    assert(!haze_input_take_pointer().active); // Stationary synthetic move cannot steal focus.
    cursor_position={640,360};
    window_proc(window,WM_MOUSEMOVE,0,MAKELPARAM(900,500));
    pointer_packet=haze_input_take_pointer();
    assert(pointer_packet.active && pointer_packet.x==.5f && pointer_packet.y==.5f);
    window_proc(window,WM_KEYUP,VK_DOWN,0);
    haze_input_menu_state(false); sample();
    // Every button can press and release, including all four face buttons.
    captured=1;
    clear_keys(); ticks+=121;
    for (const auto& b : bindings) {
        keys[b.vk]=true;
        assert(sample().buttons & b.button);
        keys[b.vk]=false;
        assert(!(sample().buttons & b.button));
    }
    window_proc(window,WM_RBUTTONDOWN,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    window_proc(window,WM_RBUTTONUP,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    assert(sample().buttons&CELL_PAD_CTRL_R3);
    ticks+=121;
    assert(!(sample().buttons&CELL_PAD_CTRL_R3));
    captured=0; invert_y=false;
    // Lifecycle-bounded skip survives unfocused boot, then cannot confirm a menu.
    startup_skip.configure(true,true);
    const char* uri="x-cell-fs:///dev_bdvd/PS3_GAME/USRDIR/assets/g5/pamf/frd.pam";
    assert(startup_skip.playback(123,uri,true,ticks));
    focused=false; ticks+=400;
    assert(sample().buttons&CELL_PAD_CTRL_CROSS);
    startup_skip.playback(123,uri,false,ticks);
    assert(!sample().buttons);
    enabled=false;
    CellPadHostInput controller{0,128,128,128,128,0,0};
    assert(poll(0,&controller,nullptr)==1 && !controller.buttons);
    startup_skip.playback(123,uri,true,ticks); ticks+=400;
    assert(poll(0,&controller,nullptr)==1 && (controller.buttons&CELL_PAD_CTRL_CROSS));
    startup_skip.playback(123,uri,false,ticks);
    controller.buttons=0;
    assert(poll(0,&controller,nullptr)==1 && !controller.buttons);
    // Controller input and keyboard input coexist; triggers retain pressure.
    focused=true; sample(); // Observe neutral before accepting the next menu.
    enabled=true; gamepad_initialized=true; controller_enabled=true;
    fake_pad.connected=true; fake_pad.activity=true;
    fake_pad.family=HazeInputFamily::PlayStation;
    fake_pad.input={CELL_PAD_CTRL_L2,200,128,128,40,90,0};
    focused=true; next_input_update=next_config_check=0;
    haze_input_update();
    p=sample();
    assert(p.lx==200 && p.ry==40 && p.l2==90);
    assert(haze_input_family()==HazeInputFamily::PlayStation);
    fake_pad.activity=false;
    window_proc(window,WM_KEYDOWN,VK_SPACE,0);
    haze_input_update();
    assert(haze_input_family()==HazeInputFamily::Keyboard);
    p=sample(); assert((p.buttons&CELL_PAD_CTRL_CROSS) && p.lx==200 && p.l2==90);
    window_proc(window,WM_KEYUP,VK_SPACE,0);
    focused=false; p=sample();
    assert(!p.buttons && p.lx==128 && p.ry==128 && !p.l2);
    focused=true; fake_pad={}; ticks+=10; haze_input_update();
    assert(!sample().buttons && sample().lx==128);
    // Saving a real INI changes the running mapper without restarting it.
    GetFullPathNameA("local/test-input-reload.ini",MAX_PATH,config_path,nullptr);
    {
        std::ofstream file(config_path);
        file<<"[Bindings]\nCross=J\nStart=K\nForward=I\n[Mouse]\nSensitivity=0.25\nInvertY=1\n";
    }
    config_time={}; next_config_check=0; ticks+=10;
    haze_input_update();
    keys['J']=keys['I']=true;
    p=sample(); assert((p.buttons&CELL_PAD_CTRL_CROSS) && p.ly==1);
    keys['J']=keys['I']=false;
    assert(sensitivity==.25 && invert_y);
    keys[VK_RETURN]=true; assert(!(sample().buttons&CELL_PAD_CTRL_START));
    keys[VK_RETURN]=false; keys['K']=true; assert(sample().buttons&CELL_PAD_CTRL_START);
    keys['K']=false; assert(!(sample().buttons&CELL_PAD_CTRL_START));
    // A rebound manual skip cannot activate Start, including across focus loss.
    startup_skip.configure(false,true);
    assert(startup_skip.playback(123,uri,true,ticks));
    ticks+=500;
    assert(!sample().buttons);
    const auto old_start=bindings[10].vk;
    bindings[10].vk='J'; keys['J']=true;
    assert(sample().buttons==CELL_PAD_CTRL_CROSS);
    startup_skip.playback(123,uri,false,ticks);
    assert(!sample().buttons);
    focused=false; assert(!sample().buttons);
    focused=true; assert(!sample().buttons);
    ticks+=10; haze_input_update(); // A fresh focused controller sample is required.
    keys['J']=false; assert(!sample().buttons);
    keys['J']=true; assert(sample().buttons&CELL_PAD_CTRL_START);
    keys['J']=false; bindings[10].vk=old_start;
    // Controller-only mode follows the same manual path and release gate.
    enabled=false; controller_state.connected=true;
    startup_skip.playback(123,uri,true,ticks); ticks+=300;
    controller_state.input.buttons=CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_START;
    assert(sample().buttons==CELL_PAD_CTRL_CROSS);
    startup_skip.playback(123,uri,false,ticks);
    assert(!sample().buttons);
    controller_state.input.buttons=0; assert(!sample().buttons);
    controller_state.input.buttons=CELL_PAD_CTRL_START;
    assert(sample().buttons==CELL_PAD_CTRL_START);
    controller_state={}; enabled=true;
    // A mouse confirm can skip without first capturing the camera.
    const auto old_cross=bindings[0].vk;
    bindings[0].vk=VK_LBUTTON;
    startup_skip.playback(123,uri,true,ticks); ticks+=300;
    window_proc(window,WM_LBUTTONDOWN,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    assert(!captured && sample().buttons==CELL_PAD_CTRL_CROSS);
    startup_skip.playback(123,uri,false,ticks);
    assert(!sample().buttons);
    window_proc(window,WM_LBUTTONUP,0,MAKELPARAM(cursor_position.x,cursor_position.y)); assert(!sample().buttons);
    bindings[0].vk=old_cross;
    // Escape skips videos without requesting pause, and remains consumed across
    // STOP, a menu transition, focus loss, repeats, and eventual key release.
    pause_requested=false; clear_keys();
    startup_skip.playback(123,uri,true,ticks); ticks+=350;
    keys[VK_ESCAPE]=true;
    window_proc(window,WM_KEYDOWN,VK_ESCAPE,0);
    assert(!pause_requested && sample().buttons==CELL_PAD_CTRL_CROSS);
    startup_skip.playback(123,uri,false,ticks);
    haze_input_menu_state(true);
    window_proc(window,WM_KEYDOWN,VK_ESCAPE,1ll<<30);
    assert(!sample().buttons && !pause_requested);
    focused=false; assert(!sample().buttons);
    focused=true; ticks+=10; haze_input_update(); assert(!sample().buttons);
    keys[VK_ESCAPE]=false; window_proc(window,WM_KEYUP,VK_ESCAPE,0);
    assert(!sample().buttons);
    window_proc(window,WM_KEYDOWN,VK_ESCAPE,0);
    assert(sample().buttons==CELL_PAD_CTRL_START);
    window_proc(window,WM_KEYUP,VK_ESCAPE,0); sample();
    haze_input_menu_state(false);
    // Manual video skipping is not limited to the automatic-skip whitelist.
    const char* story="x-cell-fs:///dev_bdvd/PS3_GAME/USRDIR/movies/story.pam";
    startup_skip.configure(true,true);
    assert(startup_skip.playback(789,story,true,ticks)); ticks+=400;
    assert(!sample().buttons);
    window_proc(window,WM_KEYDOWN,VK_ESCAPE,0);
    assert(sample().buttons==CELL_PAD_CTRL_CROSS && !pause_requested);
    startup_skip.playback(789,story,false,ticks);
    window_proc(window,WM_KEYUP,VK_ESCAPE,0); sample();
    // Static slides consume the same manual keys without injecting any guest
    // buttons. Automatic slides also work unfocused and in controller-only mode.
    startup_skip.configure(false,false);
    assert(!haze_input_splash(456,0));
    window_proc(window,WM_KEYDOWN,VK_ESCAPE,0);
    assert(!sample().buttons && !pause_requested && haze_input_splash(456,0));
    window_proc(window,WM_KEYUP,VK_ESCAPE,0); sample();
    assert(!haze_input_splash(456,1)); // A fresh press is needed for the next slide.
    keys['J']=true; assert(!sample().buttons && haze_input_splash(456,1));
    haze_input_splash_end(456); haze_input_menu_state(true);
    assert(!sample().buttons && !haze_input_take_pointer().pressed);
    keys['J']=false; assert(!sample().buttons);
    window_proc(window,WM_KEYDOWN,'J',0); assert(sample().buttons==CELL_PAD_CTRL_CROSS);
    window_proc(window,WM_KEYUP,'J',0); sample(); haze_input_menu_state(false);
    startup_skip.configure(true,false); enabled=false; focused=false;
    assert(haze_input_splash(456,0) && !sample().buttons);
    haze_input_splash_end(456); focused=true; enabled=true;
    ticks+=10; haze_input_update(); sample();
    DeleteFileA(config_path);
    HazeStartupSkip isolated;
    const uint16_t cross=CELL_PAD_CTRL_CROSS, start=CELL_PAD_CTRL_START;
    assert(!HazeStartupSkip::known_video(nullptr));
    assert(!HazeStartupSkip::known_video("frd.pam"));
    assert(!HazeStartupSkip::known_video("x-cell-fs:///dev_bdvd/PS3_GAME/USRDIR/assets/g5/pamf/story.pam"));
    assert(isolated.playback(123,uri,true,100));
    assert(!isolated.filter(500,0,true,cross)); // Playback is the default.
    assert(isolated.filter(500,cross,true,cross)==cross);
    isolated.configure(true,true);
    assert(isolated.playback(123,uri,true,100));
    assert(!isolated.filter(399,0,true,cross));
    assert(isolated.filter(400,0,true,cross)==cross);
    assert(!isolated.filter(800,0,true,cross));
    isolated.playback(456,uri,false,900); // An unrelated player cannot cancel it.
    assert(isolated.filter(1100,0,true,cross)==cross);
    assert(isolated.filter(10900,0,false,cross)==cross); // Slow loading remains skippable.
    isolated.playback(123,uri,true,20000);
    assert(isolated.playback(123,"story.pam",true,20100));
    assert(!isolated.filter(20500,0,true,cross)); // Unknown streams never auto-skip.
    assert(isolated.filter(20500,cross|start,true,cross)==cross);
    isolated.playback(123,"story.pam",false,20500);
    assert(!isolated.filter(20500,cross|start,true,cross));
    assert(!isolated.filter(20501,0,true,cross));
    assert(isolated.filter(20502,start,true,cross)==start);
    const char* prefix="x-cell-fs:///dev_bdvd/PS3_GAME/USRDIR/assets/g5/pamf/";
    for (auto file : {"ubi.pam","ubi43.pam","frd.pam","frd43.pam"}) {
        auto path=std::string(prefix)+file;
        assert(HazeStartupSkip::opening_logo(path.c_str()));
        isolated.configure(false,true);
        assert(isolated.playback(123,path.c_str(),true,100));
        assert(!isolated.filter(450,0,true,cross));
        assert(isolated.filter(800,cross,true,cross)==cross); // Tap during auto release phase.
        assert(!isolated.filter(800,cross,false,cross));
        isolated.playback(123,path.c_str(),false,900);
        assert(!isolated.filter(1000,0,true,cross));
    }
    auto demo=std::string(prefix)+"attract.pam";
    assert(!HazeStartupSkip::opening_logo(demo.c_str()));
    isolated.playback(123,demo.c_str(),true,100);
    assert(isolated.filter(450,0,true,cross)==cross); // Independent of logos=false.
    isolated.configure(true,false);
    assert(!isolated.filter(450,0,true,cross));
    assert(isolated.filter(450,cross,true,cross)==cross);
    isolated.playback(123,demo.c_str(),false,900);
    assert(isolated.playback(123,(std::string(prefix)+"story.pam").c_str(),true,1000));
    assert(!isolated.filter(1050,0,true,cross));
    assert(!isolated.filter(1400,0,true,cross));
    // Exercise production attachment/update/capture against a deterministic
    // window API, without clipping or hiding the user's real desktop cursor.
    clear_keys(); memset(keys,0,sizeof(keys)); captured=0; menu_active=false;
    capture_policy={}; fake_pad={}; enabled=true; focused=false; capture_calls=0;
    haze_input_attach((void*)1); assert(!captured && !capture_calls);
    focused=true; ticks+=10; haze_input_update();
    assert(captured && capture_calls==1 && cursor_count<0);
    window_proc(window,WM_KEYDOWN,VK_F1,0);
    assert(!captured && cursor_count>=0);
    for (int i=0;i<10;i++) { ticks+=10; haze_input_update(); }
    assert(!captured && capture_calls==1); // Startup cannot undo F1.
    window_proc(window,WM_KEYDOWN,VK_F1,0); assert(captured);
    // A nonblocking tip publishes gameplay ownership and must preserve counts.
    mouse_motion.add(12,-34);
    haze_input_menu_state(false); ticks+=10; haze_input_update(); assert(captured);
    assert(haze_input_take_mouse(dx,dy,speed,inverse) && dx==12 && dy==-34);
    mouse_motion.add(56,78);
    haze_input_menu_state(true); ticks+=10; haze_input_update();
    assert(!captured);
    assert(!haze_input_take_mouse(dx,dy,speed,inverse) && dx==0 && dy==0);
    window_proc(window,WM_KEYDOWN,VK_F1,0); assert(!captured);
    cursor_position={320,540}; window_proc(window,WM_MOUSEMOVE,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    auto mapped=haze_input_take_pointer();
    assert(mapped.active && mapped.x==.25f && mapped.y==.75f);
    client_width=960; client_height=540; cursor_position={480,135};
    window_proc(window,WM_SIZE,0,0); window_proc(window,WM_MOUSEMOVE,0,MAKELPARAM(cursor_position.x,cursor_position.y));
    mapped=haze_input_take_pointer();
    assert(mapped.x==.5f && mapped.y==.25f); // Recompute after a window/display resize.
    haze_input_menu_state(false); ticks+=10; haze_input_update(); assert(captured);
    focused=false; window_proc(window,WM_KILLFOCUS,0,0);
    ticks+=10; haze_input_update(); assert(!captured && cursor_count>=0);
    focused=true; ticks+=10; haze_input_update(); assert(captured);
    window_proc(window,WM_KEYDOWN,VK_ESCAPE,0);
    window_proc(window,WM_KEYUP,VK_ESCAPE,0);
    ticks+=10; haze_input_update(); assert(!captured);
    ticks+=1000; haze_input_update(); assert(captured); // Guest ignored pause during loading.
    haze_input_shutdown(); assert(!captured && !mouse_capture && cursor_count>=0);
    capture_policy={}; capture_policy.enabled=false; enabled=false;
    haze_input_attach((void*)1); ticks+=10; haze_input_update(); assert(!captured);
    haze_input_shutdown();
    assert(menu_exiting_process);
    puts("Haze keyboard taps, focus release, mouse capture gating and movie skip isolation passed.");
}
