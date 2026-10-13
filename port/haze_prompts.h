#pragma once
#include "haze_gamepad.h"

// Initialize before renderer creation; update and PNG decoding use the window
// thread. Immutable snapshots are shared with the render consumer.
void haze_prompts_initialize(const char* config);
void haze_prompts_update(HazeInputFamily family);
// Draw the same current atlas glyph used by the game into a native PC page.
bool haze_prompts_draw_icon(void* dc,int slot,int x,int y,int size);
