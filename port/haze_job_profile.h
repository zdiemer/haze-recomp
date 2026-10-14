#pragma once
#include <cstdint>

// Diagnostic-only scopes around the original completion loop. Stages include
// their callees; nested loop rows must not be added to their parents.
#if defined(HAZE_JOB_PROFILE) && defined(_WIN32)
struct HazeJobProfileScope {
    uint64_t begin=0, phase_begin=0, ticks[4]{}, calls[4]{};
    uint64_t job=0, caller=0;
    unsigned depth=0;
    bool active=false;
    HazeJobProfileScope(unsigned thread, uint64_t toc, uint64_t handle, uint64_t lr);
    ~HazeJobProfileScope();
    void start(unsigned phase);
    void stop(unsigned phase);
};
void haze_job_profile_dump();
#else
struct HazeJobProfileScope {
    HazeJobProfileScope(unsigned, uint64_t, uint64_t, uint64_t) {}
    void start(unsigned) {}
    void stop(unsigned) {}
};
inline void haze_job_profile_dump() {}
#endif
