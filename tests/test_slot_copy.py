"""Differential test for the guarded native Haze job-slot payload copy."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT / "scripts"))
from patches.extract_slot_copy_oracle import extract
from patches.patch_slot_copy import (
    FUNCTION, MEMMOVE_FUNCTION, expected_traced_caller, function_body, patch, verify_caller,
)


FIXTURE = r'''#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ppu_context.h"
#include "ppu_native_memory.h"

constexpr uint32_t kRamSize=0x30000;
extern "C" {
uint8_t* vm_base=nullptr;
uint32_t ppu_vm_size=kRamSize;
uint32_t ppu_hle_inject_base=0x03000000u;
uint32_t g_null_sweep_hi=0;
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*)=nullptr;
}
#define DRAIN_TRAMPOLINE(ctx) do { while(g_trampoline_fn) { auto f=g_trampoline_fn; g_trampoline_fn=nullptr; f((void*)(ctx)); } } while(0)
uint8_t vm_read8(uint64_t); uint16_t vm_read16(uint64_t); uint32_t vm_read32(uint64_t); uint64_t vm_read64(uint64_t);
void vm_write8(uint64_t,uint8_t); void vm_write16(uint64_t,uint16_t); void vm_write32(uint64_t,uint32_t); void vm_write64(uint64_t,uint64_t);
void reference_memmove_backward(ppu_context*);
void func_00C0A6B0(ppu_context*);
void reference_caller_next_tail(ppu_context*);

@ROTATE@
static inline int64_t ppc_sraw(uint64_t rs, uint64_t rb) { unsigned sh=(unsigned)rb&63u; return sh>=64 ? ((int64_t)rs<0?-1:0) : ((int64_t)rs >> sh); }
@MOVE@
@BACKWARD@
@BITFILL@
void func_00C0A6B0(ppu_context* ctx) { reference_caller_next_helper(ctx); }
@NEXTTAIL@
@CONTINUATION@

static std::array<uint8_t,kRamSize> ref_mem{},got_mem{};
static bool available=true,locked=false;
static uint32_t reserved_line=UINT32_MAX;
static unsigned locks=0,unlocks=0,res_checks=0;
extern "C" int ppu_native_memory_available(void) { return available; }
extern "C" void spu_lockline_lock(void) { assert(!locked); locked=true; ++locks; }
extern "C" void spu_lockline_unlock(void) { assert(locked); locked=false; ++unlocks; }
extern "C" int spu_coh_is_reserved(uint32_t line) { assert(locked); ++res_checks; return line==reserved_line; }

static uint64_t read_be(uint64_t a,unsigned n) {
    if(a+n>kRamSize) return 0;
    uint64_t v=0; for(unsigned i=0;i<n;++i) v=(v<<8)|vm_base[a+i]; return v;
}
static void write_be(uint64_t a,uint64_t v,unsigned n) {
    if(a+n>kRamSize) return;
    for(unsigned i=0;i<n;++i) vm_base[a+i]=static_cast<uint8_t>(v >> (8*(n-i-1)));
}
uint8_t vm_read8(uint64_t a) { return static_cast<uint8_t>(read_be(a,1)); }
uint16_t vm_read16(uint64_t a) { return static_cast<uint16_t>(read_be(a,2)); }
uint32_t vm_read32(uint64_t a) { return static_cast<uint32_t>(read_be(a,4)); }
uint64_t vm_read64(uint64_t a) { return read_be(a,8); }
void vm_write8(uint64_t a,uint8_t v) { write_be(a,v,1); }
void vm_write16(uint64_t a,uint16_t v) { write_be(a,v,2); }
void vm_write32(uint64_t a,uint32_t v) { write_be(a,v,4); }
void vm_write64(uint64_t a,uint64_t v) { write_be(a,v,8); }
@SLOT_COPY@

static void seed(ppu_context& c) {
    std::memset(&c,0x5A,sizeof(c));
    for(unsigned i=0;i<32;++i) c.gpr[i]=0xAABB000000000000ull+i*0x101u;
    c.gpr[1]=0x20000; c.gpr[2]=0x1A000;
    c.gpr[23]=0x14020; c.gpr[24]=0x14000; c.gpr[28]=0x12000;
    c.gpr[29]=0x18000; c.gpr[31]=0x12000;
    c.gpr[3]=0x12010; c.gpr[4]=0x14000; c.gpr[5]=0x1410;
    c.lr=0x00682798; c.cr=0xDEADBEEF; c.ctr=0x12345678; c.xer=0xABCDEF01;
}
static void reset_memory() {
    for(unsigned i=0;i<kRamSize;++i) ref_mem[i]=got_mem[i]=static_cast<uint8_t>((i*73u+0x39u)&0xffu);
}
static void run_fallback(bool api_available,uint32_t reserve,uint64_t source,uint64_t dest,uint64_t size,uint64_t lr,
                        uint32_t inject=0x03000000u,uint32_t vm_size=kRamSize) {
    ppu_context ref,got; seed(ref);
    ref.gpr[3]=dest; ref.gpr[4]=source; ref.gpr[5]=size; ref.gpr[31]=dest>=0x10?dest-0x10:0;
    ref.lr=lr; std::memcpy(&got,&ref,sizeof(ref));
    reset_memory(); available=api_available; reserved_line=reserve;
    ppu_hle_inject_base=inject; ppu_vm_size=vm_size;
    vm_base=ref_mem.data(); reference_memmove(&ref); DRAIN_TRAMPOLINE(&ref);
    vm_base=got_mem.data();
    bool fast=haze_slot_copy_try(&got);
    if(!fast) { reference_memmove(&got); DRAIN_TRAMPOLINE(&got); }
    assert(!fast);
    assert(!locked && locks==unlocks);
    assert(std::memcmp(&ref,&got,sizeof(ref))==0);
    assert(ref_mem==got_mem);
}
static void run_shape_reject(uint64_t gpr3,uint64_t gpr4,uint64_t gpr5,uint64_t slot,uint64_t lr) {
    ppu_context c; seed(c); c.gpr[3]=gpr3; c.gpr[4]=gpr4; c.gpr[5]=gpr5; c.gpr[31]=slot; c.lr=lr;
    reset_memory(); vm_base=got_mem.data(); std::array<uint8_t,kRamSize> before=got_mem;
    ppu_context before_ctx=c; unsigned before_locks=locks, before_unlocks=unlocks;
    assert(!haze_slot_copy_try(&c));
    assert(std::memcmp(&c,&before_ctx,sizeof(c))==0 && got_mem==before);
    assert(!locked && locks==before_locks && unlocks==before_unlocks);
}
static void run_native_vm_end() {
    const uint32_t slot=kRamSize-0x1450u, dest=slot+0x10u, source=0x14000u;
    ppu_context c; seed(c); c.gpr[3]=dest; c.gpr[4]=source; c.gpr[5]=0x1410; c.gpr[31]=slot; c.lr=0x00682798;
    reset_memory(); vm_base=got_mem.data(); ppu_vm_size=kRamSize; ppu_hle_inject_base=0x03000000u;
    assert(haze_slot_copy_try(&c)); assert(c.gpr[3]==dest && !locked);
    for(unsigned i=0;i<0x1410;++i) assert(got_mem[dest+i]==static_cast<uint8_t>((((source+i)*73u+0x39u)&0xffu)));
}
static void run_native(uint32_t source=0x14000u) {
    assert(!locked); locks=unlocks=res_checks=0;
    ppu_context ref,got; seed(ref); ref.gpr[4]=source; std::memcpy(&got,&ref,sizeof(ref)); reset_memory();
    vm_base=ref_mem.data(); reference_memmove(&ref); DRAIN_TRAMPOLINE(&ref);
    vm_base=got_mem.data();
    assert(haze_slot_copy_try(&got));
    assert(!locked && locks==1 && unlocks==1 && res_checks>0);
    assert(got.gpr[3]==0x12010 && got.lr==ref.lr);
    for(unsigned i=14;i<32;++i) assert(got.gpr[i]==ref.gpr[i]);
    for(unsigned field=2;field<=4;++field) assert(ppu_cr_get(&got,field)==ppu_cr_get(&ref,field));
    assert(std::memcmp(got.fpr,ref.fpr,sizeof(got.fpr))==0);
    assert(std::memcmp(got.vr,ref.vr,sizeof(got.vr))==0);
    assert(std::memcmp(got_mem.data()+0x12010,ref_mem.data()+0x12010,0x1410)==0);
    assert(std::memcmp(got_mem.data(),ref_mem.data(),0x12010)==0);
    assert(std::memcmp(got_mem.data()+0x13420,ref_mem.data()+0x13420,kRamSize-0x13420)==0);

    // Run the exact source-pinned caller continuation through its next guest
    // helper. It overwrites volatile argument regs, writes metadata, and calls
    // the bitfill helper; compare the resulting memory and live ABI outputs.
    got.lr=ref.lr=0x00682798;
    vm_base=ref_mem.data(); audited_caller_continuation(&ref);
    vm_base=got_mem.data(); audited_caller_continuation(&got);
    assert(!locked);
    assert(ref_mem==got_mem);
    assert(ref.gpr[3]==got.gpr[3]);
    for(unsigned i=14;i<32;++i) assert(ref.gpr[i]==got.gpr[i]);
    for(unsigned field=2;field<=4;++field) assert(ppu_cr_get(&ref,field)==ppu_cr_get(&got,field));
}
int main(int argc,char** argv) {
    assert(argc==2);
#ifdef _WIN32
    _putenv_s("HAZE_NATIVE_SLOT_COPY",std::strcmp(argv[1],"DEFAULT") ? (std::strcmp(argv[1],"OFF") ? "ON" : "OFF") : "");
    _putenv_s("PS3_NULL_SWEEP",std::strcmp(argv[1],"SWEEP") ? "" : "1");
    _putenv_s("PT",std::strcmp(argv[1],"PT") ? "" : "0");
#else
    if(std::strcmp(argv[1],"DEFAULT")) setenv("HAZE_NATIVE_SLOT_COPY",std::strcmp(argv[1],"OFF") ? "ON" : "OFF",1);
    else unsetenv("HAZE_NATIVE_SLOT_COPY");
    if(std::strcmp(argv[1],"SWEEP")==0) setenv("PS3_NULL_SWEEP","1",1); else unsetenv("PS3_NULL_SWEEP");
    unsetenv("PT");
#endif
    if(std::strcmp(argv[1],"ON")==0 || std::strcmp(argv[1],"DEFAULT")==0) {
        run_native();
        run_native(0x12020u); // overlapping source/destination: original memmove semantics
        run_fallback(false,UINT32_MAX,0x14000,0x12010,0x1410,0x00682798);
        for(uint32_t line: {0x12000u,0x12080u,0x13400u})
            run_fallback(true,line,0x14000,0x12010,0x1410,0x00682798);
        run_fallback(true,UINT32_MAX,0xFFFF,0x12010,0x1410,0x00682798); // source low edge
        run_fallback(true,UINT32_MAX,0x14000,0x1010,0x1410,0x00682798); // destination low edge
        run_fallback(true,UINT32_MAX,0x14000,0x12010,0x1410,0x00682798,0x12000); // HLE overlap
        run_fallback(true,UINT32_MAX,0x14000,0x12010,0x1411,0x00682798); // wrong size
        run_fallback(true,UINT32_MAX,0x14000,0x12010,0x1410,0x00682794); // wrong LR
        run_fallback(true,UINT32_MAX,0x2FFF0,0x12010,0x1410,0x00682798); // source OOB
        run_fallback(true,UINT32_MAX,0x14000,0x2FFF0,0x1410,0x00682798); // dest OOB
        run_native_vm_end();
        run_fallback(true,UINT32_MAX,0x14000,kRamSize-0x1440u,0x1410,0x00682798,
                     0x03000000u,kRamSize-1u); // exact end is now one byte OOB
        run_shape_reject(0x1000012010ull,0x14000,0x1410,0x12000,0x00682798); // high dest GPR
        run_shape_reject(0x12010,0x1000014000ull,0x1410,0x12000,0x00682798); // high source GPR
        run_shape_reject(0x12010,0x14000,0x1410,0x1000012000ull,0x00682798); // high slot GPR
        run_shape_reject(0x12010,0x14000,0x100000000ull,0x12000,0x00682798); // high size
        run_shape_reject(0x12010,0x14000,0x1410,0xFFFFFFF0ull,0x00682798); // slot addition wrap
    } else {
        run_fallback(true,UINT32_MAX,0x14000,0x12010,0x1410,0x00682798);
    }
    haze_slot_copy_dump_stats(); haze_slot_copy_dump_stats();
    puts("PASS");
}
'''


class SlotCopySource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.generated = getattr(cls, "generated", ROOT / "generated/ppu")
        if not (cls.generated / "ppu_recomp_003.cpp").exists():
            raise unittest.SkipTest("private generated PPU source is unavailable")
        cls.caller = (cls.generated / "ppu_recomp_003.cpp").read_text()
        cls.memmove = (cls.generated / "ppu_recomp_005.cpp").read_text()

    def test_source_anchor_rejects_caller_and_memmove_drift(self):
        verify_caller(self.caller)
        patch(self.caller, self.memmove)
        original_body = function_body(self.caller, FUNCTION)
        traced = self.caller.replace(original_body, expected_traced_caller(original_body), 1)
        traced = '#include "haze_job_trace.h"\n' + traced
        self.assertIn("haze_slot_copy_try(ctx)", patch(traced, self.memmove))
        with self.assertRaises(ValueError):
            patch(self.caller.replace("0x00682798; func_00C07078", "0x00682799; func_00C07078", 1), self.memmove)
        move_sig = "void func_00C07078(ppu_context* ctx) {"
        move_pos = self.memmove.index(move_sig)
        read_pos = self.memmove.index("vm_read8(", move_pos)
        changed_memmove = self.memmove[:read_pos] + "vm_read8_changed(" + self.memmove[read_pos + len("vm_read8("):]
        with self.assertRaises(ValueError):
            patch(self.caller, changed_memmove)

    def test_actual_original_oracle_against_native_helper(self):
        rotate, move, backward, bitfill, next_tail, continuation = extract(self.generated)
        source = FIXTURE.replace("@ROTATE@", rotate).replace("@MOVE@", move)
        source = source.replace("@BACKWARD@", backward).replace("@BITFILL@", bitfill)
        source = source.replace("@NEXTTAIL@", next_tail)
        source = source.replace("@CONTINUATION@", continuation)
        helper = (ROOT / "port/haze_slot_copy.cpp").read_text()
        source = source.replace("@SLOT_COPY@", helper)
        outdir = self.output_dir.resolve()
        outdir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="slot-copy-", dir=outdir) as td:
            work = Path(td)
            (work / "fixture.cpp").write_text(source)
            (work / "ppu_recomp.h").write_text('#include "ppu_context.h"\n')
            exe = work / "slot-copy.exe"
            sdk = self.sdk.resolve()
            command = [
                self.compiler, "/nologo", "/std:c++17", "/O2", "/EHsc",
                f"/I{work}", f"/I{sdk / 'runtime/ppu'}", f"/I{sdk / 'include'}",
                str(work / "fixture.cpp"), str(sdk / "runtime/ppu/ppu_native_memory.cpp"),
                "/Fo" + str(work) + "\\", "/Fe:" + str(exe),
            ]
            built = subprocess.run(command, text=True, capture_output=True)
            if built.returncode:
                (outdir / "slot-copy-build.log").write_text(built.stdout + built.stderr)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            expected_summaries = {
                "DEFAULT": "[haze-slot-copy-summary] attempts=13 native=3 rejected=10 bytes=15408",
                "ON": "[haze-slot-copy-summary] attempts=13 native=3 rejected=10 bytes=15408",
                "SWEEP": "[haze-slot-copy-summary] attempts=1 native=0 rejected=1 bytes=0",
            }
            for mode in ("DEFAULT", "ON", "OFF", "SWEEP"):
                ran = subprocess.run([str(exe), mode], text=True, capture_output=True)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
                self.assertIn("PASS", ran.stdout)
                summaries = [line for line in ran.stderr.splitlines()
                             if line.startswith("[haze-slot-copy-summary]")]
                if mode in expected_summaries:
                    self.assertEqual(summaries, [expected_summaries[mode]])
                else:
                    self.assertEqual(summaries, [])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp")
    parser.add_argument("--generated", type=Path, default=ROOT / "generated/ppu")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    SlotCopySource.generated = args.generated
    SlotCopySource.compiler = args.compiler
    SlotCopySource.sdk = args.sdk
    SlotCopySource.output_dir = args.output_dir
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SlotCopySource)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    print("PASS extracted original memmove, fallback context, and caller continuation")


if __name__ == "__main__":
    main()
