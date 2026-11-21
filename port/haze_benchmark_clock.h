#pragma once
#include <cstdint>

struct ppu_context;

// Diagnostic deterministic timedemo; inert unless HAZE_BENCHMARK_CLOCK_US is set.
bool haze_benchmark_clock_active();
void haze_benchmark_clock_register();
// Guest frame boundary (loc_006812DC), main game thread only.
void haze_benchmark_clock_frame(ppu_context* ctx);
// The local player's look update ran with gameplay input allowed this frame.
void haze_benchmark_clock_note_gameplay();
// Camera sweep schedule in gameplay frames. Returns false when no timeline is
// configured; otherwise sets token (0 = inactive) and the sweep phase.
bool haze_benchmark_clock_sweep(unsigned& token, double& seconds);
void haze_benchmark_clock_dump();
