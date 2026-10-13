// Actual Controls page/provider, SDL virtual device and Kenney PNG rendering.
// Foreground ownership is injected; physical-device acceptance is separate.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <SDL3/SDL.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <crtdbg.h>
static HWND foreground;
static HWND test_foreground() { return foreground; }
static BOOL test_activate(HWND hwnd) { foreground=hwnd; return TRUE; }
static HWND test_focus(HWND) { return nullptr; }
static BOOL test_show(HWND,int) { return TRUE; }
static SHORT test_key_state(int) { return 0; }
#define GetForegroundWindow test_foreground
#define SetForegroundWindow test_activate
#define SetFocus test_focus
#define ShowWindow test_show
#define GetAsyncKeyState test_key_state
#include "../port/haze_menu.cpp"
#include "../port/haze_controls_settings.cpp"
#include "../port/haze_controls_menu.cpp"
#include "rsx_live_draw.h"
static std::string input_path;
static HazeInputFamily input_family=HazeInputFamily::Keyboard;
static unsigned reloaded,returned,retail_returns;
extern "C" void rsx_live_draw_set_texture_override(rsx_live_texture_revision_fn,rsx_live_texture_pixels_fn,void*) {}
const char* haze_input_config_path() { return input_path.c_str(); }
void haze_input_reload() { ++reloaded; haze_gamepad_configure(input_path.c_str()); haze_prompts_initialize(input_path.c_str()); }
void haze_input_menu_return() { ++returned; }
void haze_input_menu_retail() { ++retail_returns; }
HazeInputFamily haze_input_family() { return input_family; }
unsigned haze_input_menu_button(int vk) {
    input_family=HazeInputFamily::Keyboard;
    return vk==VK_RETURN ? CELL_PAD_CTRL_CROSS : vk==VK_ESCAPE ? CELL_PAD_CTRL_CIRCLE : 0;
}
static void key(int vk) { SendMessage(panel,WM_KEYDOWN,vk,0); SendMessage(panel,WM_KEYUP,vk,0); }
static void frame(HWND owner) {
    const auto pad=haze_gamepad_update(); if (pad.activity) input_family=pad.family;
    haze_prompts_update(input_family); haze_menu_update(owner,pad);
}
static void select(const char* id) {
    for (size_t i=0;i<items.size();++i) if (items[i].key==id) {
        while (selected!=int(i)) key(VK_DOWN);
        return;
    }
    assert(false);
}
static void apply() {
    const auto rect=action_rect(0);
    SendMessage(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM((rect.left+rect.right)/2,(rect.top+rect.bottom)/2));
    SendMessage(panel,WM_LBUTTONUP,0,0);
    assert(!notice_error);
}
static void screenshot(const std::filesystem::path& path) {
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* pixels=nullptr; HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0); assert(bitmap && pixels);
    auto previous=SelectObject(dc,bitmap); SendMessage(panel,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT); GdiFlush();
    BITMAPFILEHEADER header{}; header.bfType=0x4d42; header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER); header.bfSize=header.bfOffBits+width*height*4;
    std::ofstream file(path,std::ios::binary); file.write((char*)&header,sizeof(header)); file.write((char*)&info.bmiHeader,sizeof(info.bmiHeader)); file.write((char*)pixels,width*height*4);
    assert(file.good()); SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr,screen);
}
int main(int argc,char** argv) {
    assert(argc==2); const auto repo=std::filesystem::path(argv[1]);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    input_path=(repo/"local"/("controls-ui-"+std::to_string(GetCurrentProcessId())+".ini")).string();
    { std::ofstream file(input_path); file<<"[Prompts]\nDirectory="<<(repo/"assets/input-prompts").string()<<"\n[ControllerBindings]\nTriangle=raw:b3\n"; }
    assert(haze_gamepad_initialize(input_path.c_str())); haze_prompts_initialize(input_path.c_str());
    SDL_VirtualJoystickDesc desc{}; SDL_INIT_INTERFACE(&desc); desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1; desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1; desc.name="Controls UI fixture";
    const auto id=SDL_AttachVirtualJoystick(&desc); assert(id); auto* joystick=SDL_OpenJoystick(id); assert(joystick);
    SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768); SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);
    SDL_Delay(510);
    HWND owner=CreateWindowA("STATIC","Controls test",WS_POPUP,0,0,1920,1080,nullptr,nullptr,GetModuleHandle(nullptr),nullptr); assert(owner);
    foreground=owner; haze_controls_menu_register(); assert(haze_menu_request(HazeMenuPage::Controls)); frame(owner);
    assert(items.size()==haze_control_specs().size()+3); screenshot(repo/"local/controls-page.bmp");
    select("Bindings.Forward"); key(VK_RETURN); frame(owner); key('P'); assert(stage==Stage::Preview);
    screenshot(repo/"local/controls-capture.bmp"); key(VK_RETURN); assert(edited.at("Bindings.Forward")=="P"); apply();
    assert(reloaded==1);
    select("ControllerBindings.Cross"); key(VK_RETURN); frame(owner);
    SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_NORTH,true); frame(owner); assert(stage==Stage::Preview);
    SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_NORTH,false); frame(owner);
    key(VK_RETURN); assert(stage==Stage::Conflict && edited.at("ControllerBindings.Triangle")=="raw:b3"); screenshot(repo/"local/controls-conflict.bmp");
    key(VK_RIGHT); key(VK_RETURN); assert(stage==Stage::None && edited.at("ControllerBindings.Triangle")=="None"); apply();
    SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_NORTH,true);
    const auto mapped=haze_gamepad_update(); assert((mapped.input.buttons&CELL_PAD_CTRL_CROSS) && !(mapped.input.buttons&CELL_PAD_CTRL_TRIANGLE));
    SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_NORTH,false); frame(owner);
    select("Bindings.Cross"); key(VK_RETURN); frame(owner); foreground=nullptr; frame(owner); assert(stage==Stage::None && haze_menu_active());
    foreground=owner; select("ControllerBindings.Cross"); key(VK_RETURN); frame(owner);
    SDL_CloseJoystick(joystick); SDL_DetachVirtualJoystick(id); frame(owner); assert(stage==Stage::None && haze_menu_active());
    key(VK_ESCAPE); assert(returned==1);
    assert(haze_menu_request(HazeMenuPage::Controls)); frame(owner);
    assert(edited.at("Bindings.Forward")=="P" && edited.at("ControllerBindings.Cross")=="north");
    SetWindowPos(owner,nullptr,0,0,3840,2160,SWP_NOACTIVATE|SWP_NOZORDER); frame(owner); screenshot(repo/"local/controls-4k.bmp");
    select("game-options"); key(VK_RETURN); assert(!haze_menu_active() && retail_returns==1 && returned==1);
    key(VK_DOWN); key(VK_RETURN); assert(!haze_menu_active() && retail_returns==1); // Queued messages to a hidden page.
    haze_menu_shutdown(); DestroyWindow(owner); haze_gamepad_shutdown(); std::filesystem::remove(input_path);
    std::puts("Controls UI: real keyboard capture, SDL controller rebind/apply, conflict choices, focus/disconnect cancel, reopen and PNG rendering passed.");
}
