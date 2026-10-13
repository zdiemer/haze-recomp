#pragma once
#include <cstdint>

// The guest GUI uses normalized client coordinates, independently of its
// render resolution. Events are consumed on the simulation thread.
struct HazeMenuPointer {
    float x=0, y=0;
    bool active=false, held=false, pressed=false, released=false, back=false;
    int scroll=0;
};
void haze_menu_initialize();
bool haze_menu_gameplay_allowed();
void haze_input_menu_state(bool active);
HazeMenuPointer haze_input_take_pointer();
