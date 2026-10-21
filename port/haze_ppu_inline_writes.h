#pragma once

// Developer-only force include for the two measured PPU modules. Parse the
// guest ABI before remapping writes; fallback must call the original ABI.
#include "ppu_recomp.h"
#include "ppu_inline_reads.h"
#include "ppu_native_memory.h"
#include "spu_coherency.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
extern uint32_t g_barrier_sync_watch;
extern uint32_t g_null_sweep_hi;
void vm_write8(uint64_t, uint8_t);
void vm_write16(uint64_t, uint16_t);
void vm_write32(uint64_t, uint32_t);
void vm_write64(uint64_t, uint64_t);
}

// Initialization runs before guest threads. The runtime read eligibility flag
// remains false until ppu_run checks every memory diagnostic. Reuse that
// conservative gate: PPU_INLINE_READS=OFF also disables this experiment.
inline const bool haze_inline_writes_requested = [] {
    const char* value = std::getenv("HAZE_INLINE_WRITES");
    const char* coh = std::getenv("PPU_INLINE_COH_CHECK");
    const bool enabled = value && std::strcmp(value, "1") == 0 &&
        (!coh || !*coh || std::strcmp(coh, "ON") == 0);
    std::fprintf(stderr, "[haze-inline-writes] requested=%u\n", enabled ? 1u : 0u);
    return enabled;
}();

#if defined(_MSC_VER)
#define HAZE_WRITE_INLINE __forceinline
#else
#define HAZE_WRITE_INLINE inline __attribute__((always_inline))
#endif

static HAZE_WRITE_INLINE bool haze_inline_write_eligible(uint32_t ea, uint32_t width) {
    if (!haze_inline_writes_requested || !ppu_native_reads_inline_enabled ||
        !vm_base || g_barrier_sync_watch || g_null_sweep_hi ||
        !ppu_native_ram_range(ea, width, ppu_vm_size, ppu_hle_inject_base))
        return false;
    // Keep crossing stores on the original ABI, including its exact handling
    // of reservations. Ordinary writes use the same acquire bitmap predicate
    // as VM_WRITE_COH; any marked line falls back to its locked store/notify.
    if ((ea & 127u) + width > 128u) return false;
    return !spu_coh_is_reserved_inline(ea);
}

// Packed, may-alias scalar wrappers retain unaligned accesses. Volatile keeps
// each guest store and its ordering with the inline guest loads/stores. Guest
// fences and publication/service calls remain in the unchanged lifted code.
static HAZE_WRITE_INLINE void haze_inline_vm_write8(uint64_t address, uint8_t value) {
    const uint32_t ea = static_cast<uint32_t>(address);
    if (haze_inline_write_eligible(ea, 1)) {
        *reinterpret_cast<volatile uint8_t*>(vm_base + ea) = value;
    } else vm_write8(address, value);
}
static HAZE_WRITE_INLINE void haze_inline_vm_write16(uint64_t address, uint16_t value) {
    const uint32_t ea = static_cast<uint32_t>(address);
    if (haze_inline_write_eligible(ea, 2)) {
        reinterpret_cast<volatile ppu_inline_read_u16*>(vm_base + ea)->value = __builtin_bswap16(value);
    } else vm_write16(address, value);
}
static HAZE_WRITE_INLINE void haze_inline_vm_write32(uint64_t address, uint32_t value) {
    const uint32_t ea = static_cast<uint32_t>(address);
    if (haze_inline_write_eligible(ea, 4)) {
        reinterpret_cast<volatile ppu_inline_read_u32*>(vm_base + ea)->value = __builtin_bswap32(value);
    } else vm_write32(address, value);
}
static HAZE_WRITE_INLINE void haze_inline_vm_write64(uint64_t address, uint64_t value) {
    const uint32_t ea = static_cast<uint32_t>(address);
    if (haze_inline_write_eligible(ea, 8)) {
        reinterpret_cast<volatile ppu_inline_read_u64*>(vm_base + ea)->value = __builtin_bswap64(value);
    } else vm_write64(address, value);
}

#undef HAZE_WRITE_INLINE
#define vm_write8 haze_inline_vm_write8
#define vm_write16 haze_inline_vm_write16
#define vm_write32 haze_inline_vm_write32
#define vm_write64 haze_inline_vm_write64
