#!/usr/bin/env python3
"""Compile the real VP-burst mode header and shutdown exporter with fake clocks/logging."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise ValueError(f"missing function signature: {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise ValueError(f"missing function body: {signature}")
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise ValueError(f"unterminated function body: {signature}")


FIXTURE = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* env_burst;
static const char* env_file;
static const char* env_profile;
static uint64_t fake_now;
static uint64_t fake_qpc;
static unsigned tick_calls, qpc_calls, log_calls, flush_calls;
static unsigned lock_acquires, lock_releases;
static char log_text[4096];
static size_t log_used;

static const char* test_getenv(const char* name) {
    if (!strcmp(name, "RSX_VP_BURST")) return env_burst;
    if (!strcmp(name, "RSX_VP_BURST_FILE")) return env_file;
    if (!strcmp(name, "RSX_VP_BURST_PROFILE")) return env_profile;
    return NULL;
}
static ULONGLONG test_GetTickCount64(void) { ++tick_calls; return fake_now; }
static BOOL test_QueryPerformanceCounter(LARGE_INTEGER* value) {
    ++qpc_calls; value->QuadPart = (LONGLONG)++fake_qpc; return TRUE;
}
static int test_fprintf(FILE* stream, const char* format, ...) {
    (void)stream;
    ++log_calls;
    /* Model an expensive synchronous diagnostic write in simulated ms. */
    fake_now += 1000;
    va_list args; va_start(args, format);
    const int count = _vsnprintf_s(log_text + log_used, sizeof(log_text) - log_used,
                                   _TRUNCATE, format, args);
    va_end(args);
    if (count > 0) log_used += (size_t)count;
    return count;
}
static int test_fflush(FILE* stream) { (void)stream; ++flush_calls; return 0; }
static void test_AcquireSRWLockExclusive(SRWLOCK* lock) {
    (void)lock; ++lock_acquires;
}
static void test_ReleaseSRWLockExclusive(SRWLOCK* lock) {
    (void)lock; ++lock_releases;
}
#define getenv test_getenv
#define GetTickCount64 test_GetTickCount64
#define QueryPerformanceCounter test_QueryPerformanceCounter
#define fprintf test_fprintf
#define fflush test_fflush
#define AcquireSRWLockExclusive test_AcquireSRWLockExclusive
#define ReleaseSRWLockExclusive test_ReleaseSRWLockExclusive
#include "gcm_vp_burst_mode.h"
static SRWLOCK s_gcm_fifo_lock;
/* EXTRACTED_EXPORT */

int main(int argc, char** argv) {
    if (argc < 2) return 10;
    if (!strcmp(argv[1], "fixed")) {
        env_burst = "1";
        gcm_vp_burst_poll();
        if (!s_vp_burst_mode.enabled || log_calls != 3 || tick_calls || qpc_calls != 1)
            return 11;
        gcm_vp_burst_record(0, 1);
        gcm_vp_burst_record(0, 0);
        for (unsigned i = 0; i < 4; ++i) {
            fake_now += 3000;
            gcm_vp_burst_poll();
        }
        if (log_calls != 3 || tick_calls || s_vp_burst_mode.native != 1 ||
            s_vp_burst_mode.original != 1) return 12;
        cellGcm_vp_burst_dump();
        cellGcm_vp_burst_dump();
        if (log_calls != 4 || flush_calls != 1 || lock_acquires != 2 ||
            lock_releases != 2 || !s_vp_burst_mode.final_reported ||
            !strstr(log_text, "[rsx-vp-burst-mode] enabled=1") ||
            !strstr(log_text, "[rsx-vp-burst-final]") ||
            !strstr(log_text, "original=1 native=1 rejected=1")) return 13;
        return 0;
    }
    if (!strcmp(argv[1], "profile")) {
        env_profile = "1";
        gcm_vp_burst_poll();
        if (!s_vp_burst_mode.active || s_vp_burst_mode.enabled ||
            log_calls != 3 || tick_calls != 1) return 20;
        gcm_vp_burst_record(0, 1);
        fake_now = 2501;
        gcm_vp_burst_poll();
        if (log_calls != 4 || tick_calls != 2 ||
            !strstr(log_text, "[rsx-vp-burst-counts]")) return 21;
        cellGcm_vp_burst_dump();
        if (log_calls != 5 || flush_calls != 1 ||
            !strstr(log_text, "[rsx-vp-burst-final]")) return 22;
        return 0;
    }
    if (!strcmp(argv[1], "inactive")) {
        gcm_vp_burst_poll();
        cellGcm_vp_burst_dump();
        if (log_calls || tick_calls || qpc_calls || s_vp_burst_mode.active)
            return 30;
        return 0;
    }
    if (!strcmp(argv[1], "file")) {
        if (argc != 3) return 31;
        env_file = argv[2];
        FILE* control = fopen(env_file, "w");
        if (!control) return 32;
        fputs("1\n", control); fclose(control);
        gcm_vp_burst_poll();
        if (!s_vp_burst_mode.enabled || log_calls != 3 || tick_calls != 1)
            return 33;
        gcm_vp_burst_record(0, 1);
        fake_now += 3000;
        gcm_vp_burst_poll();
        if (log_calls != 4 || tick_calls != 2) return 34;
        control = fopen(env_file, "w");
        if (!control) return 35;
        fputs("0\n", control); fclose(control);
        fake_now += 3000;
        gcm_vp_burst_poll();
        if (s_vp_burst_mode.enabled || log_calls != 7 || tick_calls != 3 ||
            s_vp_burst_mode.native || s_vp_burst_mode.original) return 36;
        gcm_vp_burst_record(0, 0);
        cellGcm_vp_burst_dump(); cellGcm_vp_burst_dump();
        if (log_calls != 8 || flush_calls != 1 ||
            !strstr(log_text, "enabled=0 original=1 native=0 rejected=0")) return 37;
        return 0;
    }
    return 40;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, help="clang-cl executable")
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    cell_source = (sdk / "libs/video/cellGcmSys.c").read_text(encoding="utf-8")
    export = extract_function(cell_source, "void cellGcm_vp_burst_dump(void)")
    if "gcm_vp_burst_final_report(gcm_vp_burst_clock())" not in export:
        raise AssertionError("shutdown export does not use the mode final-report helper")
    if args.output_dir:
        out = args.output_dir.resolve()
        out.mkdir(parents=True, exist_ok=True)
        context = tempfile.TemporaryDirectory(dir=out)
    else:
        context = tempfile.TemporaryDirectory()
    with context as temp:
        temp_path = Path(temp)
        source = temp_path / "vp_burst_reporting.c"
        exe = temp_path / "vp_burst_reporting.exe"
        fixture = FIXTURE.replace("/* EXTRACTED_EXPORT */", export)
        source.write_text(fixture, encoding="utf-8")
        command = [args.compiler, "/nologo", "/std:c17", "/W4", "/WX", "/O2",
                   f"/I{sdk / 'libs/video'}", str(source), f"/Fe:{exe}",
                   f"/Fo:{temp_path / 'vp_burst_reporting.obj'}"]
        subprocess.run(command, check=True)
        for scenario in ("fixed", "profile", "inactive"):
            subprocess.run([str(exe), scenario], check=True)
            print(f"PASS VP-burst reporting {scenario}")
        subprocess.run([str(exe), "file", str(temp_path / 'control.txt')], check=True)
        print("PASS VP-burst reporting file-control transitions and final totals")
    print("PASS fixed mode has no periodic count logging or poll clock; shutdown final totals are once-only")


if __name__ == "__main__":
    main()
