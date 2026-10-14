#include "ppu_recomp.h"
#include "ppu_native_memory.h"

#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uint8_t* vm_base;

namespace {
constexpr uint64_t kAuditedCallsite = 0x00682774ull;
constexpr uint32_t kSlotBytes = 0x1450u;
constexpr uint32_t kMaxNativeBytes = 0x100000u;

struct BulkFillConfig {
    bool enabled;
    bool profile;
};

std::atomic<uint64_t> g_eligible_attempts{0};
std::atomic<uint64_t> g_native_successes{0};
std::atomic<uint64_t> g_span_rejections{0};
std::atomic<uint64_t> g_native_bytes{0};

void dump_profile()
{
    std::fprintf(stderr,
        "[haze-bulk-fill-profile] attempts=%llu native=%llu rejected=%llu bytes=%llu\n",
        static_cast<unsigned long long>(g_eligible_attempts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_native_successes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_span_rejections.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_native_bytes.load(std::memory_order_relaxed)));
}

const BulkFillConfig& config_once()
{
    // Function-local initialization is thread-safe and immutable after the
    // first audited call. No environment lookup occurs on later fills.
    static const BulkFillConfig config = [] {
        const char* value = std::getenv("HAZE_NATIVE_BULK_FILL");
        const bool selected = value && std::strcmp(value, "ON") == 0;
        const char* profile_value = std::getenv("HAZE_BULK_FILL_PROFILE");
        const bool profile = profile_value && std::strcmp(profile_value, "1") == 0;
        std::fprintf(stderr, "[haze-bulk-fill-mode] enabled=%d\n", selected ? 1 : 0);
        if (profile) std::atexit(dump_profile);
        return BulkFillConfig{selected, profile};
    }();
    return config;
}

bool checked_loop_span(const ppu_context* ctx, uint32_t& start, uint32_t& size,
                       uint32_t& final_cursor)
{
    if (!ctx || ctx->lr != kAuditedCallsite) return false;

    // This one call site clears a newly acquired 0x1450-byte job slot with
    // zero, then copies the payload and publishes descriptor fields. The
    // narrow argument/scratch checks keep the native path tied to that shape.
    const uint64_t base64 = ctx->gpr[3];
    const uint64_t end64 = ctx->gpr[5];
    const uint64_t aligned_end64 = ctx->gpr[12];
    const uint64_t threshold64 = ctx->gpr[7];
    const uint64_t start64 = ctx->gpr[6];
    if (base64 > UINT32_MAX || end64 > UINT32_MAX || aligned_end64 > UINT32_MAX ||
        threshold64 > UINT32_MAX || start64 > UINT32_MAX || end64 < base64 ||
        end64 - base64 != kSlotBytes || ctx->gpr[4] != 0 || ctx->gpr[0] != 0 ||
        ctx->gpr[9] != 0 || ctx->gpr[11] != 0)
        return false;

    const uint32_t base = static_cast<uint32_t>(base64);
    const uint32_t end = static_cast<uint32_t>(end64);
    const uint32_t head16 = (base + 1u) & ~1u;
    const uint32_t head32 = (head16 + 2u) & ~3u;
    const uint32_t expected_start = (head32 + 4u) & ~7u;
    const uint32_t expected_aligned_end = end & ~7u;
    if (ctx->gpr[8] != head16 || ctx->gpr[10] != head32 ||
        static_cast<uint32_t>(start64) != expected_start ||
        static_cast<uint32_t>(aligned_end64) != expected_aligned_end ||
        expected_aligned_end < 64u ||
        static_cast<uint32_t>(threshold64) != expected_aligned_end - 64u ||
        expected_start > expected_aligned_end - 64u)
        return false;

    // Match the generated loop's unsigned 32-bit threshold comparisons while
    // excluding wraparound. Each iteration writes exactly one 64-byte block.
    const uint64_t blocks =
        (uint64_t(expected_aligned_end - 64u - expected_start) / 64u) + 1u;
    const uint64_t bytes = blocks * 64u;
    const uint64_t final64 = uint64_t(expected_start) + bytes;
    if (!bytes || bytes > kMaxNativeBytes || final64 > UINT32_MAX ||
        final64 > expected_aligned_end || (final64 & 7u) != 0)
        return false;

    start = expected_start;
    size = static_cast<uint32_t>(bytes);
    final_cursor = static_cast<uint32_t>(final64);
    return true;
}
} // namespace

// Called once before the generated 64-byte loop backedge. The exact LR
// whitelist plus the caller-function source guard in patch_bulk_fill.py binds
// this operation to slot initialization; other memset callers stay on the
// normal vm_write path.
bool haze_bulk_fill_try(ppu_context* ctx)
{
    if (!ctx || ctx->lr != kAuditedCallsite) return false;
    const BulkFillConfig& config = config_once();
    if (!config.enabled) return false;

    uint32_t start = 0, bytes = 0, final_cursor = 0;
    if (!checked_loop_span(ctx, start, bytes, final_cursor)) return false;
    if (config.profile) g_eligible_attempts.fetch_add(1, std::memory_order_relaxed);
    if (!vm_base ||
        !ppu_native_memory_range(static_cast<uint32_t>(ctx->gpr[3]), kSlotBytes) ||
        !ppu_native_memory_range(start, bytes)) {
        if (config.profile) g_span_rejections.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint8_t* const base = vm_base;
    if (!ppu_native_memory_begin_write(start, bytes)) {
        if (config.profile) g_span_rejections.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (vm_base != base) {
        ppu_native_memory_end();
        if (config.profile) g_span_rejections.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // The begin API owns the reservation lock. No service, allocation, wait,
    // or diagnostic is allowed until the lock is released.
    std::memset(base + start, 0, bytes);
    ppu_native_memory_end();

    if (config.profile) {
        g_native_successes.fetch_add(1, std::memory_order_relaxed);
        g_native_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    static std::atomic<bool> reported{false};
    if (!reported.load(std::memory_order_relaxed)) {
        bool expected = false;
        if (reported.compare_exchange_strong(expected, true, std::memory_order_relaxed))
            std::fprintf(stderr, "[haze-bulk-fill] native bytes=%u\n", bytes);
    }

    // The original loop exits with r8 at its final block and r6 just after it.
    // F48 immediately recomputes CR0; all other scratch state is untouched.
    ctx->gpr[8] = final_cursor - 64u;
    ctx->gpr[6] = final_cursor;
    return true;
}
