#include "ppu_recomp.h"
#include "ppu_native_memory.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uint8_t* vm_base;
extern "C" uint32_t g_null_sweep_hi;

namespace {
constexpr uint64_t kAuditedCallsite = 0x00682798ull;
constexpr uint32_t kSlotBytes = 0x1450u;
constexpr uint32_t kCopyOffset = 0x10u;
constexpr uint32_t kCopyBytes = 0x1410u;
std::atomic<bool> g_selector_seen{false};
std::atomic<bool> g_selector_enabled{false};
std::atomic<bool> g_summary_dumped{false};
std::atomic<uint64_t> g_copy_attempts{0};
std::atomic<uint64_t> g_copy_native{0};
std::atomic<uint64_t> g_copy_rejected{0};
std::atomic<uint64_t> g_copy_bytes{0};

bool enabled_once()
{
    static const bool enabled = [] {
        const char* value = std::getenv("HAZE_NATIVE_SLOT_COPY");
        const bool selected = !value || !*value || std::strcmp(value, "ON") == 0;
        g_selector_enabled.store(selected, std::memory_order_relaxed);
        g_selector_seen.store(true, std::memory_order_release);
        return selected;
    }();
    return enabled;
}

bool diagnostic_writes_disabled()
{
    // vm_write8 has a Windows PT truncation detector even with ordinary
    // memory probes off. Null-sweep mode also gives valid RAM writes extra
    // semantics after it observes a low-address store. Both environment
    // controls are process-start configuration; still check the live sweep
    // state below in case another guest thread has already entered that mode.
    static const bool disabled = [] {
        if (std::getenv("PS3_NULL_SWEEP")) return true;
#ifdef _WIN32
        if (std::getenv("PT")) return true;
#endif
        return false;
    }();
    return disabled || g_null_sweep_hi != 0;
}

bool exact_shape(const ppu_context* ctx, uint32_t* source, uint32_t* destination)
{
    if (!ctx || ctx->lr != kAuditedCallsite ||
        ctx->gpr[3] > UINT32_MAX || ctx->gpr[4] > UINT32_MAX ||
        ctx->gpr[5] != kCopyBytes || ctx->gpr[31] > UINT32_MAX)
        return false;

    const uint32_t slot = static_cast<uint32_t>(ctx->gpr[31]);
    const uint32_t dst = static_cast<uint32_t>(ctx->gpr[3]);
    if (slot > UINT32_MAX - kSlotBytes || slot > UINT32_MAX - kCopyOffset ||
        dst != slot + kCopyOffset)
        return false;

    *source = static_cast<uint32_t>(ctx->gpr[4]);
    *destination = dst;
    return true;
}
} // namespace

// Native replacement for the one 0x1410-byte memmove in the audited
// job-slot acquisition path. It deliberately keeps the preceding slot memset
// and following metadata writes as generated guest operations. The source and
// destination may overlap: std::memmove preserves the original helper's byte
// semantics. False leaves ctx and guest RAM untouched so generated code runs
// the original memmove implementation.
bool haze_slot_copy_try(ppu_context* ctx)
{
    uint32_t source = 0, destination = 0;
    if (!exact_shape(ctx, &source, &destination) || !enabled_once()) return false;
    g_copy_attempts.fetch_add(1, std::memory_order_relaxed);
    if (diagnostic_writes_disabled()) {
        g_copy_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uint32_t slot = static_cast<uint32_t>(ctx->gpr[31]);
    if (!vm_base ||
        !ppu_native_memory_range(slot, kSlotBytes) ||
        !ppu_native_memory_range(source, kCopyBytes) ||
        !ppu_native_memory_range(destination, kCopyBytes)) {
        g_copy_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint8_t* const base = vm_base;
    if (!ppu_native_memory_begin_write(destination, kCopyBytes)) {
        g_copy_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (vm_base != base) {
        ppu_native_memory_end();
        g_copy_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // The begin-write API holds the reservation lock. Do not call guest code,
    // allocate, wait, or log until after releasing it.
    std::memmove(base + destination, base + source, kCopyBytes);
    ppu_native_memory_end();

    // The caller's next instructions replace its volatile argument registers;
    // r3 is the only return value required from the memmove call. Callee-saved
    // registers and LR remain untouched as required by the PPU ABI.
    ctx->gpr[3] = destination;
    g_copy_native.fetch_add(1, std::memory_order_relaxed);
    g_copy_bytes.fetch_add(kCopyBytes, std::memory_order_relaxed);

    return true;
}

// Call once after guest producer threads have stopped. The atomic snapshot is
// safe even if teardown races a late caller, but callers should place this at
// normal process shutdown for complete totals. Nothing is written per copy.
extern "C" void haze_slot_copy_dump_stats(void)
{
    if (!g_selector_seen.load(std::memory_order_acquire) ||
        !g_selector_enabled.load(std::memory_order_relaxed)) return;
    if (g_summary_dumped.exchange(true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
        "[haze-slot-copy-summary] attempts=%llu native=%llu rejected=%llu bytes=%llu\n",
        static_cast<unsigned long long>(g_copy_attempts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_copy_native.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_copy_rejected.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_copy_bytes.load(std::memory_order_relaxed)));
}
