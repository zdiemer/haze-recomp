#ifndef HAZE_JOB_TRACE_H
#define HAZE_JOB_TRACE_H

#include <cstdint>

enum class HazeJobTraceKind : unsigned {
    Submit = 1,
    Dispatch = 2,
    Kernel = 3,
    Wait = 4,
    Handler = 5,
    Completion = 6,
    SpuSubmit = 7,
    SpuCreateThread = 8,
    SpuSetup = 9,
    SpuExecute = 10,
    SpuFinish = 11,
    SpuCleanup = 12,
    SpuCreateFailed = 13,
    SpuSetupFailed = 14,
    SpuReuseWorker = 15,
    SpuDispatchDiscovery = 16,
};

#if defined(HAZE_JOB_TRACE_TEST_CAPACITY)
#define HAZE_JOB_TRACE_CAPACITY HAZE_JOB_TRACE_TEST_CAPACITY
#else
// Diagnostic builds only: retain both 24-second comparison windows plus
// warmup/close time. Consumers must still validate actual ring coverage.
#define HAZE_JOB_TRACE_CAPACITY 2097152u
#endif

#if defined(HAZE_JOB_TRACE) && defined(_WIN32)
class HazeJobTraceScope {
public:
    HazeJobTraceScope(unsigned kind, unsigned guest_thread,
        std::uint64_t toc, std::uint64_t caller,
        const std::uint64_t* args, unsigned arg_count) noexcept;
    ~HazeJobTraceScope() noexcept;

    HazeJobTraceScope(const HazeJobTraceScope&) = delete;
    HazeJobTraceScope& operator=(const HazeJobTraceScope&) = delete;

    void set_slot(std::uint32_t slot) noexcept;
    void set_route(std::uint32_t mode) noexcept { if (active_) route_ = mode; }
    void set_slot_base(std::uint32_t base) noexcept { if (active_) slot_base_ = base; }
    void set_dispatch(std::uint64_t code, std::uint64_t toc) noexcept {
        if (active_) {
            dispatch_code_ = code;
            dispatch_toc_ = toc;
        }
    }

private:
    friend void haze_job_trace_dump() noexcept;
    friend void haze_job_trace_record(HazeJobTraceScope&, std::int64_t) noexcept;
    friend void haze_job_trace_interval(unsigned, std::uint64_t, std::uint64_t,
        const std::uint64_t*, unsigned) noexcept;
    HazeJobTraceScope* previous_;
    bool active_;
    bool submit_generation_assigned_;
    unsigned kind_, guest_thread_, arg_count_, depth_;
    std::uint32_t host_tid_, slot_, route_, slot_base_;
    std::int64_t begin_qpc_;
    std::uint64_t toc_, caller_, args_[8];
    std::uint64_t generation_, dispatch_code_, dispatch_toc_;
    std::uint64_t span_id_, parent_id_;
    std::int64_t external_end_qpc_;
};

void haze_job_trace_dump() noexcept;
void haze_job_trace_interval(unsigned kind, std::uint64_t begin_qpc, std::uint64_t end_qpc,
    const std::uint64_t* args, unsigned arg_count) noexcept;
#else
class HazeJobTraceScope {
public:
    HazeJobTraceScope(unsigned, unsigned, std::uint64_t, std::uint64_t,
        const std::uint64_t*, unsigned) noexcept {}
    void set_slot(std::uint32_t) noexcept {}
    void set_route(std::uint32_t) noexcept {}
    void set_slot_base(std::uint32_t) noexcept {}
    void set_dispatch(std::uint64_t, std::uint64_t) noexcept {}
};

inline void haze_job_trace_dump() noexcept {}
inline void haze_job_trace_interval(unsigned, std::uint64_t, std::uint64_t,
    const std::uint64_t*, unsigned) noexcept {}
#endif

#endif
