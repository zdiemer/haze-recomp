#pragma once
#include "haze_gamepad.h"

// Initialize before renderer creation. A worker owns later filesystem checks
// and atlas rebuilding; update only posts the requested input family.
void haze_prompts_initialize(const char* config);
void haze_prompts_update(HazeInputFamily family);
// Stops admission and waits at most timeout_ms. A blocked worker retains its
// process-lifetime storage until exit; initialize refuses to reset it meanwhile.
bool haze_prompts_shutdown(unsigned timeout_ms);
// Draw the same current atlas glyph used by the game into a native PC page.
bool haze_prompts_draw_icon(void* dc,int slot,int x,int y,int size);
bool haze_prompts_draw_binding(void* dc,HazeInputFamily family,const char* binding,int x,int y,int size);
