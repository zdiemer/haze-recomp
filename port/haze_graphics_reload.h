#pragma once
#include <string>
// Thread-safe explicit request. Processing runs on the native render thread.
void haze_graphics_request_reload();
std::string haze_graphics_notice();
