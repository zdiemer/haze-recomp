"""Differentially run the extracted PPU memset against its guarded native hook."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from patches.extract_bulk_fill_oracle import fixture_functions
from patches.patch_bulk_fill import CALLER_FUNCTION, patch, verify_caller


FIXTURE = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ppu_context.h"
#include "ppu_native_memory.h"

constexpr size_t kArenaSize=0x20000;
extern "C" {
uint8_t* vm_base=nullptr;
uint32_t ppu_vm_size=0;
uint32_t ppu_hle_inject_base=0x03000000u;
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*)=nullptr;
}

@HELPERS@

static std::array<uint8_t,kArenaSize> memory_a{},memory_b{};
static bool native_available=true, native_locked=false;
static uint32_t reserved_line=UINT32_MAX;
static bool selector_on=false;
static unsigned native_lock_calls=0,native_unlock_calls=0,reservation_checks=0;
static unsigned native_lock_depth=0;
static unsigned write8_calls=0,write16_calls=0,write32_calls=0,write64_calls=0;

static void guest_write(uint64_t address,uint64_t value,unsigned width) {
    switch(width) { case 1: ++write8_calls; break; case 2: ++write16_calls; break;
        case 4: ++write32_calls; break; default: ++write64_calls; break; }
    if(address>UINT32_MAX || address+width>kArenaSize || !vm_base) return;
    for(unsigned i=0;i<width;++i)
        vm_base[static_cast<size_t>(address)+i]=static_cast<uint8_t>(value>>(8u*(width-1u-i)));
}
void vm_write8(uint64_t a,uint64_t v) { guest_write(a,v,1); }
void vm_write16(uint64_t a,uint64_t v) { guest_write(a,v,2); }
void vm_write32(uint64_t a,uint64_t v) { guest_write(a,v,4); }
void vm_write64(uint64_t a,uint64_t v) { guest_write(a,v,8); }
void func_00C11F80(ppu_context*) {}

extern "C" int ppu_native_memory_available(void) { return native_available; }
extern "C" void spu_lockline_lock(void) {
    assert(native_lock_depth==0);
    ++native_lock_depth; ++native_lock_calls; native_locked=true;
}
extern "C" void spu_lockline_unlock(void) {
    assert(native_lock_depth==1);
    --native_lock_depth; ++native_unlock_calls; native_locked=false;
}
extern "C" int spu_coh_is_reserved(uint32_t line) {
    ++reservation_checks;
    return line==reserved_line;
}

@FUNCTIONS@

static uint64_t random_state=0x8B5AD4CEF1230769ull;
static uint64_t next_random() {
    random_state^=random_state<<13; random_state^=random_state>>7; random_state^=random_state<<17;
    return random_state;
}
static void reset_counters() {
    native_lock_calls=native_unlock_calls=reservation_checks=0;
    write8_calls=write16_calls=write32_calls=write64_calls=0;
    assert(!native_locked && native_lock_depth==0);
}
static void run_case(uint64_t base,uint64_t length,uint8_t byte,uint64_t lr,
                     bool available,uint32_t reserve,bool expected_native) {
    for(size_t i=0;i<kArenaSize;++i) {
        memory_a[i]=memory_b[i]=static_cast<uint8_t>((i*37u+0x69u)&0xffu);
    }
    ppu_context ref,got;
    std::memset(&ref,0xA7,sizeof(ref));
    for(unsigned i=0;i<32;++i) ref.gpr[i]=next_random();
    ref.gpr[3]=base; ref.gpr[4]=byte; ref.gpr[5]=length; ref.lr=lr;
    ref.cr=static_cast<uint32_t>(next_random()); ref.ctr=next_random();
    std::memcpy(&got,&ref,sizeof(ref));
    native_available=available; reserved_line=reserve;
    ppu_vm_size=kArenaSize; ppu_hle_inject_base=0x03000000u;

    vm_base=memory_a.data(); reset_counters();
    reference_fill(&ref);
    const unsigned reference_writes64=write64_calls;

    vm_base=memory_b.data(); reset_counters();
    patched_fill(&got);
    assert(!native_locked && native_lock_depth==0);
    assert(std::memcmp(&ref,&got,sizeof(ref))==0);
    assert(std::memcmp(memory_a.data(),memory_b.data(),kArenaSize)==0);
    const bool attempt=selector_on && available && lr==0x00682774ull && byte==0 &&
        length==0x1450u && base>=0x10000u && uint64_t(base)+length<=kArenaSize;
    const bool lock_attempt=attempt && available;
    assert((native_lock_calls==1)==lock_attempt);
    assert((native_unlock_calls==1)==lock_attempt);
    assert((reservation_checks!=0)==lock_attempt);
    if(expected_native) assert(write64_calls<reference_writes64);
    else assert(write64_calls==reference_writes64);
}

static void check_real_span_api() {
    vm_base=memory_a.data();
    ppu_vm_size=0x220000u; ppu_hle_inject_base=0x03000000u;
    native_available=true; reserved_line=UINT32_MAX; reset_counters();
    const auto original=memory_a;
    assert(ppu_native_memory_begin_write(0x10020u,1));
    assert(native_locked && native_lock_depth==1);
    ppu_native_memory_end();
    assert(!native_locked && native_lock_calls==1 && native_unlock_calls==1);

    // Every intersecting line in a three-line range rejects before writing.
    for(uint32_t line: {0x10000u,0x10080u,0x10100u}) {
        reserved_line=line; reset_counters();
        assert(!ppu_native_memory_begin_write(0x10020u,0x140u));
        assert(!native_locked && native_lock_calls==1 && native_unlock_calls==1);
    }
    reserved_line=UINT32_MAX;

    ppu_hle_inject_base=0x10080u;
    reset_counters();
    assert(!ppu_native_memory_begin_write(0x10000u,0x100u));
    assert(native_lock_calls==0 && native_unlock_calls==0);
    ppu_hle_inject_base=0x03000000u;
    ppu_vm_size=kArenaSize;
    assert(ppu_native_memory_begin_write(kArenaSize-1u,1));
    ppu_native_memory_end();
    reset_counters();
    assert(!ppu_native_memory_begin_write(kArenaSize-1u,2));
    assert(!ppu_native_memory_begin_write(0xFFFFu,1));
    assert(!ppu_native_memory_begin_write(0x10000u,0));
    assert(native_lock_calls==0 && native_unlock_calls==0);

    // The inclusive 1 MiB cap is accepted; the next byte is rejected.
    ppu_vm_size=0x220000u;
    assert(ppu_native_memory_begin_write(0x10000u,0x100000u));
    ppu_native_memory_end();
    reset_counters();
    assert(!ppu_native_memory_begin_write(0x10000u,0x100001u));
    assert(native_lock_calls==0 && native_unlock_calls==0);
    native_available=false;
    assert(!ppu_native_memory_begin_write(0x10000u,64));
    assert(native_lock_calls==0 && native_unlock_calls==0);
    native_available=true;
    ppu_vm_size=0xC1000000u;
    assert(!ppu_native_memory_begin_write(0xC0000000u,1));
    assert(native_lock_calls==0 && native_unlock_calls==0);
    assert(std::memcmp(memory_a.data(),original.data(),kArenaSize)==0);
}

int main(int argc,char** argv) {
    assert(argc==3);
#ifdef _WIN32
    _putenv_s("HAZE_NATIVE_BULK_FILL",std::strcmp(argv[1],"DEFAULT") ? argv[1] : "");
    _putenv_s("HAZE_BULK_FILL_PROFILE",argv[2]);
#else
    if(std::strcmp(argv[1],"DEFAULT")) setenv("HAZE_NATIVE_BULK_FILL",argv[1],1);
    else unsetenv("HAZE_NATIVE_BULK_FILL");
    setenv("HAZE_BULK_FILL_PROFILE",argv[2],1);
#endif
    const bool on=std::strcmp(argv[1],"ON")==0;
    selector_on=on;
    check_real_span_api();
    for(uint32_t offset=0;offset<8;++offset) {
        run_case(0x10000u+offset,0x1450u,0,0x00682774ull,true,UINT32_MAX,on);
        if(offset==0) {
#ifdef _WIN32
            _putenv_s("HAZE_BULK_FILL_PROFILE",std::strcmp(argv[2],"1") ? "1" : "0");
#else
            setenv("HAZE_BULK_FILL_PROFILE",std::strcmp(argv[2],"1") ? "1" : "0",1);
#endif
        }
    }
#ifdef _WIN32
    _putenv_s("HAZE_NATIVE_BULK_FILL",on ? "OFF" : "ON");
#else
    setenv("HAZE_NATIVE_BULK_FILL",on ? "OFF" : "ON",1);
#endif
    run_case(0x10008u,0x1450u,0,0x00682774ull,true,UINT32_MAX,on); // cached selector
    run_case(0x10003u,0x1450u,0,0x00682774ull,false,UINT32_MAX,false); // probes/diagnostics
    run_case(0x10004u,0x1450u,0,0x00682774ull,true,0x10000u,false); // reserved first line
    run_case(0x10000u,0x1450u,0,0x00682774ull,true,0x10A00u,false); // reserved middle line
    run_case(0x10000u,0x1450u,0,0x00682774ull,true,0x11400u,false); // reserved final line
    run_case(0x10005u,0x1450u,0,0x00682770ull,true,UINT32_MAX,false); // other caller
    run_case(0x10006u,0x1450u,0x5Au,0x00682774ull,true,UINT32_MAX,false);
    for(uint64_t length: {0ull,1ull,7ull,8ull,63ull,64ull,65ull,127ull,128ull,129ull,
                          0x144Full,0x1451ull,0x100000ull,0x100001ull})
        run_case(0x10020u,length,0,0x00682774ull,true,UINT32_MAX,false);
    run_case(0xFFFFF000u,0x1450u,0,0x00682774ull,true,UINT32_MAX,false); // end exceeds u32
    run_case(0x100001000ull,0x1450u,0,0x00682774ull,true,UINT32_MAX,false); // high GPR3
    run_case(0x10000u,0x100000000ull,0,0x00682774ull,true,UINT32_MAX,false); // high length
    puts("PASS");
}
'''


class BulkFillSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.generated = ROOT / "generated/ppu"
        if not (cls.generated / "ppu_recomp_006.cpp").exists():
            raise unittest.SkipTest("Private PPU lift is unavailable")
        cls.source = (cls.generated / "ppu_recomp_006.cpp").read_text()
        cls.caller = (cls.generated / "ppu_recomp_003.cpp").read_text()

    def test_extraction_and_patch_are_fingerprint_guarded(self):
        helpers, original, changed = fixture_functions(self.generated)
        self.assertIn("static inline uint64_t ppc_rldicl", helpers)
        self.assertIn("loc_00C11F08:", original)
        self.assertIn("haze_bulk_fill_try(ctx)", changed)
        with self.assertRaises(ValueError):
            patch(self.source.replace("loc_00C11F08:", "loc_00C11F09:", 1), self.caller)
        with self.assertRaises(ValueError):
            caller_body = self.caller[
                self.caller.index(CALLER_FUNCTION):
                self.caller.index("\nvoid ", self.caller.index(CALLER_FUNCTION) + 5)
            ]
            mutated = caller_body.replace("ctx->gpr[5] = (int64_t)(int32_t)(0x1450);",
                                           "ctx->gpr[5] = (int64_t)(int32_t)(0x1440);", 1)
            verify_caller(self.caller.replace(caller_body, mutated, 1))


def _compile_and_run(compiler: str, sdk: Path, generated: Path, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    helpers, original, patched = fixture_functions(generated)
    fixture = output_dir / "bulk_fill_fixture.cpp"
    fixture_source = FIXTURE.replace("@HELPERS@", helpers)
    fixture_source = fixture_source.replace("@FUNCTIONS@", original + "\n" + patched)
    fixture.write_text(fixture_source, encoding="utf-8")
    shim = output_dir / "ppu_recomp.h"
    shim.write_text('#include "ppu_context.h"\n', encoding="utf-8")
    exe = output_dir / "bulk_fill_fixture.exe"
    helper = ROOT / "port/haze_bulk_fill.cpp"
    msvc = Path(compiler).name.lower() in ("clang-cl", "clang-cl.exe")
    includes = [output_dir, sdk / "runtime/ppu"]
    span_api = sdk / "runtime/ppu/ppu_native_memory.cpp"
    if msvc:
        command = [compiler, "/nologo", "/O2", "/std:c++17", "/EHsc", "/W4",
                   "/D_CRT_SECURE_NO_WARNINGS", f"/Fo{output_dir}/",
                   f"/Fe{exe}"]
        command += [f"/I{path}" for path in includes]
        command += [str(fixture), str(helper), str(span_api)]
    else:
        command = [compiler, "-O2", "-std=c++17", "-o", str(exe)]
        command += [f"-I{path}" for path in includes]
        command += [str(fixture), str(helper), str(span_api)]
    subprocess.run(command, check=True, cwd=ROOT)
    for mode in ("DEFAULT", "typo", "OFF", "ON"):
        for profile in ("0", "1"):
            run = subprocess.run([str(exe), mode, profile], check=True, capture_output=True,
                                 text=True, timeout=30, cwd=ROOT)
            if "PASS" not in run.stdout:
                raise AssertionError((mode, profile, run.stdout, run.stderr))
            expected_enabled = mode == "ON"
            if f"[haze-bulk-fill-mode] enabled={int(expected_enabled)}" not in run.stderr:
                raise AssertionError((mode, profile, "missing/incorrect immutable mode line", run.stderr))
            has_success = "[haze-bulk-fill] native bytes=" in run.stderr
            if has_success != expected_enabled:
                raise AssertionError((mode, profile, "unexpected native-success line", run.stderr))
            profile_lines = [line for line in run.stderr.splitlines()
                             if line.startswith("[haze-bulk-fill-profile]")]
            if profile == "0":
                if profile_lines:
                    raise AssertionError((mode, "profile-off emitted aggregate counters", profile_lines))
            else:
                if len(profile_lines) != 1:
                    raise AssertionError((mode, "expected one atexit aggregate", run.stderr))
                expected = ((13, 9, 4, 9 * 0x1440) if expected_enabled else (0, 0, 0, 0))
                observed = tuple(int(item.split("=", 1)[1])
                                 for item in profile_lines[0].split()[1:])
                if observed != expected:
                    raise AssertionError((mode, profile, expected, observed, run.stderr))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp")
    parser.add_argument("--generated", type=Path, default=ROOT / "generated/ppu")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BulkFillSource)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    _compile_and_run(args.compiler, args.sdk, args.generated, args.output_dir)
    print("PASS extracted full-context differential cases: HAZE_NATIVE_BULK_FILL OFF/ON")


if __name__ == "__main__":
    main()
