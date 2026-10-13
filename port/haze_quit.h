#pragma once
#include <cstdint>
#include <string>

enum class HazeQuitPhase { Idle, Confirm, Waiting, Failed, Ready, Stopping };
struct HazeQuitState {
    HazeQuitPhase phase=HazeQuitPhase::Idle;
    bool gameplay=false;
    uint64_t generation=0;
    std::string message;
};

HazeQuitState haze_quit_state();
// An offer belongs to the current main/pause menu generation. A stale button
// cannot confirm a later request or request an exit from a different page.
bool haze_quit_request(bool gameplay,uint64_t menu_generation);
bool haze_quit_confirm(uint64_t generation);
bool haze_quit_window_close();
bool haze_quit_cancel(uint64_t generation);
// Called at the owning guest frame boundary, after queued settings work. On
// Ready the caller must park that producer until cancellation or host exit.
// Waiting continues rendering; it never treats elapsed time as save success.
bool haze_quit_poll(uint64_t menu_generation,bool guest_busy,bool settings_busy);
// Window/render thread: commit once, only after the producer is parked.
bool haze_quit_take_shutdown(bool producer_parked);

struct HazeQuitOffer {
    uint64_t generation=0;
    bool available=false,gameplay=false;
};
HazeQuitOffer haze_quit_offer();
bool haze_quit_guest_parked();
bool haze_quit_guest_ready();
void haze_quit_resume_guest();
struct ppu_context;
void haze_quit_guest_page(ppu_context* context);
void haze_quit_guest_tick(ppu_context* context);
