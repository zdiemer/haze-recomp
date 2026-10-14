#include "haze_job_trace.h"

#if defined(HAZE_JOB_TRACE) && defined(_WIN32)

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

constexpr std::uint64_t kInvalidSequence = (std::numeric_limits<std::uint64_t>::max)();
constexpr std::uint32_t kInvalidSlot = (std::numeric_limits<std::uint32_t>::max)();
constexpr std::size_t kMaxPath = 4096;

struct Event {
    std::uint64_t ticket;
    std::int64_t begin_qpc;
    std::int64_t end_qpc;
    std::uint32_t host_tid;
    std::uint32_t guest_thread;
    std::uint32_t kind;
    std::uint32_t arg_count;
    std::uint32_t slot;
    std::uint32_t depth;
    std::uint64_t caller;
    std::uint64_t toc;
    std::uint64_t args[8];
    std::uint64_t generation;
    std::uint64_t dispatch_code;
    std::uint64_t dispatch_toc;
    std::uint64_t span_id;
    std::uint64_t parent_id;
    std::uint32_t route;
    std::uint32_t slot_base;
};

struct RingSlot {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> sequence{kInvalidSequence};
    Event event{};
};

static_assert(HAZE_JOB_TRACE_CAPACITY > 0, "trace ring capacity must be nonzero");
static_assert((sizeof(RingSlot) + sizeof(std::uint64_t)) * static_cast<std::size_t>(HAZE_JOB_TRACE_CAPACITY) <= 512u * 1024u * 1024u,
    "trace ring must stay at or under 512 MiB");

RingSlot g_ring[HAZE_JOB_TRACE_CAPACITY];
std::uint64_t g_span_scratch[HAZE_JOB_TRACE_CAPACITY];
std::atomic<std::uint64_t> g_next_ticket{0};
std::atomic<std::uint64_t> g_next_span{1};
std::atomic<std::uint64_t> g_generation[512]{};
std::atomic<std::uint64_t> g_dropped_busy{0};
std::atomic<std::uint64_t> g_dropped_stale{0};
std::atomic<std::uint64_t> g_dropped_frozen{0};
std::atomic<std::int64_t> g_max_discarded_end_qpc{0};
std::atomic<std::uint32_t> g_writers{0};
std::atomic<std::uint32_t> g_open_scopes{0};
std::atomic<unsigned> g_init_state{0};
std::atomic<bool> g_accepting{false};
std::atomic<bool> g_dump_started{false};
bool g_enabled = false;
char g_path[kMaxPath]{};
thread_local HazeJobTraceScope* t_current_scope = nullptr;

void initialize() noexcept {
    unsigned state = g_init_state.load(std::memory_order_acquire);
    if (state == 2) return;
    unsigned expected = 0;
    if (g_init_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        const char* path = std::getenv("HAZE_JOB_TRACE_CSV");
        if (path && path[0] != '\0') {
            const std::size_t length = std::strlen(path);
            if (length < sizeof(g_path)) {
                std::memcpy(g_path, path, length + 1);
                g_enabled = true;
                g_accepting.store(true, std::memory_order_seq_cst);
            }
        }
        g_init_state.store(2, std::memory_order_release);
    } else {
        while (g_init_state.load(std::memory_order_acquire) != 2) YieldProcessor();
    }
}

bool enter_writer() noexcept {
    if (!g_accepting.load(std::memory_order_seq_cst)) return false;
    g_writers.fetch_add(1, std::memory_order_seq_cst);
    if (!g_accepting.load(std::memory_order_seq_cst)) {
        g_writers.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }
    return true;
}

void leave_writer() noexcept {
    g_writers.fetch_sub(1, std::memory_order_seq_cst);
}

