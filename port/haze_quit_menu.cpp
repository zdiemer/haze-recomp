#include "haze_quit_menu.h"
#include "haze_quit.h"
#include "haze_menu.h"

namespace quit_ui {
HazeMenuModal dialog;
HazeQuitPhase shown=HazeQuitPhase::Idle;
unsigned revision=0;
uint64_t generation=0;
void status() {
    const auto state=haze_quit_state();
    if (state.phase==HazeQuitPhase::Idle) { dialog={}; shown=state.phase; return; }
    if (shown!=state.phase || generation!=state.generation || !dialog.revision) {
        shown=state.phase; generation=state.generation;
        dialog={++revision,"Quit to Desktop",state.message,
            state.phase==HazeQuitPhase::Confirm ? std::vector<std::string>{"Cancel","Quit to Desktop"} : std::vector<std::string>{"Cancel"},false,0};
    }
    dialog.message=state.message;
}
void choose(int choice) {
    const auto state=haze_quit_state();
    if (state.phase==HazeQuitPhase::Confirm && choice==1) haze_quit_confirm(state.generation);
    else if (haze_quit_cancel(state.generation)) {
        haze_menu_interrupt({});
        if (haze_menu_view().page==HazeMenuPage::Quit) haze_menu_retail();
    }
    status();
}
}
void haze_quit_menu_register() {
    HazeMenuProvider provider;
    provider.title="QUIT TO DESKTOP";
    provider.guest_back=false;
    provider.begin=[] {
        const auto offer=haze_quit_offer();
        if (haze_quit_state().phase==HazeQuitPhase::Idle && offer.available)
            haze_quit_request(offer.gameplay,offer.generation);
        quit_ui::status();
    };
    provider.modal=[] { return quit_ui::dialog; };
    provider.modal_choose=quit_ui::choose;
    provider.update=[](bool focused) {
        const auto state=haze_quit_state();
        if (!focused && state.phase==HazeQuitPhase::Confirm) haze_quit_cancel(state.generation);
        quit_ui::status();
        if (haze_quit_state().phase==HazeQuitPhase::Idle) {
            const auto view=haze_menu_view();
            haze_menu_command({view.generation,HazeMenuCommandKind::Back});
        }
    };
    haze_menu_register(HazeMenuPage::Quit,std::move(provider));
}
bool haze_quit_menu_active() { return haze_quit_state().phase!=HazeQuitPhase::Idle; }
bool haze_quit_menu_has_focus() { return haze_menu_has_focus(); }
bool haze_quit_menu_key(int key,bool repeat) {
    // A close request may arrive between native contexts (for example loading).
    // Escape must still release a failed/waiting shutdown before a page exists.
    if (key!=27 || repeat || !haze_quit_menu_active() || (haze_menu_active() && !haze_menu_blocks_input())) return false;
    return haze_quit_cancel(haze_quit_state().generation);
}
void haze_quit_menu_update(void*,const HazeGamepadState&) {
    // Window close can arrive on any native page, including during Apply or a
    // display preview. Keep that provider polling underneath the status dialog.
    quit_ui::status();
    const auto view=haze_menu_view();
    if (!haze_quit_menu_active() || view.page==HazeMenuPage::Quit) haze_menu_interrupt({});
    else if (haze_menu_active()) haze_menu_interrupt(quit_ui::dialog,quit_ui::choose);
    else haze_menu_request(HazeMenuPage::Quit);
}
void haze_quit_menu_shutdown() { quit_ui::dialog={}; quit_ui::shown=HazeQuitPhase::Idle; }
