"""Focused native regression tests for PPU byte/halfword read fast paths.

Extracts the real accessors and their bounds/probe guards from ppu_loader.cpp,
then links small host stubs for unrelated diagnostics. The baseline executable
reconstructs the previous native behavior by guarding the exact fast branches
with __EMSCRIPTEN__; the patched executable uses source as checked in.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PROBE_ENV_NAMES = (
    "PPU_MEMORY_DIAGNOSTICS", "PPU_HOTMAP", "PPU_RWATCH",
    "PPU_FORCE_READ_ADDR", "PPU_RVAL", "PPU_RVAL64", "PPU_SPINBT",
    "PPU_WVAL", "PPU_WWATCH", "PPU_WW_GUARD", "PT", "YDKJ_RWATCH",
    "PS3_NULL_READ", "PS3_NULL_SWEEP", "PS3_NULL_WRITE",
)
FORCE_ENV_NAMES = (*PROBE_ENV_NAMES, "PPU_FORCE_READ_VAL")

# These exact source blocks are the sole fast-path changes under test. A source
# drift or duplicate match fails loudly instead of testing a guessed variant.
READ8_FAST = (
    "    /* DEFLATE reads bytes in its inner loop. Match read32's uninstrumented\n"
    "     * path when memory probes are disabled; low-address diagnostics remain. */\n"
    "    if (!vm_memory_probes() && (uint32_t)a >= 0x10000u) return vm_base[(uint32_t)a];\n"
)
READ16_FAST = (
    "    if (!vm_memory_probes() && (uint32_t)a >= 0x10000u) {\n"
    "        uint16_t v; memcpy(&v, vm_base + (uint32_t)a, 2); return __builtin_bswap16(v);\n"
    "    }\n"
)


def extract_section(source: str, begin: str, end: str) -> str:
    start = source.find(begin)
    if start < 0:
        raise RuntimeError(f"source anchor missing: {begin!r}")
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise RuntimeError(f"source end anchor missing after: {begin!r}")
    return source[start:finish]


def replace_unique(text: str, before: str, after: str, label: str) -> str:
    count = text.count(before)
    if count != 1:
        raise RuntimeError(f"expected exactly one {label} anchor, found {count}")
    return text.replace(before, after, 1)


def extract_source(sdk: Path, baseline: bool, sample: bool) -> str:
    loader = sdk / "runtime/ppu/ppu_loader.cpp"
    source = loader.read_text(encoding="utf-8")
    vm_oob = extract_section(
        source, "static inline int vm_oob(uint32_t a, uint32_t n)",
        "/* A guest STORE into the first page.",
    )
    probes = extract_section(
        source, "static inline bool vm_memory_probes(void)",
        "static void vm_hotmap(",
    )
    read8 = extract_section(
        source, "uint8_t  vm_read8 (uint64_t a)", "uint16_t vm_read16(uint64_t a)",
    )
    read16 = extract_section(
        source, "uint16_t vm_read16(uint64_t a)", "static uint32_t vm_read32_slow(",
    )

    for accessor, body, anchor in (
        ("vm_read8", read8, READ8_FAST),
        ("vm_read16", read16, READ16_FAST),
    ):
        if body.count(anchor) != 1:
            raise RuntimeError(f"patched {accessor} branch must occur exactly once")
        if "#ifdef __EMSCRIPTEN__" in body:
            raise RuntimeError(f"{accessor} still has an Emscripten-only branch")

    if baseline:
        read8 = replace_unique(
            read8, READ8_FAST, "#ifdef __EMSCRIPTEN__\n" + READ8_FAST + "#endif\n",
            "vm_read8 baseline branch",
        )
        read16 = replace_unique(
            read16, READ16_FAST, "#ifdef __EMSCRIPTEN__\n" + READ16_FAST + "#endif\n",
            "vm_read16 baseline branch",
        )

    macro = "#define VM_SAMPLE_READS 1\n" if sample else ""
    return f'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define PPU_THREAD_LOCAL thread_local
{macro}

static unsigned char storage[0x11000];
uint8_t* vm_base = storage;
uint32_t ppu_vm_size = sizeof(storage);
static int oob_reports, hotmap_calls, null_read_calls, rwatch_calls;
static int vm_oob_report(uint32_t, uint32_t) {{ ++oob_reports; return 1; }}
static void vm_hotmap(uint32_t, int) {{ ++hotmap_calls; }}
static void ppu_null_read_report(uint32_t, int, void*) {{ ++null_read_calls; }}
static void ppu_rwatch_hit(uint32_t, int, void*) {{ ++rwatch_calls; }}
static unsigned long GetCurrentThreadId() {{ return 1; }}
static uint32_t ppu_prof_resolve_host(void*) {{ return 0; }}

{vm_oob}
{probes}
{read8}
{read16}

int main() {{
    const char* which = getenv("FASTPATH_CASE");
    assert(which);
    if (!strcmp(which, "ordinary")) {{
        assert(!vm_memory_probes());
        storage[0x10000] = 0xAB;
        storage[0x10001] = 0x12; storage[0x10002] = 0x34;
        storage[sizeof(storage)-2] = 0x5A; storage[sizeof(storage)-1] = 0xC3;
        assert(vm_read8(0x10000) == 0xAB);
        assert(vm_read16(0x10001) == 0x1234);
        assert(vm_read8(sizeof(storage)-1) == 0xC3);
        assert(vm_read16(sizeof(storage)-2) == 0x5AC3);
        if ({1 if not baseline and not sample else 0}) {{
            assert(hotmap_calls == 0 && rwatch_calls == 0);
        }} else {{
            assert(hotmap_calls > 0 && rwatch_calls > 0);
        }}
    }} else if (!strcmp(which, "low")) {{
        assert(vm_memory_probes() == {1 if sample else 0});
        storage[0x10] = 0xCD; storage[0x11] = 0x56; storage[0x12] = 0x78;
        assert(vm_read8(0x10) == 0xCD);
        assert(vm_read16(0x11) == 0x5678);
        assert(null_read_calls == 1 && hotmap_calls > 0 && rwatch_calls > 0);
    }} else if (!strcmp(which, "oob")) {{
        assert(vm_read8(sizeof(storage)) == 0);
        assert(vm_read16(sizeof(storage)-1) == 0);
        assert(oob_reports == 2 && hotmap_calls == 0 && rwatch_calls == 0);
    }} else if (!strcmp(which, "probe")) {{
        assert(vm_memory_probes());
        storage[0x10000] = 0xEF; storage[0x10001] = 0xBE; storage[0x10002] = 0xAD;
        assert(vm_read8(0x10000) == 0xEF);
        assert(vm_read16(0x10001) == 0xBEAD);
        assert(hotmap_calls > 0 && rwatch_calls > 0);
    }} else if (!strcmp(which, "force")) {{
        assert(vm_memory_probes());
        assert(vm_read8(0x10001) == 0xA5);
        assert(hotmap_calls > 0 && rwatch_calls == 0);
    }} else if (!strcmp(which, "sample")) {{
        assert(vm_memory_probes());
        storage[0x10000] = 0xEF; storage[0x10001] = 0xBE; storage[0x10002] = 0xAD;
        assert(vm_read8(0x10000) == 0xEF);
        assert(vm_read16(0x10001) == 0xBEAD);
        assert(hotmap_calls > 0 && rwatch_calls > 0);
    }} else {{ assert(!"unknown case"); }}
    puts("PASS");
}}
'''


def compiler_command(compiler: str, source: Path, exe: Path, define_sample: bool) -> list[str]:
    name = Path(compiler).name.lower()
    if name in ("clang-cl", "clang-cl.exe"):
        cmd = [compiler, "/nologo", "/std:c++17", "/O2", "/W4", "/wd4100", "/wd4127"]
        if define_sample:
            cmd.append("/DVM_SAMPLE_READS=1")
        return [*cmd, str(source), f"/Fe:{exe}", f"/Fo:{exe.with_suffix('.obj')}"]
    cmd = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter"]
    if define_sample:
        cmd.append("-DVM_SAMPLE_READS=1")
    return [*cmd, str(source), "-o", str(exe)]


def run_case(exe: Path, case: str, extra_env: dict[str, str] | None = None) -> None:
    env = os.environ.copy()
    for name in FORCE_ENV_NAMES:
        env.pop(name, None)
    env["FASTPATH_CASE"] = case
    if case == "force":
        env["PPU_FORCE_READ_ADDR"] = "10001"
        env["PPU_FORCE_READ_VAL"] = "A5"
    if extra_env:
        env.update(extra_env)
    result = subprocess.run([str(exe)], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"{exe.name} / {case} failed:\n{result.stdout}")
    if "PASS" not in result.stdout:
        raise RuntimeError(f"{exe.name} / {case} produced no PASS marker")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, help="clang++ or clang-cl executable")
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp",
                        help="ps3recomp SDK root")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="directory for generated fixtures and executables")
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    loader = sdk / "runtime/ppu/ppu_loader.cpp"
    digest = hashlib.sha256(loader.read_bytes()).hexdigest()
    print(f"SDK source SHA-256 {digest}")

    variants = (("baseline", True, False), ("patched", False, False),
                ("sample", False, True))
    checks = 0
    for label, baseline, sample in variants:
        fixture = args.output_dir / f"memory_read_fastpath_{label}.cpp"
        exe_label = "fast" if label == "patched" else label
        exe = args.output_dir / f"memory_read_fastpath_{exe_label}.exe"
        fixture.write_text(extract_source(sdk, baseline, sample), encoding="utf-8")
        command = compiler_command(args.compiler, fixture, exe, sample)
        compiled = subprocess.run(command, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if compiled.returncode:
            raise RuntimeError(f"{label} compile failed:\n{compiled.stdout}")

        if label == "sample":
            cases = (("sample", {}), ("low", {}), ("oob", {}))
        else:
            cases = [(case, {}) for case in ("ordinary", "low", "oob", "force")]
            cases.extend(("probe", {name: "1"}) for name in PROBE_ENV_NAMES)
        for case, env in cases:
            run_case(exe, case, env)
            checks += 1
            print(f"PASS {label} {case}" + (f" [{next(iter(env))}]" if env else ""))
    print(f"PASS {checks} focused native memory-read checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
