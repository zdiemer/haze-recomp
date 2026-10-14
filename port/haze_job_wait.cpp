#include "ppu_recomp.h"
#include "haze_job_wait_policy.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" int ppu_native_memory_range(uint32_t address, uint32_t size);
extern "C" int ppu_native_memory_available(void);
extern "C" uint8_t* vm_base;
extern "C" void spu_lockline_lock(void);
extern "C" void spu_lockline_unlock(void);

namespace {
thread_local bool completion_delay = false;
thread_local uint32_t completion_slot = 0;
thread_local uint32_t completion_toc = 0;

struct CompletionScope {
    bool previous_delay = completion_delay;
    uint32_t previous_slot = completion_slot;
    uint32_t previous_toc = completion_toc;

    explicit CompletionScope(ppu_context* ctx) {
        completion_delay = ctx->thread_id == 1 && ctx->gpr[2] == 0xE94EE0 &&
            ctx->lr == 0x00682574 && ctx->gpr[31] < 512;
        completion_slot = completion_delay ? static_cast<uint32_t>(ctx->gpr[31]) : 0;
        completion_toc = completion_delay ? static_cast<uint32_t>(ctx->gpr[2]) : 0;
    }
    ~CompletionScope() {
        completion_delay = previous_delay;
        completion_slot = previous_slot;
        completion_toc = previous_toc;
    }
};

std::atomic<uint64_t> ready_count{0};
std::atomic<uint64_t> ready_primary_count{0};
std::atomic<uint64_t> ready_queue_count{0};
std::atomic<uint64_t> expired_count{0};
std::atomic<uint64_t> native_count{0};
// Counts only enabled, exact-guard attempts that safely fall back to lv2_syscall.
std::atomic<uint64_t> fallback_count{0};
std::atomic_flag counts_dumped = ATOMIC_FLAG_INIT;

#ifdef _WIN32
bool native_wait_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("HAZE_NATIVE_JOB_WAIT");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void emit_mode_once(bool enabled) {
    static std::atomic<bool> emitted{false};
    if (emitted.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (!emitted.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    LARGE_INTEGER timestamp = {};
    const long long qpc = QueryPerformanceCounter(&timestamp) && timestamp.QuadPart > 0
        ? timestamp.QuadPart : 0;
    // Retain the mode-event format consumed by benchmark_present_modes.py.
    std::fprintf(stderr, "[haze-job-wait-mode] enabled=%u qpc=%lld\n",
        enabled ? 1u : 0u, qpc);
}

bool resolve_slot_state(uint32_t* state_ea) {
    if (!completion_delay || completion_slot >= 512 || completion_toc != 0xE94EE0)
        return false;
    const uint64_t anchor_ea64 = static_cast<uint64_t>(completion_toc) - 0x7D6Cu;
    if (anchor_ea64 > UINT32_MAX) return false;
    const uint32_t anchor_ea = static_cast<uint32_t>(anchor_ea64);
    if (!ppu_native_memory_range(anchor_ea, 4)) return false;

    const uint32_t anchor = vm_read32(anchor_ea);
    if (anchor < 0x8000u || (anchor & 3u)) return false;
    const uint32_t table_ea = anchor - 0x8000u;
    if (!ppu_native_memory_range(table_ea, 4)) return false;

    const uint32_t table = vm_read32(table_ea);
    if (!table || (table & 3u)) return false;
    const uint64_t slot_base64 = static_cast<uint64_t>(table) +
        static_cast<uint64_t>(completion_slot) * 0x1450u;
    if (slot_base64 > UINT32_MAX - 9u) return false;
    const uint32_t state = static_cast<uint32_t>(slot_base64) + 8u;
    if (!ppu_native_memory_range(state, 2)) return false;
    *state_ea = state;
    return true;
}

bool resolve_completion_queue(uint32_t* queue_ea) {
    const uint64_t root_ea64 = static_cast<uint64_t>(completion_toc) - 0x7C1Cu;
    if (root_ea64 > UINT32_MAX) return false;
    const uint32_t root_ea = static_cast<uint32_t>(root_ea64);
    if (!ppu_native_memory_range(root_ea, 4)) return false;
    const uint32_t root = vm_read32(root_ea);
    if (root < 0x7FD8u || (root & 3u)) return false;
    const uint32_t table_ea = root - 0x7FD8u;
    if (!ppu_native_memory_range(table_ea, 4)) return false;
    const uint32_t table = vm_read32(table_ea);
    if (!table || (table & 3u) || !ppu_native_memory_range(table, 4)) return false;
    const uint32_t queue = vm_read32(table);
    if (!queue || (queue & 127u) || !ppu_native_memory_range(queue, 128)) return false;
    *queue_ea = queue;
    return true;
}

haze_job_wait_policy::ProbeResult queue_nonempty(uint32_t queue_ea) {
    if (!queue_ea || (queue_ea & 127u) || !ppu_native_memory_range(queue_ea, 128))
        return haze_job_wait_policy::ProbeResult::invalid;

    spu_lockline_lock();
    const uint32_t head = vm_read32(queue_ea + 0x00u);
    const uint32_t tail = vm_read32(queue_ea + 0x04u);
    const uint32_t entry_size = vm_read32(queue_ea + 0x08u);
    const uint32_t depth = vm_read32(queue_ea + 0x0Cu);
    const uint64_t buffer = vm_read64(queue_ea + 0x10u);
    const uint32_t direction = vm_read32(queue_ea + 0x1Cu);

    bool valid = entry_size == 16u && depth && depth <= 0x3FFFFFFFu &&
        buffer && buffer <= UINT32_MAX && !(buffer & 15u) && direction == 1u;
    uint64_t buffer_size = 0;
    if (valid) {
        buffer_size = static_cast<uint64_t>(entry_size) * depth;
        valid = buffer_size <= UINT32_MAX && buffer + buffer_size <= 0x100000000ull &&
            ppu_native_memory_range(static_cast<uint32_t>(buffer), static_cast<uint32_t>(buffer_size));
    }

    haze_job_wait_policy::ProbeResult result = haze_job_wait_policy::ProbeResult::invalid;
    if (valid) {
        const uint32_t normalized_head = (head & 0x80000000u) ? ~head : head;
        const uint32_t normalized_tail = (tail & 0x80000000u) ? ~tail : tail;
        const uint32_t modulus = depth * 2u;
        if (normalized_head < modulus && normalized_tail < modulus) {
            const uint32_t count = normalized_tail >= normalized_head
                ? normalized_tail - normalized_head
                : modulus - normalized_head + normalized_tail;
            if (count <= depth) {
                if ((head | tail) & 0x80000000u) {
                    // Queue API may be between claiming and publishing an index.
                    result = haze_job_wait_policy::ProbeResult::waiting;
                } else {
                    result = count ? haze_job_wait_policy::ProbeResult::ready_queue
                                   : haze_job_wait_policy::ProbeResult::waiting;
                }
            }
        }
    }
    spu_lockline_unlock();
    return result;
}

struct CompletionProbe {
    uint32_t state_ea;
    bool queue_resolved = false;
    uint32_t queue_ea = 0;

    haze_job_wait_policy::ProbeResult operator()() {
        const volatile uint8_t* state_ptr = vm_base + state_ea;
        const uint8_t state = *state_ptr;
        if (state == 2) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return haze_job_wait_policy::ProbeResult::ready_primary;
        }
        if (state != 1 || state_ptr[1] != 2) return haze_job_wait_policy::ProbeResult::waiting;
        if (!queue_resolved) {
            if (!resolve_completion_queue(&queue_ea)) return haze_job_wait_policy::ProbeResult::invalid;
            queue_resolved = true;
        }
        return queue_nonempty(queue_ea);
    }
};

bool bounded_native_delay(uint32_t state_ea, haze_job_wait_policy::WaitResult* outcome) {
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&start) || start.QuadPart < 0)
        return false;
    if (static_cast<uint64_t>(frequency.QuadPart) > (UINT64_MAX - 999999u) / 330u) return false;
    const uint64_t duration = (static_cast<uint64_t>(frequency.QuadPart) * 330u + 999999u) / 1000000u;
    const auto result = haze_job_wait_policy::wait_until_ready_or_deadline(
        static_cast<uint64_t>(start.QuadPart), duration,
        CompletionProbe{state_ea},
        [] {
            LARGE_INTEGER now;
            if (!QueryPerformanceCounter(&now) || now.QuadPart < 0)
                return haze_job_wait_policy::ClockSample{false, 0};
            return haze_job_wait_policy::ClockSample{true, static_cast<uint64_t>(now.QuadPart)};
        },
        [] { SwitchToThread(); YieldProcessor(); });
    if (result == haze_job_wait_policy::WaitResult::invalid ||
        result == haze_job_wait_policy::WaitResult::clock_error)
        return false;
    *outcome = result;
    return true;
}
#endif
} // namespace

