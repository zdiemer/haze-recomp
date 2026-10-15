#include "haze_producer_profile.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <algorithm>

#if defined(HAZE_PRODUCER_PROFILE_TEST_CLOCKS)
extern "C" int haze_producer_test_qpc(std::uint64_t* value) noexcept;
extern "C" int haze_producer_test_frequency(std::uint64_t* value) noexcept;
extern "C" int haze_producer_test_cycles(std::uint64_t* value) noexcept;
#endif

namespace haze_producer_profile {
namespace {
#if defined(HAZE_PRODUCER_PROFILE_TEST_CAPACITY)
constexpr std::uint64_t kCapacity = HAZE_PRODUCER_PROFILE_TEST_CAPACITY;
#else
constexpr std::uint64_t kCapacity = 1048576;
#endif

struct record {
    std::uint64_t id;
    std::uint64_t parent_id;
    std::uint64_t generation;
    std::uint64_t qpc_begin;
    std::uint64_t qpc_end;
    std::uint64_t cycles_begin;
    std::uint64_t cycles_end;
    std::uint32_t host_tid;
    std::uint32_t guest_ea;
    std::uint32_t callsite_lr;
    std::uint32_t callee_ea;
    std::uint8_t kind; // 0=frame, 1=call
    std::uint8_t qpc_valid;
    std::uint8_t cycles_valid;
    std::uint8_t complete;
};
static_assert(sizeof(record) <= 96, "producer profile ring must stay bounded");
static_assert(kCapacity > 0, "producer profile ring capacity must be nonzero");

struct configuration {
    const char* csv_path;
    std::uint64_t qpc_frequency;
    bool coarse;
};

// A diagnostic frontier through the main frame and its render-record branch.
// Include frame setup, whose formerly unprofiled interval can dominate tails.
// Omit per-material/item loops; setup queue entries are intentionally retained.
// Omitted scopes leave the nearest recorded ancestor active.
bool coarse_edge(std::uint32_t lr, std::uint32_t callee) noexcept {
    switch (lr) {
    case 0x00680FF8: return callee == 0x006DD638;
    case 0x00681004: return callee == 0x0068ECD8;
    case 0x00681014: return callee == 0x00719EF8;
    case 0x0068101C: return callee == 0x00682060;
    case 0x00681024: return callee == 0x0076F580;
    case 0x0068102C: return callee == 0x006CBED0;
    case 0x0068103C: return callee == 0x00C2796C;
    case 0x00681054: return callee == 0x00C2786C;
    case 0x006810B8: return callee == 0x00723660;
    case 0x006810EC: return callee == 0x00680A30;
    case 0x00681100: return callee == 0x006C8870;
    case 0x0068110C: return callee == 0x006A6220;
    case 0x00681114: return callee == 0x006D1E90;
    case 0x00681120: return callee == 0x0074C950;
    case 0x00681128: return callee == 0x006808D8;
    case 0x0068112C: return callee == 0x0076E6C0;
    case 0x00681134: return callee == 0x00709838;
    case 0x0068113C: return callee == 0x006B2DF0;
    case 0x00681148: return callee == 0x0076F798;
    case 0x006812F8: return callee == 0x006869F0;
    case 0x00681184: return callee == 0x005FBB70;
    case 0x0068128C: return callee == 0x00697C70;
    case 0x006812A0: return callee == 0x00686810;
    case 0x006811E8: return callee == 0x006A6BE8;
    case 0x00686A50: return callee == 0x00684ED8;
    case 0x006851D0: return callee == 0x006DEC78;
    // 006DEC78 tail-dispatches into 006DE730; keep the actual call-site LR.
    case 0x006DE828: return callee == 0x006AC9A8;
    case 0x006DE814: return callee == 0x0070CF48;
    case 0x006DEB34: return callee == 0x00713D90;
    case 0x006DE844: return callee == 0x006F52F8;
    case 0x006ACB70: return callee == 0x007EB5A8;
    case 0x007EBBD0: return callee == 0x007E41E8;
    default: return false;
    }
}

const configuration& config() noexcept {
    static const configuration value = []() noexcept {
        const char* path = std::getenv("HAZE_PRODUCER_PROFILE_CSV");
        const char* mode = std::getenv("HAZE_PRODUCER_PROFILE_COARSE");
        const bool coarse = mode && std::strcmp(mode, "1") == 0;
#if defined(HAZE_PRODUCER_PROFILE_TEST_CLOCKS)
        std::uint64_t frequency = 0;
        if (!path || !*path || !haze_producer_test_frequency(&frequency) || !frequency)
            return configuration{nullptr, 0, coarse};
        return configuration{path, frequency, coarse};
#else
        LARGE_INTEGER frequency{};
        if (!path || !*path || !QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0) {
            return configuration{nullptr, 0, coarse};
        }
        return configuration{path, static_cast<std::uint64_t>(frequency.QuadPart), coarse};
#endif
    }();
    return value;
}

record g_ring[kCapacity]{};
std::mutex g_mutex;
bool g_frozen = false; // protected by g_mutex; fast check uses atomic below
volatile long g_frozen_fast = 0;
std::uint64_t g_next_id = 0;
std::uint64_t g_begun = 0;
std::uint64_t g_completed = 0;
std::uint64_t g_inflight = 0;
std::uint64_t g_overwritten = 0;
std::uint64_t g_overwritten_inflight = 0;
std::uint64_t g_late_discarded = 0;
std::uint64_t g_discarded_invalid_qpc_observations = 0;
std::uint64_t g_max_discarded_end_qpc = 0;
std::uint64_t g_earliest_overwritten_inflight_begin_qpc = 0;
thread_local scope* g_current_scope = nullptr;

bool is_frozen() noexcept {
    return InterlockedCompareExchange(&g_frozen_fast, 0, 0) != 0;
}

bool query_qpc(std::uint64_t& out) noexcept {
#if defined(HAZE_PRODUCER_PROFILE_TEST_CLOCKS)
    return haze_producer_test_qpc(&out) != 0;
#else
    LARGE_INTEGER qpc{};
    if (!QueryPerformanceCounter(&qpc) || qpc.QuadPart < 0) {
        out = 0;
        return false;
    }
    out = static_cast<std::uint64_t>(qpc.QuadPart);
    return true;
#endif
}

bool query_cycles(std::uint64_t& out) noexcept {
#if defined(HAZE_PRODUCER_PROFILE_TEST_CLOCKS)
    return haze_producer_test_cycles(&out) != 0;
#else
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        out = 0;
        return false;
    }
    out = cycles;
    return true;
#endif
}

void update_max_discarded(std::uint64_t end_qpc, bool valid) noexcept {
    if (valid && end_qpc > g_max_discarded_end_qpc)
        g_max_discarded_end_qpc = end_qpc;
}
} // namespace

