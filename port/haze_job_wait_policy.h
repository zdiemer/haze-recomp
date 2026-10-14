#pragma once

#include <cstdint>

namespace haze_job_wait_policy {

enum class ProbeResult { ready_primary, ready_queue, waiting, invalid };
enum class WaitResult { ready_primary, ready_queue, expired, invalid, clock_error };

struct ClockSample {
    bool valid;
    uint64_t ticks;
};

/* The caller supplies a fixed QPC deadline and a nonblocking readiness probe.
 * Probe first so readiness observed at the deadline can still return early;
 * otherwise the clock bounds the pause loop without re-entering guest code. */
template <typename Probe, typename Clock, typename Pause>
WaitResult wait_until_ready_or_deadline(uint64_t start, uint64_t duration,
                                        Probe&& probe, Clock&& clock, Pause&& pause)
{
    for (;;) {
        const ProbeResult state = probe();
        if (state == ProbeResult::ready_primary) return WaitResult::ready_primary;
        if (state == ProbeResult::ready_queue) return WaitResult::ready_queue;
        if (state == ProbeResult::invalid) return WaitResult::invalid;
        const ClockSample now = clock();
        if (!now.valid || now.ticks < start) return WaitResult::clock_error;
        if (now.ticks - start >= duration) return WaitResult::expired;
        pause();
    }
}

} // namespace haze_job_wait_policy
