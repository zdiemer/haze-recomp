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
static unsigned graphics_reloads;
void haze_graphics_request_reload() { ++graphics_reloads; }
std::string haze_graphics_notice() { return {}; }
static SHORT test_key(int vk) { return keys[vk] ? (SHORT)0x8000 : 0; }
static HWND test_foreground() { return focused ? (HWND)1 : nullptr; }
static DWORD test_owner(HWND hwnd,DWORD* pid) { *pid=hwnd ? GetCurrentProcessId() : 0; return 1; }
static ULONGLONG test_time() { ticks+=8; return ticks; }
static UINT forwarded_message;
static std::string forwarded_caption;
static RAWINPUT raw_packet;
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
static bool menu_active;
bool haze_menu_active() { return menu_active; }
bool haze_menu_has_focus() { return false; }
void haze_menu_update(void*,const HazeGamepadState&) {}
void haze_menu_shutdown() {}
void haze_video_menu_register() {}
void haze_audio_menu_register() {}
void haze_controls_menu_register() {}
void haze_steam_menu_register() {}
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
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_CALL_REPORTFAULT);
    window=(HWND)1;
    for (auto& b : bindings) b.vk=key_code(b.fallback);
    assert(key_code("w")=='W'); assert(key_code("Mouse2")==VK_RBUTTON);
    assert(sample().lx==128);
    menu_active=true; keys['W']=keys[VK_SPACE]=keys[VK_LBUTTON]=true;
    auto blocked=sample(); assert(!blocked.buttons && blocked.lx==128 && blocked.ly==128);
    menu_active=false; haze_input_menu_return();
    assert(!sample().buttons && menu_back_reports==3);
    keys['W']=keys[VK_SPACE]=keys[VK_LBUTTON]=false;
    assert(!sample().buttons && menu_back_reports==2);
    assert(sample().buttons==CELL_PAD_CTRL_CIRCLE);
    assert(sample().buttons==CELL_PAD_CTRL_CIRCLE);
    assert(!sample().buttons && !menu_back_reports);
    keys[VK_SPACE]=true; haze_input_menu_retail();
    assert(!sample().buttons && menu_back_reports==3);
    keys[VK_SPACE]=false;
    assert(!sample().buttons && !menu_back_reports);
    assert(!sample().buttons); // Retail handoff never injects Back.
    quit_active=true; keys['W']=keys[VK_SPACE]=true;
    blocked=sample(); assert(!blocked.buttons && blocked.ly==128);
    double quit_x,quit_y,quit_speed; bool quit_invert;
    assert(!haze_input_take_mouse(quit_x,quit_y,quit_speed,quit_invert));
    quit_active=false; keys['W']=keys[VK_SPACE]=false;
    haze_input_menu_retail(); assert(!sample().buttons && !menu_back_reports);
    close_accepted=true; forwarded_message=0; window_proc(window,WM_CLOSE,0,0);
    assert(forwarded_message!=WM_CLOSE);
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
    // Every button can press and release, including all four face buttons.
    clear_keys(); ticks+=121;
    for (const auto& b : bindings) {
        keys[b.vk]=true;
        assert(sample().buttons & b.button);
        keys[b.vk]=false;
        assert(!(sample().buttons & b.button));
    }
    window_proc(window,WM_RBUTTONDOWN,0,0);
    window_proc(window,WM_RBUTTONUP,0,0);
    assert(sample().buttons&CELL_PAD_CTRL_R3);
    ticks+=121;
    assert(!(sample().buttons&CELL_PAD_CTRL_R3));
    captured=0; invert_y=false;
    // Lifecycle-bounded skip survives unfocused boot, then cannot confirm a menu.
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
        file<<"[Bindings]\nCross=J\nForward=I\n[Mouse]\nSensitivity=0.25\nInvertY=1\n";
    }
    config_time={}; next_config_check=0; ticks+=10;
    haze_input_update();
    keys['J']=keys['I']=true;
    p=sample(); assert((p.buttons&CELL_PAD_CTRL_CROSS) && p.ly==1);
    keys['J']=keys['I']=false;
    assert(sensitivity==.25 && invert_y);
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
    window_proc(window,WM_LBUTTONDOWN,0,0);
    assert(!captured && sample().buttons==CELL_PAD_CTRL_CROSS);
    startup_skip.playback(123,uri,false,ticks);
    assert(!sample().buttons);
    window_proc(window,WM_LBUTTONUP,0,0); assert(!sample().buttons);
    bindings[0].vk=old_cross;
    DeleteFileA(config_path);
    HazeStartupSkip isolated;
    const uint16_t cross=CELL_PAD_CTRL_CROSS, start=CELL_PAD_CTRL_START;
    assert(!HazeStartupSkip::known_video(nullptr));
    assert(!HazeStartupSkip::known_video("frd.pam"));
    assert(!HazeStartupSkip::known_video("x-cell-fs:///dev_bdvd/PS3_GAME/USRDIR/assets/g5/pamf/story.pam"));
    assert(isolated.playback(123,uri,true,100));
    assert(!isolated.filter(399,0,true,cross));
    assert(isolated.filter(400,0,true,cross)==cross);
    assert(!isolated.filter(800,0,true,cross));
    isolated.playback(456,uri,false,900); // An unrelated player cannot cancel it.
    assert(isolated.filter(1100,0,true,cross)==cross);
    assert(isolated.filter(10900,0,false,cross)==cross); // Slow loading remains skippable.
    isolated.playback(123,uri,true,20000);
    assert(!isolated.playback(123,"story.pam",true,20100));
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
    assert(!isolated.playback(123,(std::string(prefix)+"story.pam").c_str(),true,1000));
    assert(!isolated.filter(1050,0,true,cross));
    assert(!isolated.filter(1400,0,true,cross));
    puts("Haze keyboard taps, focus release, mouse capture gating and movie skip isolation passed.");
}
