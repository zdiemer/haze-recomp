"""Check the actual loader store macro and startup selector for inline coherency."""
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
#include <initializer_list>
#include <thread>
#define YieldProcessor() std::this_thread::yield()
#include "spu_coherency.h"
static uint8_t storage[0x11000];
uint8_t* vm_base = storage;
unsigned char spu_coh_reservation_bitmap[SPU_COH_BITMAP_SIZE];
uint32_t spu_coh_reservations_armed;
static int abi_calls, locks, notifications, lock_held;
static uint32_t notified_address;
extern "C" int spu_coh_is_reserved(uint32_t a) {
    ++abi_calls; return spu_coh_is_reserved_inline(a);
}
extern "C" void spu_lockline_lock() { assert(!lock_held); lock_held=1; ++locks; }
extern "C" void spu_lockline_unlock() { assert(lock_held); lock_held=0; }
extern "C" void spu_coh_notify_write(uint32_t a) {
    assert(lock_held); ++notifications; notified_address=a;
}
@INITIALIZATION@
@STORE_MACRO@
int main(int argc, char** argv) {
    assert(argc==3);
    const bool null_init = !strcmp(argv[1], "null-init");
    const int expected = atoi(argv[2]);
    if (null_init) vm_base=nullptr;
    ppu_native_reads_inline_init();
    assert(ppu_inline_coh_predicate_enabled==expected);
    assert(ppu_native_reads_inline_enabled==0);
    vm_base=storage;
    const uint64_t raw=0xEFCDAB8967452301ULL;
    // Exercise unarmed, armed/unreserved, then reserved paths, and each width.
    for (unsigned state=0; state<3; ++state) {
        spu_coh_atomic_store_u32(&spu_coh_reservations_armed, state!=0);
        spu_coh_atomic_store_u8(&spu_coh_reservation_bitmap[0x10080u >> 10],
            state==2 ? (1u << ((0x10080u >> 7) & 7u)) : 0u);
        for (unsigned width : {1u,2u,4u,8u}) {
            const uint64_t a = 0x123400010087ULL; // original macro truncates EA
            const uint32_t ea = static_cast<uint32_t>(a);
            memset(storage+ea-1, 0xA5, 10);
            abi_calls=locks=notifications=0;
            VM_WRITE_COH(a, &raw, width);
            assert(abi_calls==(expected ? 0 : 1));
            assert(locks==(state==2) && notifications==(state==2) && !lock_held);
            if (state==2) assert(notified_address==ea);
            assert(!memcmp(storage+ea, &raw, width));
            assert(storage[ea-1]==0xA5 && storage[ea+width]==0xA5);
        }
    }
    // Startup mode is immutable after the first initialization.
    ppu_native_reads_inline_init();
    assert(ppu_inline_coh_predicate_enabled==expected);
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
    start = source.index('#define VM_WRITE_COH(a, src, n)')
    macro = source[start:source.index('\nextern "C" {', start)]
    assert 'spu_coh_is_reserved_inline' in macro
    fixture = args.output_dir / 'coherency_predicate.cpp'
    fixture.write_text(FIXTURE.replace('@INITIALIZATION@', init).replace('@STORE_MACRO@', macro), encoding='utf-8')
    checks = 0
    for sample in (False, True):
        exe = args.output_dir / ('sample.exe' if sample else 'ordinary.exe')
        command = [args.compiler, '/nologo', '/O2', '/std:c++17', '/EHsc', '/W4',
                   '/D_CRT_SECURE_NO_WARNINGS', '/clang:-msse4.1',
                   f'/I{args.sdk / "runtime/spu"}', f'/I{args.sdk / "include"}',
                   f'/Fo{exe.with_suffix(".obj")}', f'/Fe{exe}', str(fixture)]
        if sample: command.append('/DVM_SAMPLE_READS=1')
        subprocess.run(command, check=True)
        cases = [('ordinary', 0, {}), ('ordinary', 0, {'PPU_INLINE_COH_CHECK':'OFF'}),
                 ('ordinary', 0, {'PPU_INLINE_COH_CHECK':'typo'}),
                 ('null-init', 0, {'PPU_INLINE_COH_CHECK':'ON'}),
                 ('ordinary', int(not sample), {'PPU_INLINE_COH_CHECK':'ON'})]
        cases += [('ordinary', 0, {'PPU_INLINE_COH_CHECK':'ON', name:value})
                  for name in PROBE_ENV_NAMES for value in ('1', '')]
        for case, expected, extra in cases:
            env = os.environ.copy()
            for name in (*PROBE_ENV_NAMES, 'PPU_INLINE_READS', 'PPU_INLINE_COH_CHECK'):
                env.pop(name, None)
            env.update(extra)
            run = subprocess.run([str(exe), case, str(expected)], env=env,
                                 capture_output=True, text=True, timeout=10)
            assert run.returncode==0 and 'PASS' in run.stdout, (sample, case, extra, run.stdout, run.stderr)
            checks += 1
    print(f'PASS {checks} actual loader store-macro/startup coherency cases')


if __name__ == '__main__':
    main()
