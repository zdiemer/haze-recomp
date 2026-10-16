#!/usr/bin/env python3
"""Exercise the actual bounded texture-diagnostic ring extracted from SDK."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


BEGIN = "/* BEGIN RSX_TEXTURE_DIAGNOSTIC_DEFERRED */"
END = "/* END RSX_TEXTURE_DIAGNOSTIC_DEFERRED */"


def extract(source: str) -> str:
    start = source.index(BEGIN) + len(BEGIN)
    end = source.index(END, start)
    return source[start:end]


def extract_function(source: str, signature: str) -> str:
    start = source.rindex(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise ValueError(f"unterminated function: {signature}")


PRELUDE = r"""
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { u32 location, offset, format, width, height, pitch; } rsx_dsp_texture;
static u32 g_ld_frames = 10;
static u64 g_ld_texture_cache_full, g_ld_texture_decode_fail, g_ld_texture_cache_evictions;
static struct { int ready; } g;
static unsigned writes, flushes;
static char output[131072];
static size_t output_size;
static int fixture_fprintf(FILE* ignored, const char* format, ...) {
    (void)ignored;
    ++writes;
    va_list args;
    va_start(args, format);
    int count = vsnprintf(output + output_size, sizeof(output) - output_size, format, args);
    va_end(args);
    assert(count >= 0 && (size_t)count < sizeof(output) - output_size);
    output_size += (size_t)count;
    return count;
}
static int fixture_fflush(FILE* ignored) { (void)ignored; ++flushes; return 0; }
#define fprintf fixture_fprintf
#define fflush fixture_fflush
#define stderr ((FILE*)1)
"""

EXIT_STUBS = r"""
static void ld_sampler_pages_dump(void) {}
typedef struct { u64 hits, misses, mutations, evictions, rejected; } rsx_fp_program_cache_stats;
static void* g_ld_fp_program_cache;
static rsx_fp_program_cache_stats rsx_fp_program_cache_statistics(const void* cache) {
    (void)cache;
    const rsx_fp_program_cache_stats stats = {0};
    return stats;
}
static void ld_report_counters(void) {}
static void ld_slow_flush_dump(void) {}
static void ld_texture_content_summary(void) {}
static void ld_slow_draw_dump(void) {}
static void ld_pull_summary(void) {}
static void ld_present_measure_dump(void) {}
static void ld_texture_watch_summary(void) {}
static void ld_geometry_cache_summary(void) {}
static void ld_vertex_worker_summary(void) {}
"""


TEST = r"""
int main(void) {
    rsx_dsp_texture t = { 7, 0x1234, 0x86, 256, 128, 512 };
    for (u32 i = 1; i <= 260; ++i) {
        g_ld_frames = 1000 + i;
        ld_texture_diag_record(LD_TEXTURE_DIAG_CACHE_FULL, i, &t, i % 128, 128, i * 2);
    }
    assert(writes == 0 && flushes == 0);
    assert(g_ld_texture_diag.records == 260);
    assert(g_ld_texture_diag.retained == LD_TEXTURE_DIAG_CAP);
    assert(g_ld_texture_diag.overwritten == 4);
    g.ready = 1;
    rsx_live_draw_dump_present_samples();
    /* One fragment-cache summary, one texture summary, and the retained ring. */
    assert(writes == LD_TEXTURE_DIAG_CAP + 2 && flushes == 1);
    assert(strstr(output, "[fp-plan] hits=0 misses=0 mutations=0 evictions=0 rejected=0") != NULL);
    assert(strstr(output, "records=260 retained=256 overwritten=4 capacity=256") != NULL);
    assert(strstr(output, "n=5 frame=1005 src=7:0x00001234") != NULL);
    assert(strstr(output, "n=260 frame=1260 src=7:0x00001234") != NULL);
    assert(strstr(output, "n=4 frame=1004 src=7:0x00001234") == NULL);
    unsigned saved_writes = writes;
    ld_texture_diag_dump();
    assert(writes == saved_writes && flushes == 1);
    puts("deferred texture diagnostic ring: bounded overwrite, event identity/order, no hot-path I/O, one-shot dump passed");
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=None)
    parser.add_argument("--source", type=Path,
                        default=Path("external/ps3recomp/libs/video/rsx_live_draw.c"))
    parser.add_argument("--output-dir", type=Path,
                        default=Path("local/texture-diagnostic-defer-test"))
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("clang-cl") or shutil.which("clang")
    if not compiler:
        parser.error("clang-cl/clang not found; pass --compiler")
    source = args.source.read_text(encoding="utf-8")
    helper = extract(source)
    exit_dump = extract_function(source, "void rsx_live_draw_dump_present_samples(void)")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source_path = args.output_dir / "texture_diag_fixture.c"
    exe_path = args.output_dir / "texture_diag_fixture.exe"
    source_path.write_text(PRELUDE + helper + EXIT_STUBS + exit_dump + TEST, encoding="utf-8")
    cmd = [compiler, "/nologo", "/W0", "/O1", "/TC", str(source_path), f"/Fe{exe_path}"]
    if "clang-cl" in Path(compiler).name.lower():
        obj_path = args.output_dir / "texture_diag_fixture.obj"
        cmd += [f"/Fo{obj_path}"]
    subprocess.run(cmd, check=True)
    subprocess.run([str(exe_path)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