void note_discarded_end(std::int64_t end_qpc) noexcept {
    std::int64_t prior = g_max_discarded_end_qpc.load(std::memory_order_relaxed);
    while (prior < end_qpc &&
        !g_max_discarded_end_qpc.compare_exchange_weak(prior, end_qpc,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

bool has_span(const std::uint64_t* spans, std::size_t count, std::uint64_t id) noexcept {
    return id != 0 && std::binary_search(spans, spans + count, id);
}

void print_event(FILE* file, const Event& event, bool parent_missing) noexcept {
    std::fprintf(file,
        "event,%" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32,
        event.ticket, event.begin_qpc, event.end_qpc, event.host_tid, event.guest_thread,
        event.kind, event.caller, event.toc, event.arg_count);
    for (unsigned i = 0; i != 8; ++i) std::fprintf(file, ",%" PRIu64, event.args[i]);
    std::fprintf(file,
        ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%u",
        event.slot, event.generation, event.dispatch_code, event.dispatch_toc,
        event.span_id, event.parent_id, event.depth, parent_missing ? 1u : 0u);
    std::fprintf(file, ",,,,,,,,,%" PRIu32 ",%" PRIu32 "\n", event.route, event.slot_base);
}

} // namespace

void haze_job_trace_record(HazeJobTraceScope& scope, std::int64_t end_qpc) noexcept {
    const std::uint64_t ticket = g_next_ticket.fetch_add(1, std::memory_order_relaxed);
    RingSlot& slot = g_ring[ticket % HAZE_JOB_TRACE_CAPACITY];
    if (slot.busy.test_and_set(std::memory_order_acquire)) {
        g_dropped_busy.fetch_add(1, std::memory_order_relaxed);
        note_discarded_end(end_qpc);
        return;
    }
    const std::uint64_t prior = slot.sequence.load(std::memory_order_acquire);
    if (prior != kInvalidSequence && prior > ticket) {
        g_dropped_stale.fetch_add(1, std::memory_order_relaxed);
        note_discarded_end(end_qpc);
        slot.busy.clear(std::memory_order_release);
        return;
    }
    if (prior != kInvalidSequence) note_discarded_end(slot.event.end_qpc);

    Event event{};
    event.ticket = ticket;
    event.begin_qpc = scope.begin_qpc_;
    event.end_qpc = end_qpc;
    event.host_tid = scope.host_tid_;
    event.guest_thread = scope.guest_thread_;
    event.kind = scope.kind_;
    event.arg_count = scope.arg_count_;
    event.slot = scope.slot_;
    event.depth = scope.depth_;
    event.caller = scope.caller_;
    event.toc = scope.toc_;
    std::memcpy(event.args, scope.args_, sizeof(event.args));
    event.generation = scope.generation_;
    event.dispatch_code = scope.dispatch_code_;
    event.dispatch_toc = scope.dispatch_toc_;
    event.span_id = scope.span_id_;
    event.parent_id = scope.parent_id_;
    event.route = scope.route_;
    event.slot_base = scope.slot_base_;
    slot.event = event;
    slot.sequence.store(ticket, std::memory_order_release);
    slot.busy.clear(std::memory_order_release);
}

HazeJobTraceScope::HazeJobTraceScope(unsigned kind, unsigned guest_thread,
    std::uint64_t toc, std::uint64_t caller, const std::uint64_t* args,
    unsigned arg_count) noexcept
    : previous_(nullptr), active_(false), submit_generation_assigned_(false),
      kind_(kind), guest_thread_(guest_thread), arg_count_(0), depth_(0),
      host_tid_(0), slot_(kInvalidSlot), route_(kInvalidSlot), slot_base_(0), begin_qpc_(0), toc_(toc), caller_(caller),
      args_{}, generation_(0), dispatch_code_(0), dispatch_toc_(0), span_id_(0), parent_id_(0), external_end_qpc_(0) {
    initialize();
    if (!g_enabled || !enter_writer()) return;

    previous_ = t_current_scope;
    if (previous_) {
        parent_id_ = previous_->span_id_;
        depth_ = previous_->depth_ + 1;
        if (kind_ != static_cast<unsigned>(HazeJobTraceKind::Submit) &&
            kind_ != static_cast<unsigned>(HazeJobTraceKind::Wait) &&
            kind_ != static_cast<unsigned>(HazeJobTraceKind::Completion)) {
            slot_ = previous_->slot_;
            generation_ = previous_->generation_;
            route_ = previous_->route_;
            slot_base_ = previous_->slot_base_;
        }
    }
    span_id_ = g_next_span.fetch_add(1, std::memory_order_relaxed);
    host_tid_ = GetCurrentThreadId();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    begin_qpc_ = now.QuadPart;
    arg_count_ = (std::min)(arg_count, 8u);
    if (args && arg_count_) std::memcpy(args_, args, sizeof(std::uint64_t) * arg_count_);
    g_open_scopes.fetch_add(1, std::memory_order_relaxed);
    active_ = true;
    t_current_scope = this;
    leave_writer();
}

HazeJobTraceScope::~HazeJobTraceScope() noexcept {
    if (!active_) return;
    LARGE_INTEGER now{};
    if (external_end_qpc_) now.QuadPart = external_end_qpc_;
    else QueryPerformanceCounter(&now);
    if (enter_writer()) {
        haze_job_trace_record(*this, now.QuadPart);
        g_open_scopes.fetch_sub(1, std::memory_order_relaxed);
        leave_writer();
    } else {
        g_dropped_frozen.fetch_add(1, std::memory_order_relaxed);
    }
    t_current_scope = previous_;
}

void HazeJobTraceScope::set_slot(std::uint32_t slot) noexcept {
    if (!active_) return;
    if (slot >= 512) {
        slot_ = kInvalidSlot;
        generation_ = 0;
        submit_generation_assigned_ = false;
        return;
    }
    const std::uint32_t prior_slot = slot_;
    slot_ = slot;
    if (kind_ == static_cast<unsigned>(HazeJobTraceKind::Submit)) {
        if (!submit_generation_assigned_ || prior_slot != slot) {
            generation_ = g_generation[slot].fetch_add(1, std::memory_order_seq_cst) + 1;
            submit_generation_assigned_ = true;
        }
    } else {
        generation_ = g_generation[slot].load(std::memory_order_seq_cst);
    }
}

void haze_job_trace_interval(unsigned kind, std::uint64_t begin_qpc, std::uint64_t end_qpc,
    const std::uint64_t* args, unsigned arg_count) noexcept {
    if (!begin_qpc || end_qpc < begin_qpc || end_qpc > INT64_MAX) return;
    HazeJobTraceScope scope(kind, 0, 0, 0, args, arg_count);
    if (scope.active_) {
        scope.begin_qpc_ = static_cast<std::int64_t>(begin_qpc);
        scope.external_end_qpc_ = static_cast<std::int64_t>(end_qpc);
    }
}

void haze_job_trace_dump() noexcept {
    initialize();
    if (!g_enabled || g_dump_started.exchange(true, std::memory_order_acq_rel)) return;
    g_accepting.store(false, std::memory_order_seq_cst);
    while (g_writers.load(std::memory_order_seq_cst) != 0) SwitchToThread();

    const std::uint64_t next = g_next_ticket.load(std::memory_order_seq_cst);
    const std::uint64_t first = next > HAZE_JOB_TRACE_CAPACITY ? next - HAZE_JOB_TRACE_CAPACITY : 0;
    std::uint64_t missing_count = 0;
    for (std::uint64_t ticket = first; ticket < next; ++ticket) {
        RingSlot& slot = g_ring[ticket % HAZE_JOB_TRACE_CAPACITY];
        if (slot.sequence.load(std::memory_order_acquire) != ticket) ++missing_count;
    }
    for (std::size_t i = 0; i != HAZE_JOB_TRACE_CAPACITY; ++i) {
        RingSlot& slot = g_ring[i];
        const std::uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
        if (sequence != kInvalidSequence && sequence < first)
            note_discarded_end(slot.event.end_qpc);
    }

    std::size_t span_used = 0;
    for (std::uint64_t ticket = first; ticket < next; ++ticket) {
        RingSlot& slot = g_ring[ticket % HAZE_JOB_TRACE_CAPACITY];
        if (slot.sequence.load(std::memory_order_acquire) == ticket)
            g_span_scratch[span_used++] = slot.event.span_id;
    }
    std::sort(g_span_scratch, g_span_scratch + span_used);

    FILE* file = std::fopen(g_path, "wb");
    if (!file) return;
    std::fprintf(file,
        "record_type,ticket,begin_qpc,end_qpc,host_tid,guest_thread,kind,caller,toc,arg_count,"
        "arg0,arg1,arg2,arg3,arg4,arg5,arg6,arg7,slot,generation,dispatch_code,dispatch_toc,"
        "span_id,parent_id,depth,parent_missing_in_dump,dropped_busy,dropped_stale,dropped_frozen,tickets_before_retained_window,missing_tickets,incomplete_scopes,max_discarded_end_qpc,qpc_frequency,effective_route,slot_base\n");
    for (std::uint64_t ticket = first; ticket < next; ++ticket) {
        RingSlot& slot = g_ring[ticket % HAZE_JOB_TRACE_CAPACITY];
        if (slot.sequence.load(std::memory_order_acquire) != ticket) continue;
        const Event event = slot.event;
        const bool parent_missing = event.parent_id != 0 && !has_span(g_span_scratch, span_used, event.parent_id);
        print_event(file, event, parent_missing);
    }
    std::fprintf(file, "summary");
    for (unsigned i = 0; i != 26; ++i) std::fputc(',', file);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    std::fprintf(file, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%" PRId64 ",%" PRId64 ",,\n",
        g_dropped_busy.load(std::memory_order_relaxed),
        g_dropped_stale.load(std::memory_order_relaxed),
        g_dropped_frozen.load(std::memory_order_relaxed),
        first, missing_count,
        g_open_scopes.load(std::memory_order_relaxed),
        g_max_discarded_end_qpc.load(std::memory_order_relaxed), frequency.QuadPart);
    std::fclose(file);
}

#endif
