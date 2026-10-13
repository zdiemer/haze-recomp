#pragma once
#include <string>
#include <vector>
#include <functional>
#include "haze_gamepad.h"

enum class HazeMenuPage { None, Video, Audio, Controls };
enum class HazeMenuApply { Applied, Pending, Failed };
struct HazeMenuChoice { std::string value,label; };
struct HazeMenuRow {
    std::string key,label,value,hint;
    std::vector<HazeMenuChoice> choices;
    bool slider=false;
    double minimum=0,maximum=1,step=.05;
    bool action=false;
    std::string prompt_binding;
    HazeInputFamily prompt_family=HazeInputFamily::Keyboard;
};
struct HazeMenuModal {
    unsigned revision=0;
    std::string title,message;
    std::vector<std::string> choices;
    bool capturing=false;
};
struct HazeMenuProvider {
    std::string title;
    std::function<void()> begin,defaults;
    std::function<std::vector<HazeMenuRow>()> rows;
    std::function<void(const std::string&,const std::string&)> change;
    std::function<HazeMenuApply(std::string&)> apply;
    // Called on the window thread while Apply is pending. Providers publish
    // completion from their owning thread; polling must never block rendering.
    std::function<HazeMenuApply(std::string&)> poll;
    std::function<void(const std::string&)> activate;
    std::function<void(bool)> update;
    std::function<HazeMenuModal()> modal;
    std::function<void(int)> modal_key,modal_choose;
};
// Register on the window thread before dispatching guest menu events.
void haze_menu_register(HazeMenuPage page,HazeMenuProvider provider);
// Guest-thread notification. Returns false for pages without a PC provider.
bool haze_menu_request(HazeMenuPage page);
bool haze_menu_active();
bool haze_menu_has_focus();
void haze_menu_update(void* owner,const HazeGamepadState& pad);
void haze_menu_shutdown();
void haze_menu_retail();
void haze_video_menu_register();
void haze_controls_menu_register();
