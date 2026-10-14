#pragma once
#include "haze_gamepad.h"
void haze_input_initialize();
void haze_input_attach(void* window);
void haze_input_update();
void haze_input_shutdown();
// Audited static startup-slide update; all loading stays in the guest.
bool haze_input_splash(uint32_t scene, int screen);
void haze_input_splash_end(uint32_t scene);
// Suppress held inputs across closing a PC page, then send its original Back.
void haze_input_menu_return();
// Release the PC overlay after neutral input without popping its retail page.
void haze_input_menu_retail();
unsigned haze_input_menu_button(int virtual_key);
// Window-thread settings transactions use the same startup path and reload.
const char* haze_input_config_path();
void haze_input_reload();
HazeInputFamily haze_input_family();
// Simulation-thread consumer: raw device counts and degrees per count.
bool haze_input_take_mouse(double& x, double& y, double& sensitivity, bool& invert_y);
