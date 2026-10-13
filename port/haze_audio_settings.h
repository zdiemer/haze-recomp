#pragma once
#include <array>
#include <cstdint>
#include <string>

enum class HazeAudioPhase { Ready, Queued, Saving, Applied, Failed };
// Music, effects, dialogue (0..100), subtitles (0 or 1).
using HazeAudioValues=std::array<int,4>;
struct HazeAudioState {
    HazeAudioValues values{},defaults{};
    uint64_t generation=0;
    bool ready=false;
    HazeAudioPhase phase=HazeAudioPhase::Ready;
    std::string message;
};
HazeAudioState haze_audio_state();
bool haze_audio_apply(const HazeAudioValues& values,uint64_t generation,std::string& message);
void haze_audio_menu_register();
struct ppu_context;
bool haze_audio_guest_open(ppu_context* context);
void haze_audio_guest_tick(ppu_context* context);
