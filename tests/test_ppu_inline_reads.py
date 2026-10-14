"""Compile the real generated-read shim and runtime startup decision.

Original-accessor stubs make fallback observable without touching device or
unmapped addresses. Runtime diagnostic eligibility is extracted, not copied.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess

from test_memory_read_fastpaths import PROBE_ENV_NAMES, extract_section

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <thread>
#include <chrono>
#define YieldProcessor() std::this_thread::yield()
#include "ppu_recomp.h"
alignas(64) static uint8_t storage[0x11000];
extern "C" {
uint8_t* vm_base = storage;
uint32_t ppu_vm_size = sizeof(storage);
uint32_t ppu_hle_inject_base = 0x03000000;
}
@INITIALIZATION@
static int calls;
static uint64_t last_address;
static unsigned last_width;
static ppu_context* fallback_context;
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
#define FALLBACK(bits, type) \
extern "C" NOINLINE type vm_read##bits(uint64_t address) { \
    ++calls; last_address=address; last_width=bits/8; \
    if (bits==32 && fallback_context) fallback_context->gpr[7]=0x87654321; \
    return (type)0xA5; }
FALLBACK(8, uint8_t)
FALLBACK(16, uint16_t)
FALLBACK(32, uint32_t)
FALLBACK(64, uint64_t)
extern "C" void vm_write32(uint64_t address, uint32_t value) {
    ++calls; last_address=address; last_width=value;
}
#include "haze_ppu_inline_reads.h"
static_assert(sizeof(ppu_inline_read_u16)==2 && alignof(ppu_inline_read_u16)==1);
static_assert(sizeof(ppu_inline_read_u32)==4 && alignof(ppu_inline_read_u32)==1);
static_assert(sizeof(ppu_inline_read_u64)==8 && alignof(ppu_inline_read_u64)==1);

extern "C" NOINLINE uint32_t inline_codegen_probe(uint64_t address) {
    return vm_read32(address) + 1;
}

extern "C" NOINLINE uint32_t inline_poll_probe(uint64_t address) {
    uint32_t value;
    do { value=vm_read32(address); } while (!value);
    return value;
}
#define POLL(bits, type) \
extern "C" NOINLINE type inline_poll##bits##_probe(uint64_t address) { \
    type value; do { value=vm_read##bits(address); } while (!value); return value; }
POLL(8, uint8_t)
POLL(16, uint16_t)
POLL(64, uint64_t)

extern "C" NOINLINE uint64_t inline_context_probe(ppu_context* ctx, uint64_t address) {
    ctx->gpr[7] = 0x12345678;
    uint64_t loaded = vm_read32(address);
    return ctx->gpr[7] + loaded;
}

static uint64_t read_width(unsigned width, uint64_t address) {
    switch (width) {
    case 1: return vm_read8(address);
    case 2: return vm_read16(address);
    case 4: return vm_read32(address);
    case 8: return vm_read64(address);
    }
    abort();
}
static void fallback(unsigned width, uint64_t address) {
    int before=calls;
    assert(read_width(width,address)==0xA5);
    assert(calls==before+1 && last_address==address && last_width==width);
}
static void direct(unsigned width, uint32_t ea, uint64_t high=0) {
    uint64_t expected=0;
    for (unsigned i=0; i<width; ++i) {
        storage[ea+i]=(uint8_t)(0x81+i*13);
        expected=(expected<<8)|storage[ea+i];
    }
    int before=calls;
    assert(read_width(width,high|ea)==expected);
    assert(calls==before);
}
int main(int argc, char** argv) {
    assert(argc==2);
    // No generated caller may inline reads before the runtime's eager init.
    assert(ppu_native_reads_inline_enabled==0);
    fallback(4,0x10001);
    if (!strcmp(argv[1],"null-init")) vm_base=NULL;
    ppu_native_reads_inline_init();
    int enabled=!strcmp(argv[1],"enabled");
    assert(ppu_native_reads_inline_enabled==enabled);
    vm_base=storage;
    for (unsigned width: {1u,2u,4u,8u}) {
        if (!enabled) { fallback(width,0x10001); continue; }
        direct(width,0x10000);
        direct(width,0x10001); // unaligned, explicit big-endian oracle
        direct(width,0x10003,0xFEDCBA9800000000ull); // legacy EA truncation
        direct(width,sizeof(storage)-width); // last complete valid value
        fallback(width,sizeof(storage)-width+1);
        fallback(width,0xFFFF); // low-address diagnostics even across boundary
        fallback(width,0xE0000000ull); // Raw SPU/device range
        fallback(width,0xFFFFFFFFull); // overflow-safe end check
        fallback(width,0xABCDEF00FFFFFFFFull); // full input forwarded
        ppu_vm_size=0;
        direct(width,0x10001);
        // Range predicate tests avoid reserving gigabytes in this fixture.
        assert(ppu_native_read_inline_range(0xE0000000u-width,width));
        assert(!ppu_native_read_inline_range(0xE0000000u-width+1,width));
        fallback(width,0xE0000000u);
        ppu_vm_size=sizeof(storage);
        vm_base=NULL;
        fallback(width,0x10001);
        vm_base=storage;
    }
    if (enabled) {
        ppu_context context={};
        direct(4,0x10001);
        assert(inline_context_probe(&context,0x10001)==0x12345678ull+0x818E9BA8ull);
        assert(context.gpr[7]==0x12345678);
        fallback_context=&context;
        assert(inline_context_probe(&context,0xFFFF)==0x87654321ull+0xA5);
        fallback_context=nullptr;
        // A publisher changes guest RAM after polling begins. The compiler
        // must retain the polling load even though no guest fence is present.
        uint64_t* shared=reinterpret_cast<uint64_t*>(storage+0x10040);
        for (unsigned width: {1u,2u,4u,8u}) {
            __atomic_store_n(shared,0ull,__ATOMIC_SEQ_CST);
            std::thread publisher([shared] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                __atomic_store_n(shared,0x80ull,__ATOMIC_SEQ_CST);
            });
            uint64_t observed=0;
            switch (width) {
            case 1: observed=inline_poll8_probe(0x10040); break;
            case 2: observed=inline_poll16_probe(0x10040); break;
            case 4: observed=inline_poll_probe(0x10040); break;
            case 8: observed=inline_poll64_probe(0x10040); break;
            }
            publisher.join();
            assert(observed==(0x80ull<<(8*(width-1))));
        }
        // Relocatable HLE base: ref polling must reach the original handler.
        ppu_hle_inject_base=0xE000;
        fallback(4,0x10008);
        direct(1,0x10008); direct(2,0x10008); direct(8,0x10008);
        ppu_hle_inject_base=0x03000000;
        ppu_vm_size=0;
        fallback(4,0x03002008);
    }
    // Writes must remain on the existing reservation/coherency-aware ABI.
    int before=calls;
    vm_write32(0x123456789ull,71);
    assert(calls==before+1 && last_address==0x123456789ull && last_width==71);
    ppu_native_reads_inline_init();
    assert(ppu_native_reads_inline_enabled==enabled);
    puts("PASS");
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp")
    parser.add_argument("--recomp", type=Path, default=ROOT / "generated/ppu")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = (args.sdk / "runtime/ppu/ppu_loader.cpp").read_text(encoding="utf-8")
    init = extract_section(source, "static inline bool vm_memory_probes(void)",
                           "static void vm_hotmap(")
    assert "ppu_native_reads_inline_init" in init
    fixture = args.output_dir / "inline_reads.cpp"
    fixture.write_text("#include <initializer_list>\n" + FIXTURE.replace("@INITIALIZATION@", init), encoding="utf-8")
    msvc = Path(args.compiler).name.lower() in ("clang-cl", "clang-cl.exe")
    checks = 0
    for sample in (False, True):
        exe = args.output_dir / ("inline_sample.exe" if sample else "inline_reads.exe")
        assembly = exe.with_suffix(".asm")
        includes = [ROOT / "port", args.sdk / "runtime/ppu", args.recomp]
        if msvc:
            command = [args.compiler, "/nologo", "/O2", "/std:c++17", "/EHsc",
                       "/W4", "/D_CRT_SECURE_NO_WARNINGS", "/clang:-fno-strict-aliasing", "/FA", f"/Fa{assembly}",
                       f"/Fo{exe.with_suffix('.obj')}", f"/Fe{exe}"]
            command += [f"/I{p}" for p in includes]
            if sample: command.append("/DVM_SAMPLE_READS=1")
            command.append(str(fixture))
        else:
            command = [args.compiler, "-O2", "-std=c++17", "-fno-strict-aliasing", "-o", str(exe)]
            command += [f"-I{p}" for p in includes]
            if sample: command.append("-DVM_SAMPLE_READS=1")
            command.append(str(fixture))
        subprocess.run(command, check=True)
        if msvc:
            asm = assembly.read_text()
            probe = re.search(r"inline_codegen_probe:.*?(?:# -- End function|ENDP)", asm, re.S)
            assert probe, "missing representative generated caller in assembly"
            assert "bswap" in probe[0] and "vm_read32" in probe[0], probe[0]
            assert not re.search(r"(?:call|jmp)\s+ppu_inline", probe[0]), probe[0]
            for bits in ('', '8', '16', '64'):
                poll = re.search(r"inline_poll" + bits + r"_probe:.*?(?:# -- End function|ENDP)", asm, re.S)
                assert poll and "Inner Loop Header" in poll[0], "polling load optimized away"
                assert not re.search(r"\b[mls]fence\b", poll[0]), "unexpected hardware fence"
            context = re.search(r"inline_context_probe:.*?(?:# -- End function|ENDP)", asm, re.S)
            assert context and 'bswap' in context[0], "missing native context/read path"
            fast_tail = re.split(r'\bret\b', context[0].split('bswap', 1)[1], maxsplit=1)[0]
            assert '305419896' in fast_tail, "ordinary read lost known host-register value"
            assert not re.search(r'\[[^\]]*\+ 56\]', fast_tail), "ordinary read forced a context reload"
        cases = [("disabled", {}), ("disabled", {"PPU_INLINE_READS": "OFF"}),
                 ("disabled", {"PPU_INLINE_READS": "typo"}),
                 ("null-init", {"PPU_INLINE_READS": "ON"}),
                 ("disabled" if sample else "enabled", {"PPU_INLINE_READS": "ON"})]
        # Each invocation is a fresh process because probe eligibility is cached.
        cases += [("disabled", {"PPU_INLINE_READS": "ON", name: value})
                  for name in PROBE_ENV_NAMES for value in ("1", "")]
        for case, extra in cases:
            env = os.environ.copy()
            for name in (*PROBE_ENV_NAMES, "PPU_INLINE_READS"):
                env.pop(name, None)
            env.update(extra)
            run = subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=15)
            assert run.returncode == 0 and "PASS" in run.stdout, (sample, case, extra, run.stdout, run.stderr)
            checks += 1
    print(f"PASS {checks} inline-read startup/range/fallback checks and compiled native-load path")


if __name__ == "__main__":
    main()
