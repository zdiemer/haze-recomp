#pragma once
#include <cstdint>

// Reconcile hardware samples with queued Win32 presses. Message timestamps and
// GetTickCount share the same wrapping millisecond clock. A message older than
// an observed hold is already represented by that hold, not another tap.
struct HazeMenuKey {
    uint32_t observed=0, pending_time=0;
    bool observed_valid=false, pending=false, suppressed=false, held=false;

    void press(uint32_t time) {
        if (!suppressed && (!observed_valid || int32_t(time-observed)>0)) {
            pending=true;
            pending_time=time;
        }
    }
    void reset(bool physical, uint32_t now) {
        observed=now; observed_valid=true;
        pending=false; held=false; suppressed=physical;
    }
    void release(uint32_t time) {
        if (!observed_valid || int32_t(time-observed)>=0) suppressed=false;
    }
    bool sample(bool physical, uint32_t now, bool& new_press) {
        new_press=false;
        if (suppressed) {
            if (!physical) suppressed=false;
            pending=false; held=false;
            observed=now; observed_valid=true;
            return false;
        }
        const bool tap=pending && (!observed_valid || int32_t(pending_time-observed)>0);
        pending=false;
        new_press=physical && !held;
        held=physical;
        if (physical) { observed=now; observed_valid=true; }
        return physical || tap;
    }
};
