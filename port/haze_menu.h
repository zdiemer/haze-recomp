#pragma once
#include <string>
#include <vector>
#include <functional>
#include "haze_gamepad.h"

enum class HazeMenuPage { None, Video, Audio, Controls };
struct HazeMenuChoice { std::string value,label; };
struct HazeMenuRow {
    std::string key,label,value,hint;
    std::vector<HazeMenuChoice> choices;
    bool slider=false;
    double minimum=0,maximum=1,step=.05;
};
struct HazeMenuProvider {
    std::string title;
    std::function<void()> begin,defaults;
    std::function<std::vector<HazeMenuRow>()> rows;
    std::function<void(const std::string&,const std::string&)> change;
    std::function<bool(std::string&)> apply;
};
// Register on the window thread before dispatching guest menu events.
void haze_menu_register(HazeMenuPage page,HazeMenuProvider provider);
// Guest-thread notification. Returns false for pages without a PC provider.
bool haze_menu_request(HazeMenuPage page);
bool haze_menu_active();
bool haze_menu_has_focus();
void haze_menu_update(void* owner,const HazeGamepadState& pad);
void haze_menu_shutdown();
void haze_video_menu_register();
