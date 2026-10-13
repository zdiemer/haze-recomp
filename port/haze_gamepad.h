#pragma once
#include "cellPad.h"
#include <string>

enum class HazeInputFamily { Keyboard, Xbox, PlayStation, Switch };
struct HazeGamepadState {
    CellPadHostInput input{0,128,128,128,128,0,0};
    HazeInputFamily family=HazeInputFamily::Xbox;
    bool connected=false;
    bool activity=false;
    CellPadHostInput menu_input{0,128,128,128,128,0,0};
    unsigned menu_recovery=0;
    bool menu_input_valid=false;
    bool menu_recovery_raw=false;
};

// These functions belong to the window thread. Guest threads consume a copy.
bool haze_gamepad_initialize(const char* config);
void haze_gamepad_configure(const char* config);
HazeGamepadState haze_gamepad_update();
void haze_gamepad_shutdown();

struct HazeControllerInfo { std::string name; bool connected=false,mapped=false; HazeInputFamily family=HazeInputFamily::Xbox; };
HazeControllerInfo haze_gamepad_info();
enum class HazeBindingCapturePhase { Idle, Release, Listening, Captured, Cancelled };
struct HazeBindingCapture {
    HazeBindingCapturePhase phase=HazeBindingCapturePhase::Idle;
    std::string source,message;
};
// Window-thread API. axis_only captures a full axis for movement/camera.
void haze_gamepad_capture_begin(bool axis_only);
void haze_gamepad_capture_cancel();
HazeBindingCapture haze_gamepad_capture();
