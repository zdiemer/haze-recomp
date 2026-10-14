#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <array>
#include "haze_gamepad.h"

enum class HazeMenuPage { None, Video, Audio, Controls, Desktop, Quit };
enum class HazeMenuApply { Applied, Pending, Failed };
struct HazeMenuChoice { std::string value,label; bool operator==(const HazeMenuChoice&) const = default; };
struct HazeMenuRow {
    std::string key,label,value,hint;
    std::vector<HazeMenuChoice> choices;
    bool slider=false;
    double minimum=0,maximum=1,step=.05;
    bool action=false;
    std::string prompt_binding;
    HazeInputFamily prompt_family=HazeInputFamily::Keyboard;
    bool operator==(const HazeMenuRow&) const = default;
};
struct HazeMenuModal {
    unsigned revision=0;
    std::string title,message;
    std::vector<std::string> choices;
    bool capturing=false;
    int cancel_choice=-1; // -1 keeps the existing convention: last choice cancels
    bool operator==(const HazeMenuModal&) const = default;
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
    std::array<std::string,3> actions={"Apply","Defaults","Back"};
    std::string footer_hint="Apply saves changes. Back discards edits made since the last Apply.";
    std::string defaults_notice="Defaults selected. Apply to save.";
    bool guest_back=true;
};
// Register on the window thread before dispatching guest menu events.
void haze_menu_register(HazeMenuPage page,HazeMenuProvider provider);
// Guest-thread notification. Returns false for pages without a PC provider.
bool haze_menu_request(HazeMenuPage page);
bool haze_menu_active();
bool haze_menu_has_focus();
void* haze_menu_owner();
void haze_menu_update(void* owner,const HazeGamepadState& pad);
// Final process exit skips provider callbacks: the native shutdown path owns
// display cleanup and must not start another preview rollback/GPU wait.
void haze_menu_shutdown(bool exiting_process=false);
void haze_menu_retail();
// Window-thread status dialog that leaves the current provider/transaction alive.
// An empty dialog restores that provider's own modal and pending Apply state.
void haze_menu_interrupt(HazeMenuModal dialog,std::function<void(int)> choose={});
void haze_video_menu_register();
void haze_controls_menu_register();
void haze_steam_menu_register();

// Providers stay on the window thread. The guest GUI receives immutable
// snapshots and posts commands tagged with the page generation it displayed.
struct HazeMenuView {
    uint64_t generation=0,revision=0;
    HazeMenuPage page=HazeMenuPage::None;
    std::string title,notice;
    std::vector<HazeMenuRow> rows;
    std::array<std::string,3> actions{};
    HazeMenuModal modal;
    bool applying=false,error=false,pop=false;
};
enum class HazeMenuCommandKind { Change, Activate, Apply, Defaults, Back, Modal, Key, Departed };
struct HazeMenuCommand {
    uint64_t generation=0;
    HazeMenuCommandKind kind=HazeMenuCommandKind::Back;
    std::string key,value;
    int choice=0;
    unsigned modal_revision=0;
};
HazeMenuView haze_menu_view();
void haze_menu_command(HazeMenuCommand command);
void haze_menu_presented(uint64_t generation);
bool haze_menu_blocks_input();
bool haze_menu_capture_key(int key);

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
