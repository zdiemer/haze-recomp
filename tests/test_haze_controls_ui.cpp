// Actual native menu command bridge, Controls provider and SDL virtual device.
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
#include "../port/haze_prompts.h"
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
static HWND owner;
static void frame() {
    const auto pad=haze_gamepad_update(); if (pad.activity) input_family=pad.family;
    haze_prompts_update(input_family); haze_menu_update(owner,pad);
}
static void send(HazeMenuCommandKind kind,const std::string& key={},int choice=0) {
    const auto view=haze_menu_view();
    haze_menu_command({view.generation,kind,key,{},choice,view.modal.revision}); frame();
}
static void activate(const char* key) { send(HazeMenuCommandKind::Activate,key); }
static void choose_modal(int choice) { send(HazeMenuCommandKind::Modal,{},choice); }
static void key(int vk) { assert(haze_menu_capture_key(vk)); frame(); }
static void apply() { send(HazeMenuCommandKind::Apply); assert(!haze_menu_view().error); }
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
    owner=CreateWindowA("STATIC","Controls test",WS_POPUP,0,0,1920,1080,nullptr,nullptr,GetModuleHandle(nullptr),nullptr); assert(owner);
    foreground=owner; haze_controls_menu_register(); assert(haze_menu_request(HazeMenuPage::Controls)); frame();
    assert(haze_menu_view().rows.size()==haze_control_specs().size()+3);
    activate("Bindings.Forward"); frame(); key('P'); assert(stage==Stage::Preview);
    choose_modal(0); assert(edited.at("Bindings.Forward")=="P"); apply();
    assert(reloaded==1);
    activate("ControllerBindings.Cross"); frame();
    SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_NORTH,true); frame(); assert(stage==Stage::Preview);
    SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_NORTH,false); frame();
    choose_modal(0); assert(stage==Stage::Conflict && edited.at("ControllerBindings.Triangle")=="raw:b3");
    choose_modal(1); assert(stage==Stage::None && edited.at("ControllerBindings.Triangle")=="None"); apply();
    SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_NORTH,true);
    const auto mapped=haze_gamepad_update(); assert((mapped.input.buttons&CELL_PAD_CTRL_CROSS) && !(mapped.input.buttons&CELL_PAD_CTRL_TRIANGLE));
    SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_NORTH,false); frame();
    activate("Bindings.Cross"); frame(); foreground=nullptr; frame(); assert(stage==Stage::None && haze_menu_active());
    foreground=owner; activate("ControllerBindings.Cross"); frame();
    SDL_CloseJoystick(joystick); SDL_DetachVirtualJoystick(id); frame(); assert(stage==Stage::None && haze_menu_active());
    send(HazeMenuCommandKind::Back); assert(!haze_menu_active() && haze_menu_view().pop);
    assert(haze_menu_request(HazeMenuPage::Controls)); frame();
    assert(edited.at("Bindings.Forward")=="P" && edited.at("ControllerBindings.Cross")=="north");
    activate("game-options"); assert(!haze_menu_active() && !haze_menu_view().pop);
    activate("Bindings.Cross"); assert(!haze_menu_active()); // Queued commands to a departed page.
    haze_menu_shutdown(); DestroyWindow(owner); haze_gamepad_shutdown(); std::filesystem::remove(input_path);
    std::puts("Native Controls commands: keyboard capture, SDL rebind/apply, conflicts, focus/disconnect cancellation and saved reopen passed.");
}
