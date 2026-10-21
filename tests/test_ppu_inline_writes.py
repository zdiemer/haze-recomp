"""Exercise the real opt-in write shim with runtime eligibility/coherency code."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
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
#include "ppu_recomp.h"
#include "spu_coherency.h"
alignas(128) static uint8_t storage[0x12000];
extern "C" {
uint8_t* vm_base = storage;
uint32_t ppu_vm_size = sizeof(storage), ppu_hle_inject_base = 0x03000000;
uint32_t g_barrier_sync_watch = 0, g_null_sweep_hi = 0;
unsigned char spu_coh_reservation_bitmap[SPU_COH_BITMAP_SIZE] = {};
uint32_t spu_coh_reservations_armed = 0;
}
static unsigned calls, locks, notifications, last_width;
static bool held;
static uint64_t last_address, last_value;
extern "C" int spu_coh_is_reserved(uint32_t a) { return spu_coh_is_reserved_inline(a); }
extern "C" void spu_lockline_lock() { assert(!held); held=true; ++locks; }
extern "C" void spu_lockline_unlock() { assert(held); held=false; }
extern "C" void spu_coh_notify_write(uint32_t a) { assert(held); ++notifications; }
@INITIALIZATION@
@STORE_MACRO@

// The original accessor ABI is observable. Its store uses the real runtime
// coherency macro; special-address stubs record calls without touching MMIO.
static void fallback(uint64_t address, uint64_t value, unsigned width) {
    ++calls; last_address=address; last_value=value; last_width=width;
    uint32_t ea=uint32_t(address);
    if (ea>=0x10000 && uint64_t(ea)+width<=sizeof(storage)) {
        uint8_t bytes[8];
        for(unsigned i=0;i<width;++i) bytes[i]=uint8_t(value>>((width-i-1)*8));
        VM_WRITE_COH(address, bytes, width);
    }
}
extern "C" __declspec(noinline) void vm_write8(uint64_t a,uint8_t v) { fallback(a,v,1); }
extern "C" __declspec(noinline) void vm_write16(uint64_t a,uint16_t v) { fallback(a,v,2); }
extern "C" __declspec(noinline) void vm_write32(uint64_t a,uint32_t v) { fallback(a,v,4); }
extern "C" __declspec(noinline) void vm_write64(uint64_t a,uint64_t v) { fallback(a,v,8); }
#include "haze_ppu_inline_writes.h"

static void store(unsigned width,uint64_t a,uint64_t v) {
    switch(width) {
    case 1: vm_write8(a,uint8_t(v)); break;
    case 2: vm_write16(a,uint16_t(v)); break;
    case 4: vm_write32(a,uint32_t(v)); break;
    case 8: vm_write64(a,v); break;
    default: assert(false);
    }
}
static void check(unsigned width,uint64_t address,bool direct,bool reserved=false) {
    memset(storage,0xA5,sizeof(storage));
    calls=locks=notifications=0; held=false;
    const uint64_t value=0xFEDCBA9876543210ull;
    store(width,address,value);
    assert(calls==unsigned(!direct));
    if (!direct) {
        assert(last_address==address && last_width==width);
        const uint64_t mask=width==8 ? UINT64_MAX : ((1ull<<(width*8))-1);
        assert(last_value==(value&mask));
    }
    const uint32_t ea=uint32_t(address);
    if(ea>=0x10000 && uint64_t(ea)+width<=sizeof(storage)) {
        for(unsigned i=0;i<width;++i) assert(storage[ea+i]==uint8_t(value>>((width-i-1)*8)));
        assert(storage[ea-1]==0xA5);
        if(ea+width<sizeof(storage)) assert(storage[ea+width]==0xA5);
    }
    assert(!held && locks==unsigned(reserved) && notifications==unsigned(reserved));
}
extern "C" __declspec(noinline) void inline_write_codegen_probe(uint64_t a,uint32_t v) {
    vm_write32(a,v);
}
int main(int argc,char** argv) {
    assert(argc==2); const bool enabled=atoi(argv[1])!=0;
    // Before the runtime publishes eligibility, every write stays on the ABI.
    for(unsigned width:{1u,2u,4u,8u}) check(width,0x10010,false);
    ppu_native_reads_inline_init();
    for(unsigned width:{1u,2u,4u,8u}) {
        for(unsigned alignment=0;alignment<8;++alignment)
            check(width,0x123400010010ull+alignment,enabled);
        check(width,0xFFFF,false); check(width,0,false);
        check(width,0xC0000000,false); check(width,0xE0000000,false);
        check(width,0xFFFFfffE,false); check(width,sizeof(storage),false);
        check(width,sizeof(storage)-width,enabled);
        if(width>1) { check(width,sizeof(storage)-width+1,false); check(width,0x1007F,false); }
        ppu_hle_inject_base=0x10000;
        check(width,0x10010,false);
        ppu_hle_inject_base=0x10020;
        check(width,0x10020-width,enabled);
        if(width>1) check(width,0x10021-width,false);
        ppu_hle_inject_base=0x03000000;
        g_barrier_sync_watch=0x10000; check(width,0x10010,false); g_barrier_sync_watch=0;
        g_null_sweep_hi=1; check(width,0x10010,false); g_null_sweep_hi=0;
        vm_base=nullptr; assert(!haze_inline_write_eligible(0x10010,width)); vm_base=storage;
        // An armed bitmap with no mark is still eligible. A reserved line
        // must call the accessor, acquire the lock and notify exactly once.
        spu_coh_atomic_store_u32(&spu_coh_reservations_armed,1);
        check(width,0x10010,enabled);
        spu_coh_atomic_store_u8(&spu_coh_reservation_bitmap[0x10000>>10],1u<<((0x10000>>7)&7));
        check(width,0x10010,false,true);
        if(width>1) check(width,0x1007F,false,true);
        spu_coh_atomic_store_u8(&spu_coh_reservation_bitmap[0x10000>>10],0);
        spu_coh_atomic_store_u32(&spu_coh_reservations_armed,0);
        ppu_vm_size=0; check(width,0x10010,enabled); ppu_vm_size=sizeof(storage);
    }
    puts("PASS");
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--sdk', type=Path, default=ROOT/'external/ps3recomp')
    parser.add_argument('--recomp', type=Path, default=ROOT/'tests/fixtures/job_wait')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = (args.sdk/'runtime/ppu/ppu_loader.cpp').read_text(encoding='utf-8')
    init = extract_section(source, 'static inline bool vm_memory_probes(void)', 'static void vm_hotmap(')
    start = source.index('#define VM_WRITE_COH(a, src, n)')
    macro = source[start:source.index('\nextern "C" {', start)]
    fixture = args.output_dir/'inline_writes.cpp'
    fixture.write_text(FIXTURE.replace('@INITIALIZATION@', init).replace('@STORE_MACRO@', macro), encoding='utf-8')
    count = 0
    for sample in (False, True):
        exe = args.output_dir/('sample.exe' if sample else 'ordinary.exe')
        command = [args.compiler, '/nologo', '/O2', '/std:c++17', '/EHsc', '/W0',
                   '/clang:-msse4.1', '/clang:-fno-strict-aliasing', '/D_CRT_SECURE_NO_WARNINGS',
                   f'/Fo{exe.with_suffix(".obj")}', f'/Fe{exe}', '/FA', f'/Fa{exe.with_suffix(".asm")}',
                   *(f'/I{p}' for p in (ROOT/'port', args.recomp, args.sdk/'runtime/ppu', args.sdk/'runtime/spu', args.sdk/'include')),
                   str(fixture)]
        if sample: command.append('/DVM_SAMPLE_READS=1')
        subprocess.run(command, check=True)
        assembly = exe.with_suffix('.asm').read_text()
        probe = re.search(r'inline_write_codegen_probe:.*?# -- End function', assembly, re.S)
        assert probe and 'bswap' in probe[0] and 'vm_write32' in probe[0], 'missing direct store or ABI fallback'
        assert not re.search(r'(?:call|jmp)\s+(?:haze_inline|spu_coh_is_reserved)', probe[0]), 'hot checks did not inline'
        cases = [(False, {}), (False, {'HAZE_INLINE_WRITES':''}),
                 (False, {'HAZE_INLINE_WRITES':'0'}), (False, {'HAZE_INLINE_WRITES':'typo'}),
                 (not sample, {'HAZE_INLINE_WRITES':'1'}),
                 (False, {'HAZE_INLINE_WRITES':'1','PPU_INLINE_READS':'OFF'}),
                 (False, {'HAZE_INLINE_WRITES':'1','PPU_INLINE_COH_CHECK':'OFF'})]
        cases += [(False, {'HAZE_INLINE_WRITES':'1', name:value})
                  for name in PROBE_ENV_NAMES for value in ('1','')]
        for expected, extra in cases:
            env = os.environ.copy()
            for name in (*PROBE_ENV_NAMES,'HAZE_INLINE_WRITES','PPU_INLINE_READS','PPU_INLINE_COH_CHECK'):
                env.pop(name,None)
            env.update(extra)
            result = subprocess.run([str(exe),str(int(expected))],env=env,capture_output=True,text=True,timeout=15)
            assert result.returncode==0 and 'PASS' in result.stdout,(sample,extra,result.stdout,result.stderr)
            count += 1
    print(f'PASS {count} write eligibility, byte order, bounds, diagnostic, and reservation cases')


if __name__ == '__main__':
    main()