void haze_job_wait_call(ppu_context* ctx) {
    CompletionScope scope(ctx);
    // Preserve the guest duration conversion, completion predicate, and loop.
    // The scoped delay hook only replaces the exact 330 us syscall.
    func_0068EFC0(ctx);
}

bool haze_job_wait_usleep(ppu_context* ctx) {
#ifdef _WIN32
    if (!completion_delay || ctx->lr != 0x0068EFF0 ||
        ctx->gpr[11] != 0x8D || ctx->gpr[3] != 330) return false;
    const bool enabled = native_wait_enabled();
    emit_mode_once(enabled);
    if (!enabled) return false;
    if (!ppu_native_memory_available()) {
        fallback_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint32_t state_ea = 0;
    if (!resolve_slot_state(&state_ea)) {
        fallback_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    haze_job_wait_policy::WaitResult outcome = haze_job_wait_policy::WaitResult::expired;
    if (!bounded_native_delay(state_ea, &outcome)) {
        fallback_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    native_count.fetch_add(1, std::memory_order_relaxed);
    if (outcome == haze_job_wait_policy::WaitResult::ready_primary) {
        ready_count.fetch_add(1, std::memory_order_relaxed);
        ready_primary_count.fetch_add(1, std::memory_order_relaxed);
    } else if (outcome == haze_job_wait_policy::WaitResult::ready_queue) {
        ready_count.fetch_add(1, std::memory_order_relaxed);
        ready_queue_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        expired_count.fetch_add(1, std::memory_order_relaxed);
    }
    ctx->gpr[3] = 0;
    return true;
#else
    (void)ctx;
    return false;
#endif
}

void haze_job_wait_dump_counts(void) {
#ifdef _WIN32
    if (!native_wait_enabled()) return;
#else
    return;
#endif
    if (counts_dumped.test_and_set(std::memory_order_relaxed)) return;
    std::fprintf(stderr,
        "[haze-job-wait-counts] native=%llu ready=%llu ready_primary=%llu ready_queue=%llu expired=%llu fallback=%llu\n",
        static_cast<unsigned long long>(native_count.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(ready_count.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(ready_primary_count.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(ready_queue_count.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(expired_count.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(fallback_count.load(std::memory_order_relaxed)));
}
