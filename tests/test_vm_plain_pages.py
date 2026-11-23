"""Check the loader's plain-RAM page table and accessor fast paths.

Extracts the real startup decision, page table and public accessors from
ppu_loader.cpp. The full accessors they fall back to are stubs that record each
call, so every case can tell the fast path from the fallback.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess

from test_memory_read_fastpaths import PROBE_ENV_NAMES, extract_section

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#define YieldProcessor() std::this_thread::yield()
#include "spu_coherency.h"
#include "spu_raw.h"
static uint8_t storage[0x50000];
uint8_t* vm_base = storage;
uint32_t ppu_vm_size = sizeof(storage);
uint32_t ppu_hle_inject_base = 0x30000u;
#define VM_HLE_INJECT_BASE ppu_hle_inject_base
uint32_t g_barrier_sync_watch, g_null_sweep_hi;
unsigned char spu_coh_reservation_bitmap[SPU_COH_BITMAP_SIZE];
uint32_t spu_coh_reservations_armed;
unsigned char vm_inline_page[0x10000];
static int ps3_log_verbose() { return 0; }
static int full_calls;
static uint64_t full_address;
static void* full_ra;
static void full(uint64_t a, void* ra) { ++full_calls; full_address = a; full_ra = ra; }
#define FULL_READ(T, n) static __attribute__((noinline)) T vm_read##n##_full(uint64_t a, void* ra) \
    { full(a, ra); return (T)0x5A; }
#define FULL_WRITE(T, n) static __attribute__((noinline)) void vm_write##n##_full(uint64_t a, T, void* ra) \
    { full(a, ra); }
FULL_READ(uint8_t, 8) FULL_READ(uint16_t, 16) FULL_READ(uint32_t, 32) FULL_READ(uint64_t, 64)
FULL_WRITE(uint8_t, 8) FULL_WRITE(uint16_t, 16) FULL_WRITE(uint32_t, 32) FULL_WRITE(uint64_t, 64)
@INITIALIZATION@
@PLAIN_PAGES@

static bool reads_fast(uint32_t ea) {
    memcpy(storage + ea, "\x01\x23\x45\x67\x89\xAB\xCD\xEF", 8);
    full_calls = 0;
    const bool b8 = vm_read8(ea) == 0x01, b16 = vm_read16(ea) == 0x0123;
    const bool b32 = vm_read32(ea) == 0x01234567u, b64 = vm_read64(ea) == 0x0123456789ABCDEFull;
    const bool fast = b8 && b16 && b32 && b64;
    assert(fast ? full_calls == 0 : full_calls == 4 && !b8 && !b16 && !b32 && !b64);
    if (!fast) assert(full_address == ea && full_ra);
    return fast;
}
static bool writes_fast(uint32_t ea) {
    memset(storage + ea - 1, 0xA5, 10);
    full_calls = 0;
    vm_write64(ea, 0x0123456789ABCDEFull);
    const bool fast = full_calls == 0;
    if (fast) {
        assert(!memcmp(storage + ea, "\x01\x23\x45\x67\x89\xAB\xCD\xEF", 8));
        vm_write32(ea, 0x10203040u); vm_write16(ea + 4, 0x5060); vm_write8(ea + 6, 0x70);
        assert(!memcmp(storage + ea, "\x10\x20\x30\x40\x50\x60\x70\xEF", 8));
        assert(storage[ea - 1] == 0xA5 && storage[ea + 8] == 0xA5 && full_calls == 0);
    } else {
        assert(full_address == ea && full_ra && storage[ea] == 0xA5);
        vm_write32(ea, 0); vm_write16(ea, 0); vm_write8(ea, 0);
        assert(full_calls == 4 && storage[ea] == 0xA5);
    }
    return fast;
}

int main(int argc, char** argv) {
    assert(argc == 3);
    const char* mode = argv[1];
    const bool enabled = atoi(argv[2]) != 0;
    // Before startup freezes the diagnostic decision every access is full.
    assert(!reads_fast(0x10000) && !writes_fast(0x10010));
    if (!strcmp(mode, "whole-space")) {
        ppu_vm_size = 0; ppu_hle_inject_base = 0x2000FFF0u;
        ppu_native_reads_inline_init(); vm_plain_pages_init();
        for (uint32_t page : {0x0u, 0x2000u, 0x2001u, 0xE000u, 0xE05Fu, 0xFFFFu})
            assert(!s_vm_plain_page[page]);
        for (uint32_t page : {0x1u, 0x1FFFu, 0x2002u, 0xDFFFu, 0xFFFEu})
            assert(s_vm_plain_page[page] == enabled);
        assert(!s_vm_plain_page[(SPU_RAW_BASE + SPU_RAW_SPAN - 1) >> 16]);
        assert(s_vm_plain_page[(SPU_RAW_BASE + SPU_RAW_SPAN) >> 16] == enabled);
        // With nothing reserved or watched, the inline pages are the plain ones.
        for (uint32_t page = 0; page < 0x10000u; ++page)
            assert(vm_inline_page[page] == s_vm_plain_page[page]);
        puts("PASS"); return 0;
    }
    if (!strcmp(mode, "marked")) {
        // A line reserved before startup keeps its page off the inline path only.
        spu_coh_reservation_bitmap[(2u << 6) + 63] = 0x80;
        ppu_native_reads_inline_init(); vm_plain_pages_init();
        assert(s_vm_plain_page[1] && s_vm_plain_page[2]);
        assert(vm_inline_page[1] && !vm_inline_page[2]);
        puts("PASS"); return 0;
    }
    if (!strcmp(mode, "atom-watch")) {
        // SPU_ATOM_EA arms a write watch later, so no page is inline.
        ppu_native_reads_inline_init(); vm_plain_pages_init();
        assert(s_vm_plain_page[1] && s_vm_plain_page[2]);
        for (uint32_t page = 0; page < 0x10000u; ++page) assert(!vm_inline_page[page]);
        puts("PASS"); return 0;
    }
    if (!strcmp(mode, "null-init")) vm_base = nullptr;
    ppu_native_reads_inline_init(); vm_plain_pages_init();
    vm_base = storage;
    // Pages 1 and 2 are ordinary; 0 is the null page, 3 the GCM window, and
    // 4 ends at the bounded guest size.
    for (uint32_t page = 0; page < 5u; ++page) assert(vm_inline_page[page] == s_vm_plain_page[page]);
    assert(reads_fast(0x10000) == enabled && reads_fast(0x2FFF0) == enabled);
    assert(writes_fast(0x10010) == enabled && writes_fast(0x2FFF0) == enabled);
    for (uint32_t ea : {0x100u, 0x30004u, 0x40000u}) assert(!reads_fast(ea) && !writes_fast(ea));
    if (!enabled) { puts("PASS"); return 0; }
    // Write watches that arm while the game runs send stores to the full path.
    g_barrier_sync_watch = 0x10000;
    assert(!writes_fast(0x10010) && reads_fast(0x10010));
    g_barrier_sync_watch = 0; g_null_sweep_hi = 1;
    assert(!writes_fast(0x10010) && reads_fast(0x10010));
    g_null_sweep_hi = 0;
    // A reserved line takes the locked store; only an armed bitmap counts.
    spu_coh_atomic_store_u8(&spu_coh_reservation_bitmap[0x10080u >> 10],
        1u << ((0x10080u >> 7) & 7u));
    assert(writes_fast(0x10080));
    spu_coh_atomic_store_u32(&spu_coh_reservations_armed, 1);
    assert(!writes_fast(0x10080) && !writes_fast(0x100FF) && reads_fast(0x10080));
    assert(writes_fast(0x10100) && writes_fast(0x10070));
    puts("PASS");
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--sdk', type=Path, default=ROOT / 'external/ps3recomp')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = (args.sdk / 'runtime/ppu/ppu_loader.cpp').read_text(encoding='utf-8')
    init = extract_section(source, 'static inline bool vm_memory_probes(void)', 'static void vm_hotmap(')
    pages = extract_section(source, '/* Ordinary-RAM pages.', '\n}\n\nextern "C" int ppu_native_memory_available')
    assert 'ppu_native_reads_inline_init();\n    vm_plain_pages_init();' in source
    fixture = args.output_dir / 'vm_plain_pages.cpp'
    fixture.write_text(FIXTURE.replace('@INITIALIZATION@', init).replace('@PLAIN_PAGES@', pages),
                       encoding='utf-8')
    exe = args.output_dir / 'vm_plain_pages.exe'
    subprocess.run([args.compiler, '/nologo', '/O2', '/std:c++17', '/EHsc', '/W4', '/wd4100',
                    '/D_CRT_SECURE_NO_WARNINGS', '/clang:-msse4.1',
                    f'/I{args.sdk / "runtime/spu"}', f'/I{args.sdk / "include"}',
                    f'/Fo{exe.with_suffix(".obj")}', f'/Fe{exe}', str(fixture)], check=True)
    cases = [('ordinary', 1, {}), ('whole-space', 1, {}), ('null-init', 0, {}),
             ('ordinary', 0, {'PPU_INLINE_COH_CHECK': 'OFF'}),
             ('whole-space', 0, {'PPU_INLINE_COH_CHECK': 'OFF'}),
             ('ordinary', 1, {'PPU_INLINE_READS': 'OFF'}),
             ('marked', 1, {}), ('atom-watch', 1, {'SPU_ATOM_EA': '40000'})]
    cases += [('ordinary', 0, {name: '1'}) for name in PROBE_ENV_NAMES]
    checks = 0
    for mode, enabled, extra in cases:
        env = os.environ.copy()
        for name in (*PROBE_ENV_NAMES, 'PPU_INLINE_READS', 'PPU_INLINE_COH_CHECK', 'SPU_ATOM_EA'):
            env.pop(name, None)
        env.update(extra)
        run = subprocess.run([str(exe), mode, str(enabled)], env=env,
                             capture_output=True, text=True, timeout=10)
        assert run.returncode == 0 and 'PASS' in run.stdout, (mode, extra, run.stdout, run.stderr)
        checks += 1
    print(f'PASS {checks} plain/inline page table, fast-path and fallback cases')


if __name__ == '__main__':
    main()
