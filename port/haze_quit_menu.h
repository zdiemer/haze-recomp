#pragma once
#include "haze_gamepad.h"
void haze_quit_menu_register();
void haze_quit_menu_update(void* owner,const HazeGamepadState& pad);
bool haze_quit_menu_key(int key,bool repeated);
bool haze_quit_menu_active();
bool haze_quit_menu_has_focus();
void haze_quit_menu_shutdown();
