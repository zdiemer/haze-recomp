// Exercise the real SDL driver and port mapper using virtual hardware.
#include <SDL3/SDL.h>
#include "../port/haze_gamepad.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <cstdlib>

static SDL_JoystickID attach(bool custom=false) {
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type=custom ? SDL_JOYSTICK_TYPE_FLIGHT_STICK : SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.name=custom ? "Haze custom joystick" : "Haze virtual controller";
    auto id=SDL_AttachVirtualJoystick(&desc);
    assert(id);
    return id;
}
int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_CALL_REPORTFAULT);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    const auto config=std::filesystem::absolute("controller-test.ini").string();
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
    const auto mapping=std::filesystem::absolute("controller-test-mappings.txt");
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
    haze_gamepad_shutdown();
    std::filesystem::remove(config);
    std::filesystem::remove(mapping);
    puts("SDL controller hotplug, position mapping, deadzones, analog triggers, activity and rebinds passed.");
}
