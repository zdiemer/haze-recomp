#pragma once
struct haze_display_defaults {
    unsigned render_width, render_height;
    unsigned window_width, window_height;
};
// Detect physical desktop pixels and a 16:9 client fitting the monitor work area.
bool haze_display_detect(const char* monitor, haze_display_defaults& defaults);
// Call on the window-owning thread before creation, then before renderer init.
void haze_display_prepare();
bool haze_display_apply(void* window);
#include <string>
// Explicit window geometry, independent of startup environment. Fullscreen
// ownership is managed separately by the renderer's DXGI transaction.
bool haze_display_configure(void* window,const char* mode,const char* monitor,const char* size,bool activate);
// One reversible transition on the window/render thread. Begin does not write
// settings. Finish(false) restores window placement and fullscreen policy.
bool haze_display_begin(void* window,const char* mode,const char* monitor,const char* size,std::string& error);
bool haze_display_finish(bool keep,std::string& error);
bool haze_display_pending();
