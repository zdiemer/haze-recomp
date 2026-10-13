#include "haze_quit.h"
#include "haze_save_status.h"
#include <mutex>

namespace {
std::mutex quit_mutex;
HazeQuitState state;
uint64_t offered_menu=0,waiting_ticket=0,observed_ticket=0;
void fail(const char* message) {
    state.phase=HazeQuitPhase::Failed; state.message=message;
}
}
HazeQuitState haze_quit_state() {
    std::lock_guard lock(quit_mutex); return state;
}
bool haze_quit_request(bool gameplay,uint64_t menu_generation) {
    std::lock_guard lock(quit_mutex);
    if (!menu_generation || state.phase!=HazeQuitPhase::Idle) return false;
    ++state.generation; state.gameplay=gameplay; offered_menu=menu_generation;
    state.phase=HazeQuitPhase::Confirm;
    state.message=gameplay ? "Quit to Desktop? Progress since the last completed checkpoint may be lost." : "Quit to Desktop?";
    waiting_ticket=0; return true;
}
bool haze_quit_confirm(uint64_t generation) {
    std::lock_guard lock(quit_mutex);
    if (generation!=state.generation || state.phase!=HazeQuitPhase::Confirm) return false;
    state.phase=HazeQuitPhase::Waiting;
    state.message="Waiting for saves and settings to finish...";
    const auto save=haze_save_status();
    observed_ticket=save.ticket;
    waiting_ticket=save.busy ? save.ticket : 0;
    return true;
}
bool haze_quit_cancel(uint64_t generation) {
    std::lock_guard lock(quit_mutex);
    if (generation!=state.generation || state.phase==HazeQuitPhase::Idle || state.phase==HazeQuitPhase::Stopping) return false;
    if (state.phase==HazeQuitPhase::Ready) haze_save_unseal();
    state.phase=HazeQuitPhase::Idle; state.message.clear(); waiting_ticket=0;
    return true;
}
bool haze_quit_poll(uint64_t menu_generation,bool guest_busy,bool settings_busy) {
    std::lock_guard lock(quit_mutex);
    if (state.phase==HazeQuitPhase::Ready || state.phase==HazeQuitPhase::Stopping) return true;
    if (state.phase!=HazeQuitPhase::Waiting) return false;
    if (menu_generation!=offered_menu) {
        fail("The menu changed. Cancel and choose Quit to Desktop again."); return false;
    }
    const auto save=haze_save_status();
    if (!waiting_ticket && save.ticket!=observed_ticket) {
        observed_ticket=save.ticket; waiting_ticket=save.ticket;
    }
    if (waiting_ticket) {
        const auto previous=haze_save_status(waiting_ticket);
        if (!previous.ticket) { fail("The save result is unavailable. Cancel to return to the game."); return false; }
        if (!previous.completed) return false;
        if (previous.result) { fail("The save failed. Cancel to return to the game; progress was not saved."); return false; }
        waiting_ticket=0;
    }
    if (save.ticket!=observed_ticket || save.busy) {
        observed_ticket=save.ticket; waiting_ticket=save.ticket; return false;
    }
    if (guest_busy || settings_busy) return false;
    // A new save may have arrived since the snapshot. Its launch and this
    // admission seal serialize on the save observer's mutex.
    if (!haze_save_seal(observed_ticket)) return false;
    state.phase=HazeQuitPhase::Ready; state.message="Closing Haze...";
    return true;
}
bool haze_quit_take_shutdown(bool producer_parked) {
    std::lock_guard lock(quit_mutex);
    if (!producer_parked || state.phase!=HazeQuitPhase::Ready) return false;
    state.phase=HazeQuitPhase::Stopping; return true;
}
