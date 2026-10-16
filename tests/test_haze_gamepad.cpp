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

struct Motors { Uint16 low=0,high=0; };
static bool SDLCALL rumble(void* user,Uint16 low,Uint16 high) {
    auto* motors=static_cast<Motors*>(user); motors->low=low; motors->high=high; return true;
}
static SDL_JoystickID attach(bool custom=false,bool unmapped=false,Motors* motors=nullptr) {
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    if (motors) { desc.Rumble=rumble; desc.userdata=motors; }
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
    assert(haze_gamepad_bindings_overlap("south",false,"raw:b0",false));
    assert(haze_gamepad_bindings_overlap("raw:b0",false,"a",false));
    assert(!haze_gamepad_bindings_overlap("east",false,"raw:b0",false));
    assert(!haze_gamepad_bindings_overlap("None",false,"None",false));
    assert(haze_gamepad_bindings_overlap("leftx",true,"raw:-a0",false));
    assert(haze_gamepad_bindings_overlap("-leftx",false,"raw:-a0",false));
    assert(!haze_gamepad_bindings_overlap("leftx",false,"raw:-a0",false));
    assert(!haze_gamepad_bindings_overlap("raw:+a0",false,"raw:-a0",false));
    assert(haze_gamepad_bindings_overlap("raw:+a0",true,"raw:-a0",false));
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
        file<<"[ControllerBindings]\nCross=south\nCircle=raw:b0\n";
    }
    haze_gamepad_configure(config.c_str());
    SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,true);
    state=haze_gamepad_update();
    assert((state.input.buttons&CELL_PAD_CTRL_CROSS) && (state.input.buttons&CELL_PAD_CTRL_CIRCLE));
    assert(state.menu_recovery && (state.menu_input.buttons&CELL_PAD_CTRL_CROSS) && !(state.menu_input.buttons&CELL_PAD_CTRL_CIRCLE));
    SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,false);
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
    for (const auto& family : {std::pair{"xbox",HazeInputFamily::Xbox},
                              std::pair{"playstation",HazeInputFamily::PlayStation},
                              std::pair{"switch",HazeInputFamily::Switch}}) {
        { std::ofstream file(config);
          file<<"[ControllerBindings]\nStart=north\n[Controller]\nIconFamily="<<family.first<<"\n"; }
        haze_gamepad_configure(config.c_str());
        SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_START,true);
        state=haze_gamepad_update();
        assert(state.family==family.second && !(state.input.buttons&CELL_PAD_CTRL_START));
        SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_START,false);
        SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_NORTH,true);
        assert(haze_gamepad_update().input.buttons&CELL_PAD_CTRL_START);
        SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_NORTH,false);
        assert(!(haze_gamepad_update().input.buttons&CELL_PAD_CTRL_START));
    }
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
                    "rightx:a2~,righty:a3,lefttrigger:+a4,righttrigger:a5,dpup:h0.1,dpdown:h0.4,platform:Windows,\n";
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
    assert(haze_gamepad_bindings_overlap("dpup",false,"raw:h0.1",false));
    assert(!haze_gamepad_bindings_overlap("dpdown",false,"raw:h0.1",false));
    assert(haze_gamepad_bindings_overlap("rightx",false,"raw:-a2",false));
    assert(!haze_gamepad_bindings_overlap("rightx",false,"raw:+a2",false));
    assert(haze_gamepad_bindings_overlap("-rightx",false,"raw:+a2",false));
    assert(haze_gamepad_bindings_overlap("rightx",true,"raw:+a2",false));
    assert(haze_gamepad_bindings_overlap("lefttrigger",false,"raw:+a4",false));
    assert(!haze_gamepad_bindings_overlap("lefttrigger",false,"raw:-a4",false));
    // A mapping reload must immediately change alias resolution on the same device.
    const auto remapped=std::string(guid)+",Haze remapped,a:b3,b:b1,x:b2,y:+a3,leftx:b5,lefty:a1,platform:Windows,";
    assert(SDL_AddGamepadMapping(remapped.c_str())>=0);
    assert(haze_gamepad_bindings_overlap("south",false,"raw:b3",false));
    assert(!haze_gamepad_bindings_overlap("south",false,"raw:b0",false));
    assert(haze_gamepad_bindings_overlap("north",false,"raw:+a3",false)); // Axis mapped to a button.
    assert(!haze_gamepad_bindings_overlap("north",false,"raw:-a3",false));
    assert(haze_gamepad_bindings_overlap("leftx",false,"raw:b5",false)); // Button mapped to an axis.
    assert(!haze_gamepad_bindings_overlap("-leftx",false,"raw:b5",false));
    // A custom SDL mapping can alias the standard face buttons themselves.
    // Recovery must then use distinct raw buttons, with matching raw prompts.
    {
        std::ofstream file(mapping);
        file<<guid<<",Haze duplicate faces,a:b3,b:b3,x:b2,y:b0,leftx:a0,lefty:a1,platform:Windows,\n";
    }
    haze_gamepad_configure(config.c_str());
    SDL_SetJoystickVirtualButton(joy,0,false); SDL_SetJoystickVirtualButton(joy,3,true);
    state=haze_gamepad_update();
    assert((state.input.buttons&CELL_PAD_CTRL_CROSS) && (state.input.buttons&CELL_PAD_CTRL_CIRCLE));
    assert(state.menu_recovery_raw && !(state.menu_input.buttons&(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE)));
    SDL_SetJoystickVirtualButton(joy,0,true); state=haze_gamepad_update();
    assert((state.menu_input.buttons&CELL_PAD_CTRL_CROSS) && !(state.menu_input.buttons&CELL_PAD_CTRL_CIRCLE));
    SDL_CloseJoystick(joy); SDL_DetachVirtualJoystick(id);
    haze_gamepad_update();
    // Unmapped hardware remains available for raw capture and binding.
    id=attach(true,true); joy=SDL_OpenJoystick(id); assert(joy);
    assert(!SDL_IsGamepad(id));
    SDL_Delay(510); haze_gamepad_update();
    assert(!haze_gamepad_bindings_overlap("south",false,"raw:b0",false)); // No assumed mapping for an unknown device.
    assert(haze_gamepad_bindings_overlap("raw:h0.1",false,"raw:h0.1",false));
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
    // Guest actuators follow the selected device, scaled by input.ini.
    { std::ofstream ini(config); }
    haze_gamepad_configure(config.c_str());
    Motors first,second;
    attach(false,false,&first); SDL_Delay(510); haze_gamepad_update();
    haze_gamepad_rumble(1,128);
    assert(first.low==128*257 && first.high==65535);
    haze_gamepad_rumble(0,0); assert(!first.low && !first.high);
    { std::ofstream ini(config); ini<<"[Controller]\nRumbleIntensity=0.5\n"; }
    haze_gamepad_configure(config.c_str());
    haze_gamepad_rumble(1,255); assert(first.low==32768 && first.high==32768);
    { std::ofstream ini(config); ini<<"[Controller]\nRumble=0\nRumbleIntensity=0.5\n"; }
    haze_gamepad_configure(config.c_str());
    haze_gamepad_rumble(1,255); assert(!first.low && !first.high); // Disabling stops a held effect.
    { std::ofstream ini(config); ini<<"[Controller]\nRumble=1\nRumbleIntensity=2\n"; }
    haze_gamepad_configure(config.c_str());
    haze_gamepad_rumble(0,255); assert(first.low==65535 && !first.high); // Invalid intensity uses 1.
    // A held request is refreshed while serviced and expires when it is not.
    for (int i=0;i<8;++i) { SDL_Delay(50); haze_gamepad_update(); haze_gamepad_rumble(0,255); }
    assert(first.low==65535);
    SDL_Delay(300); haze_gamepad_update(); assert(!first.low);
    haze_gamepad_rumble(0,255); assert(first.low==65535);
    // Switching controllers moves the effect and stops the previous device.
    const auto second_id=attach(false,false,&second); SDL_Delay(510); haze_gamepad_update();
    haze_gamepad_rumble(0,255); assert(first.low==65535 && !second.low);
    joy=SDL_OpenJoystick(second_id); assert(joy);
    SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,true);
    assert(haze_gamepad_update().activity);
    haze_gamepad_rumble(1,255); assert(!first.low && !first.high && second.low==65535 && second.high==65535);
    SDL_CloseJoystick(joy); assert(SDL_DetachVirtualJoystick(second_id));
    haze_gamepad_update(); haze_gamepad_rumble(1,255);
    assert(first.low==65535 && first.high==65535); // Disconnect falls back to the remaining device.
    // A controller without motors is selected without failing the request.
    const auto silent=attach(); SDL_Delay(510); haze_gamepad_update();
    joy=SDL_OpenJoystick(silent); assert(joy);
    SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,true);
    assert(haze_gamepad_update().activity);
    haze_gamepad_rumble(1,255); haze_gamepad_rumble(1,255); assert(!first.low && !first.high);
    SDL_CloseJoystick(joy); assert(SDL_DetachVirtualJoystick(silent));
    haze_gamepad_update(); haze_gamepad_rumble(1,255); assert(first.low==65535);
    haze_gamepad_shutdown(); assert(!first.low && !first.high);
    std::filesystem::remove(config);
    std::filesystem::remove(mapping);
    std::filesystem::remove(directory);
    puts("SDL controller hotplug, position mapping, deadzones, analog triggers, activity, rebinds and rumble passed.");
}
