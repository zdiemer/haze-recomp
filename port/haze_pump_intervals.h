#pragma once
#include <cstdint>

// Single render-thread accounting. Scopes must be disjoint and completed
// before finish(): the drain returns before its pending flip is presented.
struct HazePumpInterval {
    int64_t begin=0, end=0, wait=0, fifo=0, present=0, tick=0;
    uint64_t cpu_100ns=0;
    unsigned frame=0, flips=0, waits=0, queued_waits=0, drains=0;
    int64_t signaled_wait=0, timeout_wait=0, sleep_wait=0, failed_wait=0;
    int64_t max_wait=0;
    unsigned signaled_waits=0, timeout_waits=0, sleep_waits=0, failed_waits=0;
    unsigned queued_after_waits=0, empty_signals=0;
    bool cpu_valid=false;
};
struct HazePumpIntervals {
    HazePumpInterval pending{};
    int64_t previous=0;
    uint64_t previous_cpu=0;
    bool previous_cpu_valid=false;

    bool finish(int64_t now, uint64_t cpu, bool cpu_valid,
                unsigned frame, unsigned flips, HazePumpInterval& row) {
        const bool ready=previous!=0;
        row=pending;
        row.begin=previous; row.end=now; row.frame=frame; row.flips=flips;
        row.cpu_valid=cpu_valid && previous_cpu_valid && cpu>=previous_cpu;
        row.cpu_100ns=row.cpu_valid ? cpu-previous_cpu : 0;
        pending={}; previous=now; previous_cpu=cpu;
        previous_cpu_valid=cpu_valid;
        return ready;
    }
};
