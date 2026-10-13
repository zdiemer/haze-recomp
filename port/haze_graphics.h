#pragma once
#include "haze_graphics_reload.h"
// Load startup graphics settings. Environment variables take precedence.
bool haze_graphics_initialize();
#include "haze_video_settings.h"
#include <vector>
struct HazeGraphicsState {
    haze_video::Values configured, effective, overrides;
    std::vector<std::string> restart;
    std::string path, notice;
};
HazeGraphicsState haze_graphics_state();
// Called after startup validation, before renderer/guest threads start.
void haze_graphics_started(const haze_video::Values& configured,
    const haze_video::Values& effective,const haze_video::Values& overrides,const char* path);
// Render thread only. The menu submits a complete staged configuration.
bool haze_graphics_apply(const haze_video::Values& configured,bool persist,std::string& error);
void haze_graphics_update(); // Render thread, outside command-list recording.
