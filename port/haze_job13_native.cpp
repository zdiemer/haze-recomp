#include <atomic>
#include <cstdio>

// Logs the first use of each native image-32 operation for diagnostics reports.
extern "C" void haze_job13_native_note(unsigned operation)
{
    static std::atomic<bool> reported[2]{};
    static const char* const names[2] = {"divide", "sqrt"};
    if (operation < 2 && !reported[operation].exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[haze-job13-native] image=32 first_%s\n", names[operation]);
}
