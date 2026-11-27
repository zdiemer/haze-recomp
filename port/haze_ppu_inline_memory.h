#pragma once

// Forced into generated PPU translation units only. Parse the original ABI
// before remapping calls, so special accesses still reach the runtime
// accessors. An access inlines the accessors' own fast path: one load from the
// runtime's ordinary-RAM page table, which stays empty until ppu_run has
// frozen every diagnostic decision. Anything else calls the original ABI.
#include "ppu_recomp.h"
#include "ppu_inline_reads.h"
#include "spu_coherency.h"

extern "C" {
extern uint32_t g_barrier_sync_watch;
extern uint32_t g_null_sweep_hi;
void vm_write8(uint64_t, uint8_t);
void vm_write16(uint64_t, uint16_t);
void vm_write32(uint64_t, uint32_t);
void vm_write64(uint64_t, uint64_t);
}

#if defined(_MSC_VER)
#define HAZE_WRITE_INLINE __forceinline
#else
#define HAZE_WRITE_INLINE inline __attribute__((always_inline))
#endif

// The out-of-line store's test: an ordinary-RAM page, no write watch armed and
// no SPU reservation anywhere in the page. A reserved page's other lines take
// the ABI, which checks the line under the lock-line protocol.
static HAZE_WRITE_INLINE bool haze_inline_write_eligible(uint32_t ea) {
    const uint32_t page = ea >> 16;
    return __builtin_expect(ppu_vm_plain_pages[page] &&
        !(g_barrier_sync_watch | g_null_sweep_hi) &&
        !spu_coh_atomic_load_u8(&spu_coh_page_marked[ea >> SPU_COH_PAGE_SHIFT]), 1);
}

// Packed, may-alias scalar wrappers retain unaligned accesses. Volatile keeps
// each guest store and its ordering with the inline guest loads/stores. Guest
// fences and publication/service calls remain in the unchanged lifted code.
static HAZE_WRITE_INLINE void haze_inline_vm_write8(uint64_t address, uint8_t value) {
    const uint32_t ea = static_cast<uint32_t>(address);
    if (haze_inline_write_eligible(ea)) {
        *reinterpret_cast<volatile uint8_t*>(vm_base + ea) = value;
    } else vm_write8(address, value);
}
static HAZE_WRITE_INLINE void haze_inline_vm_write16(uint64_t address, uint16_t value) {
    const uint32_t ea = static_cast<uint32_t>(address);
    if (haze_inline_write_eligible(ea)) {
        reinterpret_cast<volatile ppu_inline_read_u16*>(vm_base + ea)->value = __builtin_bswap16(value);
    } else vm_write16(address, value);
}
static HAZE_WRITE_INLINE void haze_inline_vm_write32(uint64_t address, uint32_t value) {
    const uint32_t ea = static_cast<uint32_t>(address);
    if (haze_inline_write_eligible(ea)) {
        reinterpret_cast<volatile ppu_inline_read_u32*>(vm_base + ea)->value = __builtin_bswap32(value);
    } else vm_write32(address, value);
}
static HAZE_WRITE_INLINE void haze_inline_vm_write64(uint64_t address, uint64_t value) {
    const uint32_t ea = static_cast<uint32_t>(address);
    if (haze_inline_write_eligible(ea)) {
        reinterpret_cast<volatile ppu_inline_read_u64*>(vm_base + ea)->value = __builtin_bswap64(value);
    } else vm_write64(address, value);
}
#undef HAZE_WRITE_INLINE

#define vm_read8 ppu_inline_vm_read8
#define vm_read16 ppu_inline_vm_read16
#define vm_read32 ppu_inline_vm_read32
#define vm_read64 ppu_inline_vm_read64
#define vm_write8 haze_inline_vm_write8
#define vm_write16 haze_inline_vm_write16
#define vm_write32 haze_inline_vm_write32
#define vm_write64 haze_inline_vm_write64
