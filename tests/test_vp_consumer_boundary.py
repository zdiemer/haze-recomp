"""Focused native tests for the live FIFO VP-burst boundary.

The fixture extracts the actual guarded function from cellGcmSys.c and links
the SDK VP-burst helpers plus generic per-word consumers. Host memory and
renderer wrappers are mocked; no game image or runtime is loaded.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
BEGIN = "/* BEGIN GCM VP BURST BOUNDARY: extracted by the boundary regression test. */"
END = "/* END GCM VP BURST BOUNDARY */"


def extract_between(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1 or source.count(end) != 1:
        raise RuntimeError(f"expected unique source markers {begin!r} / {end!r}")
    start = source.index(begin)
    finish = source.index(end, start + len(begin))
    if finish < start:
        raise RuntimeError("boundary end marker precedes begin marker")
    return source[start:finish]


def extract_function(source: str, signature: str) -> str:
    positions = [m.start() for m in re.finditer(re.escape(signature), source)]
    if not positions:
        raise RuntimeError(f"function source anchor missing: {signature!r}")
    start = positions[-1]  # Windows implementation follows the non-Windows stub.
    opening = source.find("{", start + len(signature))
    if opening < 0:
        raise RuntimeError(f"function body missing: {signature!r}")
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise RuntimeError(f"unterminated function body: {signature!r}")


def diagnostic_names(mode_source: str) -> tuple[str, ...]:
    match = re.search(r"const char\* names\[\]\s*=\s*\{(.*?)\};", mode_source, re.S)
    if not match:
        raise RuntimeError("could not extract diagnostic environment selector")
    names = tuple(re.findall(r'"([A-Z0-9_]+)"', match.group(1)))
    if not names or len(names) != len(set(names)):
        raise RuntimeError("diagnostic selector is empty or has duplicate names")
    return names


def source_fixture(boundary: str, live_wrapper: str, engine_wrapper: str) -> str:
    return f'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_commands.h"
#include "rsx_dispatch.h"
#include "rsx_vp_burst.h"
#include "ppu_native_memory.h"

#define VM_HLE_INJECT_BASE 0xC0000000u
uint32_t ppu_hle_inject_base = VM_HLE_INJECT_BASE;
uint32_t ppu_vm_size = 0x40000u;
static uint32_t packet_get;
static int mapping_fail_at = -1;
static int map_hle_region;
static int map_high_region;
static int mapping_calls, read_calls;
static int live_enabled = 1, engine_enabled = 1;
static int wrapper_calls, wrapper_order[2];
static int live_bulk_calls, engine_bulk_calls, generic_live_calls;
static int mock_composite, mock_debug;
static uint32_t last_read_addresses[32], loaded_words[32];

static rsx_dispatch live_expected, engine_expected;
static rsx_state real_state, expected_state, state_before;
static struct {{ int ready; rsx_dispatch rsx; }} g;
static struct {{ int ready; rsx_dispatch rsx; }} engine_g;
#define live_state g.rsx
#define engine_state engine_g.rsx
static int g_ld_debug_layer_enabled;
static volatile int g_ld_movie_mode;
static int g_ld_movie_composite_ui;
static int g_ld_movie_track_rsx = 0;

static int ld_movie_composite_ui_enabled(void) {{ return g_ld_movie_composite_ui; }}
void rsx_live_draw_method(uint32_t method, uint32_t arg);
void rsx_live_draw_vp_burst_32(const uint32_t words[32]);
void rsx_draw_engine_vp_burst_32(const uint32_t words[32]);

uint32_t gcm_io2ea(uint32_t io)
{{
    const uint32_t index = (io - (packet_get + 4u)) / 4u;
    ++mapping_calls;
    if (index >= 32u || (int)index == mapping_fail_at) return 0;
    if (map_hle_region) return VM_HLE_INJECT_BASE + 0x1000u + index * 4u;
    if (map_high_region) return 0xC1000000u + index * 4u;
    /* Deliberately split the packet across noncontiguous IO mappings. */
    return index < 16u ? 0x20000u + index * 4u
                       : 0x30000u + (index - 16u) * 4u;
}}

uint32_t vm_read32(uint64_t address)
{{
    if (read_calls >= 32) return 0xBAD00000u;
    last_read_addresses[read_calls] = (uint32_t)address;
    const uint32_t index = read_calls++;
    /* Model PS3 big-endian bytes; vm_read32 returns the host-order word. */
    const uint8_t b0 = 0xA5u, b1 = 0, b2 = 0, b3 = (uint8_t)index;
    loaded_words[index] = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
                          ((uint32_t)b2 << 8) | b3;
    return loaded_words[index];
}}
void vm_write32(uint64_t address, uint32_t value)
{{ (void)address; (void)value; }}
void ps3_ms(const char* key) {{ (void)key; }}

int rsx_live_draw_enabled(void) {{ return live_enabled; }}
int rsx_draw_engine_enabled(void) {{ return engine_enabled; }}

{boundary}

/* Instrument only the helper call made by each extracted real wrapper. The
 * actual helper remains linked below from the SDK source. */
static int test_live_dispatch(rsx_dispatch* rsx, uint32_t subchannel,
                              uint32_t method, const uint32_t words[32])
{{
    ++live_bulk_calls;
    assert(rsx == &live_state && subchannel == 0 && method == RSX_VP_BURST_METHOD);
    assert(real_state.vp_ucode_write == state_before.vp_ucode_write);
    assert(wrapper_calls < 2); wrapper_order[wrapper_calls++] = 1;
    return rsx_vp_burst_dispatch_32(rsx, subchannel, method, words);
}}
static int test_engine_dispatch(rsx_dispatch* rsx, uint32_t subchannel,
                                uint32_t method, const uint32_t words[32])
{{
    ++engine_bulk_calls;
    assert(rsx == &engine_state && subchannel == 0 && method == RSX_VP_BURST_METHOD);
    assert(real_state.vp_ucode_write == state_before.vp_ucode_write);
    assert(wrapper_calls < 2); wrapper_order[wrapper_calls++] = 2;
    return rsx_vp_burst_dispatch_32(rsx, subchannel, method, words);
}}

void rsx_draw_engine_vp_burst_32(const uint32_t words[32]);
#define rsx_vp_burst_dispatch_32 test_live_dispatch
{live_wrapper}
#undef rsx_vp_burst_dispatch_32

#define rsx_vp_burst_dispatch_32 test_engine_dispatch
#define g engine_g
{engine_wrapper}
#undef g
#undef rsx_vp_burst_dispatch_32

void rsx_live_draw_method(uint32_t method, uint32_t arg)
{{
    ++generic_live_calls;
    if (!g.ready) return;
    if (g_ld_movie_mode && !g_ld_movie_composite_ui && !g_ld_movie_track_rsx)
        return;
    rsx_dispatch_method(&live_state, method, arg);
}}

static void reset_case(void)
{{
    packet_get = 0x1000u;
    mapping_fail_at = -1;
    map_hle_region = map_high_region = 0;
    mapping_calls = read_calls = wrapper_calls = 0;
    live_bulk_calls = engine_bulk_calls = generic_live_calls = 0;
    memset(wrapper_order, 0, sizeof(wrapper_order));
    memset(last_read_addresses, 0, sizeof(last_read_addresses));
    memset(loaded_words, 0, sizeof(loaded_words));
    ppu_vm_size = 0x40000u;
    live_enabled = engine_enabled = 1;
    mock_composite = mock_debug = 0;
    g.ready = 1;
    engine_g.ready = 1;
    g_ld_movie_composite_ui = 0;
    g_ld_debug_layer_enabled = 0;
    g_ld_movie_mode = 0;
    g_ld_movie_track_rsx = 0;

    rsx_dispatch_init(&live_state, NULL);
    rsx_dispatch_init(&engine_state, NULL);
    rsx_dispatch_init(&live_expected, NULL);
    rsx_dispatch_init(&engine_expected, NULL);
    live_state.regs[0x1E9Cu >> 2] = live_expected.regs[0x1E9Cu >> 2] = 7u;
    engine_state.regs[0x1E9Cu >> 2] = engine_expected.regs[0x1E9Cu >> 2] = 11u;
    live_state.regs[0x1234u >> 2] = live_expected.regs[0x1234u >> 2] = 0x11223344u;
    engine_state.regs[0x1234u >> 2] = engine_expected.regs[0x1234u >> 2] = 0x55667788u;

    rsx_state_init(&real_state);
    real_state.vp_ucode_write = 0x80u;
    real_state.vp_ucode_bytes = 0x74u;
    real_state.vp_dirty = 0;
    expected_state = real_state;
    state_before = real_state;
}}

static void build_reference(void)
{{
    for (uint32_t i = 0; i < 32; ++i) {{
        const uint32_t method = RSX_VP_BURST_METHOD + i * 4u;
        const uint32_t word = 0xA5000000u | i;
        rsx_dispatch_method(&live_expected, method, word);
        rsx_dispatch_method(&engine_expected, method, word);
        assert(rsx_process_method(&expected_state, method, word) == 0);
    }}
}}

static void assert_success(uint32_t get, uint32_t put, uint32_t header)
{{
    reset_case();
    packet_get = get;
    build_reference();
    assert(gcm_try_vp_burst(&real_state, get, put, header) == 1);
    assert(read_calls == 32 && mapping_calls == 32);
    assert(wrapper_calls == 2 && wrapper_order[0] == 1 && wrapper_order[1] == 2);
    for (uint32_t i = 0; i < 32; ++i) {{
        const uint32_t expected_address = i < 16u ? 0x20000u + i * 4u
                                                   : 0x30000u + (i - 16u) * 4u;
        assert(last_read_addresses[i] == expected_address);
        assert(loaded_words[i] == (0xA5000000u | i));
    }}
    assert(memcmp(&live_state, &live_expected, sizeof(live_state)) == 0);
    assert(memcmp(&engine_state, &engine_expected, sizeof(engine_state)) == 0);
    assert(memcmp(&real_state, &expected_state, sizeof(real_state)) == 0);
}}

static void assert_rejected(uint32_t get, uint32_t put, uint32_t header,
                            int fail_at, int use_hle, int use_high, uint32_t vm_size,
                            uint32_t cursor)
{{
    reset_case();
    mapping_fail_at = fail_at;
    map_hle_region = use_hle;
    map_high_region = use_high;
    ppu_vm_size = vm_size;
    real_state.vp_ucode_write = cursor;
    state_before = real_state;
    assert(gcm_try_vp_burst(&real_state, get, put, header) == 0);
    assert(read_calls == 0 && wrapper_calls == 0);
    assert(memcmp(&real_state, &state_before, sizeof(real_state)) == 0);
}}

int main(int argc, char** argv)
{{
    assert(argc == 2);
    const char* test = argv[1];
    if (!strcmp(test, "live-wrapper")) {{
        for (int ready = 0; ready <= 1; ++ready)
        for (int composite = 0; composite <= 1; ++composite)
        for (int debug = 0; debug <= 1; ++debug)
        for (int movie = 0; movie <= 1; ++movie) {{
            reset_case();
            g.ready = ready;
            mock_composite = g_ld_movie_composite_ui = composite;
            mock_debug = g_ld_debug_layer_enabled = debug;
            g_ld_movie_mode = movie;
            uint32_t words[32];
            for (uint32_t i = 0; i < 32; ++i) words[i] = 0xA5000000u | i;
            for (uint32_t i = 0; i < 32; ++i)
                rsx_dispatch_method(&live_expected, RSX_VP_BURST_METHOD + i * 4u, words[i]);
            const rsx_dispatch live_before = live_state;
            const int serialized = composite || debug || movie;
            rsx_live_draw_vp_burst_32(words);
            assert(generic_live_calls == (serialized ? 32 : 0));
            assert(live_bulk_calls == (ready && !serialized ? 1 : 0));
            if (!ready) {{
                /* A serialized fallback still invokes the generic guarded
                 * method 32 times; the mock preserves its not-ready no-op. */
                assert(memcmp(&live_state, &live_before, sizeof(live_state)) == 0);
            }} else if (movie && !composite) {{
                assert(memcmp(&live_state, &live_before, sizeof(live_state)) == 0);
            }} else {{
                assert(memcmp(&live_state, &live_expected, sizeof(live_state)) == 0);
            }}
        }}
        puts("PASS"); return 0;
    }} else if (!strcmp(test, "draw-wrapper")) {{
        for (int ready = 0; ready <= 1; ++ready) {{
            reset_case(); engine_g.ready = ready;
            uint32_t words[32];
            for (uint32_t i = 0; i < 32; ++i) words[i] = 0xA5000000u | i;
            for (uint32_t i = 0; i < 32; ++i)
                rsx_dispatch_method(&engine_expected, RSX_VP_BURST_METHOD + i * 4u, words[i]);
            const rsx_dispatch engine_before = engine_state;
            rsx_draw_engine_vp_burst_32(words);
            assert(engine_bulk_calls == (ready ? 1 : 0));
            if (ready) assert(memcmp(&engine_state, &engine_expected, sizeof(engine_state)) == 0);
            else assert(memcmp(&engine_state, &engine_before, sizeof(engine_state)) == 0);
        }}
        puts("PASS"); return 0;
    }}
    if (!strcmp(test, "success")) {{
        assert_success(0x1000u, 0x1000u + 132u, 0x00800B80u);
    }} else if (!strcmp(test, "page-cross-success")) {{
        assert_success(0x000FFFBCu, 0x00100040u, 0x00800B80u);
    }} else if (!strcmp(test, "disabled")) {{
        reset_case(); live_enabled = engine_enabled = 0; build_reference();
        assert(gcm_try_vp_burst(&real_state, 0x1000u, 0x1000u + 132u, 0x00800B80u) == 1);
        assert(read_calls == 32 && wrapper_calls == 0);
        assert(memcmp(&real_state, &expected_state, sizeof(real_state)) == 0);
    }} else if (!strcmp(test, "live-only")) {{
        reset_case(); engine_enabled = 0; build_reference();
        assert(gcm_try_vp_burst(&real_state, 0x1000u, 0x1000u + 132u, 0x00800B80u) == 1);
        assert(wrapper_calls == 1 && wrapper_order[0] == 1);
        assert(memcmp(&live_state, &live_expected, sizeof(live_state)) == 0);
        assert(memcmp(&real_state, &expected_state, sizeof(real_state)) == 0);
    }} else if (!strcmp(test, "engine-only")) {{
        reset_case(); live_enabled = 0; build_reference();
        assert(gcm_try_vp_burst(&real_state, 0x1000u, 0x1000u + 132u, 0x00800B80u) == 1);
        assert(wrapper_calls == 1 && wrapper_order[0] == 2);
        assert(memcmp(&engine_state, &engine_expected, sizeof(engine_state)) == 0);
        assert(memcmp(&real_state, &expected_state, sizeof(real_state)) == 0);
    }} else if (!strcmp(test, "bad-header")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x00800B84u, -1, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "non-increment")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x40800B80u, -1, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "wrong-subchannel")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x00802B80u, -1, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "wrong-count-low")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x007C0B80u, -1, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "wrong-count-high")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x00840B80u, -1, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "short-put")) {{
        assert_rejected(0x1000u, 0x1000u + 131u, 0x00800B80u, -1, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "get-alignment")) {{
        assert_rejected(0x1002u, 0x1002u + 132u, 0x00800B80u, -1, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "get-overflow")) {{
        assert_rejected(0xFFFFFFFCu, 0xFFFFFFFFu, 0x00800B80u, -1, 0, 0, 0xFFFFFFFFu, 0x80u);
    }} else if (!strcmp(test, "cursor-overflow")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x00800B80u, -1, 0, 0, 0x40000u, UINT32_MAX - 3u);
    }} else if (!strcmp(test, "missing-first-page")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x00800B80u, 7, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "missing-second-page")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x00800B80u, 20, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "missing-map-all")) {{
        for (int i = 0; i < 32; ++i)
            assert_rejected(0x1000u, 0x1000u + 132u, 0x00800B80u,
                            i, 0, 0, 0x40000u, 0x80u);
    }} else if (!strcmp(test, "vm-size-oob")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x00800B80u, -1, 0, 0, 0x28000u, 0x80u);
    }} else if (!strcmp(test, "hle-region")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x00800B80u, -1, 1, 0, 0xFFFFFFFFu, 0x80u);
    }} else if (!strcmp(test, "high-region")) {{
        assert_rejected(0x1000u, 0x1000u + 132u, 0x00800B80u, -1, 0, 1, 0xFFFFFFFFu, 0x80u);
    }} else {{ assert(!"unknown test case"); }}
    puts("PASS");
    return 0;
}}
'''


def diagnostic_fixture(sample: bool, header_name: str) -> str:
    macro = "#define VM_SAMPLE_READS 1\n" if sample else ""
    return f'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
{macro}#include "{header_name}"
int main(void)
{{
    gcm_vp_burst_poll();
    const int eligible = s_vp_burst_mode.enabled && !s_vp_burst_mode.diagnostics;
    puts(eligible ? "ELIGIBLE" : "ORIGINAL");
    return 0;
}}
'''


def compiler_base(compiler: str, sdk: Path) -> list[str]:
    return [compiler, "/nologo", "/std:c17", "/O2", "/W4", "/wd4100", "/wd4127",
            f"/I{sdk / 'libs/video'}", f"/I{sdk / 'include'}",
            f"/I{sdk / 'runtime/ppu'}", f"/I{sdk / 'runtime/memory'}"]


def compile_object(base: list[str], source: Path, output: Path) -> Path:
    obj = output / f"{source.stem}.obj"
    command = [*base, "/c", str(source), f"/Fo:{obj}"]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"compile failed for {source.name}:\n{result.stdout}")
    return obj


def link_executable(base: list[str], objects: list[Path], executable: Path) -> None:
    result = subprocess.run([*base, *(str(p) for p in objects), f"/Fe:{executable}"],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"link failed for {executable.name}:\n{result.stdout}")


def run_exe(executable: Path, args: tuple[str, ...], env: dict[str, str], expected: str) -> None:
    result = subprocess.run([str(executable), *args], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode or expected not in result.stdout:
        raise RuntimeError(f"{executable.name} {args} failed (exit={result.returncode}):\n{result.stdout}")


def clean_env(names: tuple[str, ...]) -> dict[str, str]:
    env = os.environ.copy()
    for name in names:
        env.pop(name, None)
    for name in ("RSX_VP_BURST", "RSX_VP_BURST_FILE", "RSX_VP_BURST_PROFILE"):
        env.pop(name, None)
    return env


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, help="clang-cl executable")
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp")
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if Path(args.compiler).name.lower() not in ("clang-cl", "clang-cl.exe"):
        parser.error("--compiler must name clang-cl")
    sdk = args.sdk.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    video = sdk / "libs/video"
    cell_source = video / "cellGcmSys.c"
    burst_source = video / "rsx_vp_burst.c"
    dispatch_source = video / "rsx_dispatch.c"
    commands_source = video / "rsx_commands.c"
    live_source = video / "rsx_live_draw.c"
    engine_source = video / "rsx_draw_engine.c"
    mode_source = video / "gcm_vp_burst_mode.h"
    sources = (cell_source, burst_source, dispatch_source, commands_source,
               live_source, engine_source, mode_source)
    for source in sources:
        print(f"SHA-256 {source.relative_to(sdk)} {hashlib.sha256(source.read_bytes()).hexdigest()}")

    cell_text = cell_source.read_text(encoding="utf-8")
    boundary = extract_between(cell_text, BEGIN, END)
    if "ppu_vm_size" not in boundary:
        raise RuntimeError("boundary must validate against ppu_vm_size, not an unbounded VM")
    # Confirm the caller still gates optimized dispatch on all diagnostics.
    gate = "if (vp_burst_packet && s_vp_burst_mode.enabled && !s_vp_burst_mode.diagnostics &&"
    if cell_text.count(gate) != 1:
        raise RuntimeError("expected unique optimized-path diagnostic gate in cellGcmSys.c")
    live_text = live_source.read_text(encoding="utf-8")
    engine_text = engine_source.read_text(encoding="utf-8")
    live_wrapper = extract_function(
        live_text, "void rsx_live_draw_vp_burst_32(const u32 words[32])")
    engine_wrapper = extract_function(
        engine_text, "void rsx_draw_engine_vp_burst_32(const u32 words[32])")

    base = compiler_base(args.compiler, sdk)
    fixture = out / "vp_consumer_boundary_fixture.c"
    fixture.write_text(source_fixture(boundary, live_wrapper, engine_wrapper), encoding="utf-8")
    object_paths = [compile_object(base, source, out) for source in
                    (fixture, burst_source, dispatch_source, commands_source)]
    executable = out / "vp_consumer_boundary.exe"
    link_executable(base, object_paths, executable)

    cases = ("success", "page-cross-success", "live-wrapper", "draw-wrapper",
             "disabled", "live-only", "engine-only", "bad-header",
             "non-increment", "wrong-subchannel", "wrong-count-low", "wrong-count-high",
             "short-put", "get-alignment", "get-overflow", "cursor-overflow",
             "missing-first-page", "missing-second-page", "missing-map-all",
             "vm-size-oob", "hle-region",
             "high-region")
    env = clean_env(())
    passed = 0
    for case in cases:
        run_exe(executable, (case,), env, "PASS")
        print(f"PASS boundary {case}")
        passed += 1

    mode_text = mode_source.read_text(encoding="utf-8")
    diag_names = diagnostic_names(mode_text)
    diag_fixture = out / "vp_consumer_diagnostic_fixture.c"
    for sample in (False, True):
        label = "sample" if sample else "normal"
        diag_fixture.write_text(diagnostic_fixture(sample, mode_source.name), encoding="utf-8")
        diag_obj = compile_object(base, diag_fixture, out)
        diag_exe = out / f"vp_consumer_diagnostics_{label}.exe"
        link_executable(base, [diag_obj], diag_exe)
        for flag in (None, *diag_names):
            diag_env = clean_env(diag_names)
            diag_env["RSX_VP_BURST"] = "1"
            if flag:
                diag_env[flag] = "1"
            expected = "ELIGIBLE" if flag is None and not sample else "ORIGINAL"
            run_exe(diag_exe, (), diag_env, expected)
            name = "clear" if flag is None else flag
            print(f"PASS diagnostics {label} {name} -> {expected}")
            passed += 1

    print(f"PASS {passed} VP consumer boundary and diagnostic checks; flags={len(diag_names)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
