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
    unsigned display_preview=0,display_seconds=0;
    bool display_restore_failed=false;
};
HazeGraphicsState haze_graphics_state();
// Called after startup validation, before renderer/guest threads start.
void haze_graphics_started(const haze_video::Values& configured,
    const haze_video::Values& effective,const haze_video::Values& overrides,const char* path);
// Render thread only. The menu submits a complete staged configuration.
bool haze_graphics_apply(const haze_video::Values& configured,bool persist,std::string& error);
// Menu Apply: preview display changes for 15 seconds, then persist all staged
// values only on explicit Keep. Quality-only requests apply immediately.
bool haze_graphics_preview(void* window,const haze_video::Values& configured,bool persist,std::string& error);
// The revision prevents an old confirmation from accepting a newer preview.
bool haze_graphics_confirm(unsigned revision,bool keep,std::string& error);
void haze_graphics_update(); // Render thread, outside command-list recording.
