#pragma once
#include <string>
// Thread-safe explicit request. Processing runs on the native render thread.
void haze_graphics_request_reload();
void haze_graphics_request_overlay_cycle();
std::string haze_graphics_notice();
