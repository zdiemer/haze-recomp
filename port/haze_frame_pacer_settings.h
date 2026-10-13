#pragma once
#include <cstring>
#include <atomic>
// -1 retains the startup selection before graphics configuration is loaded.
inline std::atomic<int> haze_live_frame_limit{-1};

// An explicit PC cap above 30 (or unlimited) must also remove the guest wait.
// Keep the established <=30 path and permit a diagnostic override either way.
inline bool haze_native_pacer_selected(const char* override_value, const char* host_limit)
{
    if (override_value && *override_value)
        return std::strcmp(override_value, "1") == 0;
    if (!host_limit || !*host_limit) return false;
    unsigned limit = 0;
    for (const char* digit = host_limit; *digit; ++digit) {
        if (*digit < '0' || *digit > '9') return false;
        limit = limit * 10 + static_cast<unsigned>(*digit - '0');
        if (limit > 240) return false;
    }
    return limit == 0 || limit > 30;
}
