// Exercise the real SDL driver and port mapper using virtual hardware.
#include <SDL3/SDL.h>
#include "../port/haze_gamepad.h"
#include "../port/haze_input_source.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <crtdbg.h>
#include <chrono>
#include <process.h>

static SDL_JoystickID attach(bool custom=false,bool unmapped=false) {
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type=custom ? SDL_JOYSTICK_TYPE_FLIGHT_STICK : SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.nhats=1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.name=custom ? "Haze custom joystick" : "Haze virtual controller";
    if (unmapped) { desc.vendor_id=0x1234; desc.product_id=0x5678; }
    auto id=SDL_AttachVirtualJoystick(&desc);
    assert(id);
    return id;
}
int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    const auto directory=std::filesystem::temp_directory_path()/("haze-controller-test-"+
        std::to_string(_getpid())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(std::filesystem::create_directory(directory));
    const auto config=(directory/"input.ini").string();
    assert(haze_gamepad_initialize(config.c_str()));
    assert(!haze_gamepad_update().connected);
    auto id=attach();
    auto* joy=SDL_OpenJoystick(id); assert(joy);
    SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768);
    SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768);
    SDL_Delay(510);
    auto state=haze_gamepad_update();
    assert(state.connected && !state.input.buttons && state.input.lx==128);
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,true));
    state=haze_gamepad_update();
    assert(state.activity && (state.input.buttons&CELL_PAD_CTRL_CROSS));
    assert(!haze_gamepad_update().activity); // Holding does not steal icons.
    SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,false);
    state=haze_gamepad_update(); assert(!state.activity && !state.input.buttons);
    SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTX,1000);
    state=haze_gamepad_update(); assert(!state.activity && state.input.lx==128);
    SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTX,32767);
    SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_RIGHTY,-32768);
    state=haze_gamepad_update(); assert(state.activity && state.input.lx==255 && state.input.ry==0);
    SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,0);
    state=haze_gamepad_update();
    assert((state.input.buttons&CELL_PAD_CTRL_L2) && state.input.l2>100 && state.input.l2<150);
    SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,32767);
    assert(haze_gamepad_update().input.l2==255);
    {
        std::ofstream file(config);
        file<<"[ControllerBindings]\nCross=east\nCircle=none\nR2=lefttrigger\n"
              "[Controller]\nInvertLookY=1\nIconFamily=playstation\n";
    }
    haze_gamepad_configure(config.c_str());
    SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_EAST,true);
    state=haze_gamepad_update();
    assert((state.input.buttons&CELL_PAD_CTRL_CROSS) && !(state.input.buttons&CELL_PAD_CTRL_CIRCLE));
    assert(state.input.r2==255 && state.input.ry==255 && state.family==HazeInputFamily::PlayStation);
    assert(state.menu_input_valid && state.menu_recovery && (state.menu_input.buttons&CELL_PAD_CTRL_CIRCLE) && !(state.menu_input.buttons&CELL_PAD_CTRL_CROSS));
    SDL_CloseJoystick(joy);
    assert(SDL_DetachVirtualJoystick(id));
    state=haze_gamepad_update();
    assert(!state.connected && !state.input.buttons && state.input.lx==128 && state.input.ry==128);
    // Reconnect after the disconnected state has already been observed.
    id=attach(); SDL_Delay(510);
    assert(haze_gamepad_update().connected);
    assert(SDL_DetachVirtualJoystick(id));
    haze_gamepad_update();
    // An arbitrary joystick becomes a gamepad through a user-supplied mapping.
    id=attach(true);
    assert(!SDL_IsGamepad(id));
    char guid[33]; SDL_GUIDToString(SDL_GetJoystickGUIDForID(id),guid,sizeof(guid));
    const auto mapping=directory/"mappings.txt";
    {
        std::ofstream file(mapping);
        file<<guid<<",Haze custom mapping,a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1,"
                    "rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:Windows,\n";
        std::ofstream ini(config);
        ini<<"[Controller]\nMappingFile="<<mapping.filename().string()<<"\n";
    }
    haze_gamepad_configure(config.c_str());
    assert(SDL_IsGamepad(id));
    joy=SDL_OpenJoystick(id); assert(joy);
    SDL_SetJoystickVirtualAxis(joy,4,-32768);
    SDL_SetJoystickVirtualAxis(joy,5,-32768);
    SDL_SetJoystickVirtualButton(joy,0,true);
    SDL_Delay(510);
    state=haze_gamepad_update();
    assert(state.connected && (state.input.buttons&CELL_PAD_CTRL_CROSS));
    SDL_CloseJoystick(joy); SDL_DetachVirtualJoystick(id);
    haze_gamepad_update();
    // Unmapped hardware remains available for raw capture and binding.
    id=attach(true,true); joy=SDL_OpenJoystick(id); assert(joy);
    assert(!SDL_IsGamepad(id));
    SDL_Delay(510); haze_gamepad_update();
    assert(haze_gamepad_info().connected && !haze_gamepad_info().mapped);
    SDL_SetJoystickVirtualButton(joy,2,true); haze_gamepad_update();
    haze_gamepad_capture_begin(false); haze_gamepad_update();
    assert(haze_gamepad_capture().phase==HazeBindingCapturePhase::Release);
    SDL_SetJoystickVirtualButton(joy,2,false); haze_gamepad_update();
    assert(haze_gamepad_capture().phase==HazeBindingCapturePhase::Listening);
    SDL_SetJoystickVirtualHat(joy,0,SDL_HAT_DOWN); haze_gamepad_update();
    assert(haze_gamepad_capture().source=="raw:h0.4");
    SDL_SetJoystickVirtualHat(joy,0,SDL_HAT_CENTERED); haze_gamepad_update();
    haze_gamepad_capture_begin(false); haze_gamepad_update();
    SDL_SetJoystickVirtualAxis(joy,0,-32768); haze_gamepad_update();
    assert(haze_gamepad_capture().source=="raw:-a0");
    {
        std::ofstream ini(config);
        ini<<"[ControllerBindings]\nCross=raw:b2\nCircle=raw:h0.4\nSquare=raw:-a0\n"
              "[Controller]\nMoveX=raw:a1\nLookY=raw:a2\n";
    }
    haze_gamepad_configure(config.c_str());
    SDL_SetJoystickVirtualButton(joy,2,true);
    SDL_SetJoystickVirtualHat(joy,0,SDL_HAT_DOWN);
    SDL_SetJoystickVirtualAxis(joy,1,32767);
    state=haze_gamepad_update();
    assert((state.input.buttons&(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE|CELL_PAD_CTRL_SQUARE))==
        (CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE|CELL_PAD_CTRL_SQUARE));
    assert(state.input.lx==255);
    SDL_SetJoystickVirtualButton(joy,2,false); SDL_SetJoystickVirtualHat(joy,0,0);
    SDL_SetJoystickVirtualAxis(joy,0,0); SDL_SetJoystickVirtualAxis(joy,1,0); haze_gamepad_update();
    haze_gamepad_capture_begin(false); haze_gamepad_update();
    SDL_SetJoystickVirtualButton(joy,2,true); haze_gamepad_update();
    assert(haze_gamepad_capture().source=="raw:b2");
    SDL_SetJoystickVirtualButton(joy,2,false); haze_gamepad_update();
    haze_gamepad_capture_begin(true); haze_gamepad_update();
    SDL_SetJoystickVirtualAxis(joy,2,32767); haze_gamepad_update();
    assert(haze_gamepad_capture().source=="raw:a2");
    haze_gamepad_capture_begin(true); haze_gamepad_capture_cancel();
    assert(haze_gamepad_capture().phase==HazeBindingCapturePhase::Cancelled);
    haze_gamepad_capture_begin(true);
    SDL_CloseJoystick(joy); SDL_DetachVirtualJoystick(id); haze_gamepad_update();
    assert(haze_gamepad_capture().phase==HazeBindingCapturePhase::Cancelled && !haze_gamepad_info().connected);
    for (const char* invalid:{"raw:b-1","raw:a256","raw:h0.0","raw:h0.3","raw:+b1","raw:a0junk","raw:"})
        assert(haze_raw_source(invalid).kind==HazeRawSource::None);
    haze_gamepad_shutdown();
    std::filesystem::remove(config);
    std::filesystem::remove(mapping);
    std::filesystem::remove(directory);
    puts("SDL controller hotplug, position mapping, deadzones, analog triggers, activity and rebinds passed.");
}