scope::~scope() noexcept {
    if (!active_)
        return;
    active_ = false;
    if (g_current_scope == this)
        g_current_scope = previous_;
    if (is_frozen())
        return;

    // QPC encloses the body and cycle query boundary; cycle readings remain
    // raw QueryThreadCycleTime values and are never presented as milliseconds.
    std::uint64_t cycles_end = 0;
    const bool cycles_ok = query_cycles(cycles_end);
    std::uint64_t qpc_end = 0;
    const bool qpc_ok = query_qpc(qpc_end);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_frozen)
        return;
    if (g_inflight)
        --g_inflight;
    record& row = g_ring[id_ % kCapacity];
    if (row.id != id_) {
        ++g_late_discarded;
        if (!qpc_ok || !qpc_begin_valid_)
            ++g_discarded_invalid_qpc_observations;
        update_max_discarded(qpc_end, qpc_ok);
        return;
    }
    row.qpc_end = qpc_end;
    row.qpc_valid = static_cast<std::uint8_t>(row.qpc_valid && qpc_ok && qpc_end >= row.qpc_begin);
    row.cycles_end = cycles_end;
    row.cycles_valid = static_cast<std::uint8_t>(row.cycles_valid && cycles_ok &&
        cycles_end >= row.cycles_begin);
    row.complete = 1;
    ++g_completed;
}

