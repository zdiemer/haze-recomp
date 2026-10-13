#pragma once
#include "haze_gamepad.h"
void haze_input_initialize();
void haze_input_attach(void* window);
void haze_input_update();
void haze_input_shutdown();
// Suppress held inputs across closing a PC page, then send its original Back.
void haze_input_menu_return();
unsigned haze_input_menu_button(int virtual_key);
HazeInputFamily haze_input_family();
// Simulation-thread consumer: raw device counts and degrees per count.
bool haze_input_take_mouse(double& x, double& y, double& sensitivity, bool& invert_y);
