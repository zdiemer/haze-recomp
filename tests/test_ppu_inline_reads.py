"""Compile the real generated-memory shim's reads against the runtime page table.

Original-accessor stubs make fallback observable without touching device or
unmapped addresses. test_vm_plain_pages.py checks how startup fills the table.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess

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
#include "spu_coherency.h"
alignas(64) static uint8_t storage[0x11000];
extern "C" {
uint8_t* vm_base = storage;
uint32_t ppu_vm_size = sizeof(storage);
uint32_t ppu_hle_inject_base = 0x03000000;
uint8_t ppu_vm_plain_pages[0x10000];
unsigned char spu_coh_reservation_bitmap[SPU_COH_BITMAP_SIZE];
unsigned char spu_coh_page_marked[1u << (32u - SPU_COH_PAGE_SHIFT)];
uint32_t g_barrier_sync_watch, g_null_sweep_hi;
}
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
#define WRITE_FALLBACK(bits, type) extern "C" NOINLINE void vm_write##bits(uint64_t address, type value) {     ++calls; last_address=address; last_width=(unsigned)value; }
WRITE_FALLBACK(8, uint8_t)
WRITE_FALLBACK(16, uint16_t)
WRITE_FALLBACK(32, uint32_t)
WRITE_FALLBACK(64, uint64_t)
#include "haze_ppu_inline_memory.h"
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
int main() {
    // No generated caller may inline reads before the runtime fills the table.
    for (unsigned width: {1u,2u,4u,8u}) fallback(width,0x10001);
    ppu_vm_plain_pages[1]=1; // the fixture's storage page
    for (unsigned width: {1u,2u,4u,8u}) {
        direct(width,0x10000);
        direct(width,0x10001); // unaligned, explicit big-endian oracle
        direct(width,0x10003,0xFEDCBA9800000000ull); // legacy EA truncation
        direct(width,sizeof(storage)-width); // last complete valid value
        fallback(width,0xFFFF); // the null page is never ordinary RAM
        fallback(width,0x20000); // nor a page the runtime left out
        fallback(width,0xFFFFFFFFull);
        fallback(width,0xABCDEF00FFFFFFFFull); // full input forwarded
    }
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
    // Stores outside ordinary RAM stay on the reservation-aware ABI.
    int before=calls;
    vm_write32(0x123400000100ull,71);
    assert(calls==before+1 && last_address==0x123400000100ull && last_width==71);
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
    fixture = args.output_dir / "inline_reads.cpp"
    fixture.write_text("#include <initializer_list>\n" + FIXTURE, encoding="utf-8")
    msvc = Path(args.compiler).name.lower() in ("clang-cl", "clang-cl.exe")
    exe = args.output_dir / "inline_reads.exe"
    assembly = exe.with_suffix(".asm")
    includes = [ROOT / "port", args.sdk / "runtime/ppu", args.sdk / "runtime/spu", args.sdk / "include", args.recomp]
    if msvc:
        command = [args.compiler, "/nologo", "/O2", "/std:c++17", "/EHsc",
                   "/W4", "/D_CRT_SECURE_NO_WARNINGS", "/clang:-fno-strict-aliasing", "/FA", f"/Fa{assembly}",
                   f"/Fo{exe.with_suffix('.obj')}", f"/Fe{exe}"]
        command += [f"/I{p}" for p in includes]
    else:
        command = [args.compiler, "-O2", "-std=c++17", "-fno-strict-aliasing", "-o", str(exe)]
        command += [f"-I{p}" for p in includes]
    command.append(str(fixture))
    subprocess.run(command, check=True)
    if msvc:
        asm = assembly.read_text()
        probe = re.search(r"inline_codegen_probe:.*?(?:# -- End function|ENDP)", asm, re.S)
        assert probe, "missing representative generated caller in assembly"
        assert "bswap" in probe[0] and "vm_read32" in probe[0], probe[0]
        assert "ppu_vm_plain_pages" in probe[0], "inline read does not test the page table"
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
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
    assert run.returncode == 0 and "PASS" in run.stdout, (run.stdout, run.stderr)
    print("PASS inline-read page-table/fallback checks and compiled native-load path")


if __name__ == "__main__":
    main()