void scope::begin(unsigned kind, const scope* parent, std::uint64_t generation,
    std::uint32_t guest_ea, std::uint32_t callsite_lr,
    std::uint32_t callee_ea) noexcept {
    if (parent && !parent->active())
        return;
    if (!parent)
        parent = g_current_scope;
    if (parent && !parent->active())
        return;
    if (generation == 0 && parent)
        generation = parent->generation();
    const configuration& cfg = config();
    if (!cfg.csv_path)
        return;
    if (kind == 1 && cfg.coarse && !coarse_edge(callsite_lr, callee_ea))
        return;
    if (is_frozen())
        return;

    record fresh{};
    fresh.parent_id = parent ? parent->id() : 0;
    fresh.generation = generation;
    fresh.host_tid = GetCurrentThreadId();
    fresh.guest_ea = guest_ea;
    fresh.callsite_lr = callsite_lr;
    fresh.callee_ea = callee_ea;
    fresh.kind = static_cast<std::uint8_t>(kind);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_frozen)
        return;
    // Sample after the record lock is acquired so contention on the ring does
    // not inflate the measured guest interval. The scope begins just before
    // this constructor returns; its destructor samples before taking the lock.
    fresh.cycles_valid = static_cast<std::uint8_t>(query_cycles(fresh.cycles_begin));
    fresh.qpc_valid = static_cast<std::uint8_t>(query_qpc(fresh.qpc_begin));
    fresh.id = ++g_next_id;
    record& slot = g_ring[fresh.id % kCapacity];
    if (slot.id != 0) {
        ++g_overwritten;
        if (!slot.complete) {
            ++g_overwritten_inflight;
            if (!slot.qpc_valid)
                ++g_discarded_invalid_qpc_observations;
            if (slot.qpc_valid && slot.qpc_begin &&
                (!g_earliest_overwritten_inflight_begin_qpc ||
                 slot.qpc_begin < g_earliest_overwritten_inflight_begin_qpc))
                g_earliest_overwritten_inflight_begin_qpc = slot.qpc_begin;
        } else {
            if (!slot.qpc_valid)
                ++g_discarded_invalid_qpc_observations;
            update_max_discarded(slot.qpc_end, slot.qpc_valid != 0);
        }
    }
    slot = fresh;
    ++g_begun;
    ++g_inflight;
    id_ = fresh.id;
    generation_ = generation;
    qpc_begin_valid_ = fresh.qpc_valid != 0;
    previous_ = g_current_scope;
    g_current_scope = this;
    active_ = true;
}

frame_scope::frame_scope(unsigned guest_thread, std::uint64_t toc,
    std::uint32_t guest_ea, std::uint64_t generation,
    const scope* parent) noexcept {
    // Avoid even the cached environment read on unrelated PPU threads.
    if (guest_thread != 1 || toc != 0xE94EE0)
        return;
    begin(0, parent, generation, guest_ea, 0, 0);
}

call_scope::call_scope(const scope& parent, std::uint32_t callsite_lr,
    std::uint32_t callee_ea) noexcept {
    if (!parent.active())
        return;
    begin(1, &parent, parent.generation(), 0, callsite_lr, callee_ea);
}

call_scope::call_scope(std::uint32_t callsite_lr,
    std::uint32_t callee_ea) noexcept {
    // Do not touch the lazy configuration or query clocks on ordinary calls
    // that have no active profiled ancestor.
    if (!g_current_scope || !g_current_scope->active())
        return;
    begin(1, nullptr, 0, 0, callsite_lr, callee_ea);
}

