#include "../port/haze_job_trace.h"
#if defined(HAZE_JOB_TRACE)
#error This test verifies the ordinary build without job instrumentation.
#endif
int main() {
    HazeJobTraceScope trace(1, 1, 0, 0,
        reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(1)), 8);
    trace.set_slot(1);
    trace.set_dispatch(1, 1);
    trace.set_route(2);
    haze_job_trace_interval(8, 1, 2,
        reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(1)), 8);
    haze_job_trace_dump();
}