void haze_producer_profile_dump() noexcept {
    const configuration& cfg = config();
    if (!cfg.csv_path)
        return;

    std::uint64_t total, completed, inflight, overwritten, overwritten_inflight;
    std::uint64_t late_discarded, discarded_invalid_qpc;
    std::uint64_t max_discarded_end, earliest_overwritten_inflight;
    std::uint64_t dump_qpc = 0;
    bool dump_qpc_valid = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_frozen)
            return;
        dump_qpc_valid = query_qpc(dump_qpc);
        g_frozen = true;
        InterlockedExchange(&g_frozen_fast, 1);
        total = g_begun;
        completed = g_completed;
        inflight = g_inflight;
        overwritten = g_overwritten;
        overwritten_inflight = g_overwritten_inflight;
        late_discarded = g_late_discarded;
        discarded_invalid_qpc = g_discarded_invalid_qpc_observations;
        max_discarded_end = g_max_discarded_end_qpc;
        earliest_overwritten_inflight = g_earliest_overwritten_inflight_begin_qpc;
    }

    const std::uint64_t first_id = total > kCapacity ? total - kCapacity + 1 : 1;
    std::uint64_t retained_rows = 0, retained_first_id = 0, retained_last_id = 0;
    std::uint64_t retained_first_begin_qpc = 0, retained_completed_rows = 0;
    std::uint64_t retained_inflight_rows = 0;
    for (std::uint64_t id = first_id; id <= total; ++id) {
        const record& row = g_ring[id % kCapacity];
        if (row.id != id)
            continue;
        if (!retained_rows) {
            retained_first_id = id;
            retained_first_begin_qpc = row.qpc_begin;
        }
        retained_last_id = id;
        ++retained_rows;
        if (row.complete) ++retained_completed_rows;
        else ++retained_inflight_rows;
    }

    FILE* file = std::fopen(cfg.csv_path, "wb");
    if (!file) {
        std::fprintf(stderr, "[producer-profile] cannot open CSV output\n");
        return;
    }
    bool write_failed = std::fprintf(file,
        "#qpc_frequency,%llu,begun,%llu,completed,%llu,inflight_at_dump,%llu,"
        "overwritten,%llu,overwritten_inflight,%llu,late_discarded,%llu,"
        "discarded_invalid_qpc_observations,%llu,"
        "max_discarded_end_qpc,%llu,earliest_overwritten_inflight_begin_qpc,%llu,"
        "capacity,%llu,retained_rows,%llu,retained_completed_rows,%llu,"
        "retained_inflight_rows,%llu,retained_first_id,%llu,retained_last_id,%llu,"
        "retained_first_begin_qpc,%llu,dump_qpc,%llu,dump_qpc_valid,%u,coarse,%u\n",
        static_cast<unsigned long long>(cfg.qpc_frequency),
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(completed),
        static_cast<unsigned long long>(inflight),
        static_cast<unsigned long long>(overwritten),
        static_cast<unsigned long long>(overwritten_inflight),
        static_cast<unsigned long long>(late_discarded),
        static_cast<unsigned long long>(discarded_invalid_qpc),
        static_cast<unsigned long long>(max_discarded_end),
        static_cast<unsigned long long>(earliest_overwritten_inflight),
        static_cast<unsigned long long>(kCapacity),
        static_cast<unsigned long long>(retained_rows),
        static_cast<unsigned long long>(retained_completed_rows),
        static_cast<unsigned long long>(retained_inflight_rows),
        static_cast<unsigned long long>(retained_first_id),
        static_cast<unsigned long long>(retained_last_id),
        static_cast<unsigned long long>(retained_first_begin_qpc),
        static_cast<unsigned long long>(dump_qpc), dump_qpc_valid ? 1u : 0u,
        cfg.coarse ? 1u : 0u) < 0;
    write_failed |= std::fprintf(file,
        "kind,id,parent_id,generation,guest_ea,callsite_lr,callee_ea,host_tid,"
        "qpc_begin,qpc_end,qpc_valid,cycles_delta,cycles_valid,completed\n") < 0;

    for (std::uint64_t id = first_id; id <= total; ++id) {
        const record& row = g_ring[id % kCapacity];
        if (row.id != id)
            continue;
        write_failed |= std::fprintf(file,
            "%u,%llu,%llu,%llu,%u,%u,%u,%u,%llu,%llu,%u,%llu,%u,%u\n",
            row.kind,
            static_cast<unsigned long long>(row.id),
            static_cast<unsigned long long>(row.parent_id),
            static_cast<unsigned long long>(row.generation), row.guest_ea,
            row.callsite_lr, row.callee_ea, row.host_tid,
            static_cast<unsigned long long>(row.qpc_begin),
            static_cast<unsigned long long>(row.qpc_end),
            row.complete ? row.qpc_valid : 0,
            static_cast<unsigned long long>(row.cycles_end >= row.cycles_begin ?
                row.complete ? row.cycles_end - row.cycles_begin : 0 : 0),
            row.complete ? row.cycles_valid : 0, row.complete ? 1 : 0) < 0;
    }
    write_failed |= std::ferror(file) != 0;
    const int close_result = std::fclose(file);
    if (write_failed || close_result != 0)
        std::fprintf(stderr, "[producer-profile] failed while writing CSV output\n");
}

} // namespace haze_producer_profile

#else
namespace haze_producer_profile {
scope::~scope() noexcept = default;
void scope::begin(unsigned, const scope*, std::uint64_t, std::uint32_t,
    std::uint32_t, std::uint32_t) noexcept {}
frame_scope::frame_scope(unsigned, std::uint64_t, std::uint32_t,
    std::uint64_t, const scope*) noexcept {}
call_scope::call_scope(const scope&, std::uint32_t, std::uint32_t) noexcept {}
call_scope::call_scope(std::uint32_t, std::uint32_t) noexcept {}
void haze_producer_profile_dump() noexcept {}
} // namespace haze_producer_profile
#endif

