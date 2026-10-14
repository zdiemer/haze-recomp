"""Focused native regression fixture for the SDK slow FIFO drain diagnostic.

Extracts the live Windows diagnostic and wrapper from cellGcmSys.c, then links
them with a controllable walker and guest-memory stubs. It exercises real
Win32 locks, QPC, thread-cycle queries, and CSV output without game code.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DIAG_BEGIN = "/* BEGIN GCM SLOW DRAIN DIAGNOSTIC */"
DIAG_END = "/* END GCM SLOW DRAIN DIAGNOSTIC */"
WRAPPER_BEGIN = "/* BEGIN GCM SLOW DRAIN WRAPPER */"
WRAPPER_END = "/* END GCM SLOW DRAIN WRAPPER */"
KICK_BEGIN = "/* BEGIN GCM FIFO KICK WAIT RESULT */"
KICK_END = "/* END GCM FIFO KICK WAIT RESULT */"


def extract_marked(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1 or source.count(end) != 1:
        raise RuntimeError(f"expected unique extraction markers {begin!r} / {end!r}")
    start = source.index(begin)
    finish = source.index(end, start + len(begin))
    if finish <= start:
        raise RuntimeError(f"invalid extraction marker order for {begin!r}")
    return source[start:finish]


def fixture_source(diagnostic: str, wrapper: str, kick_wait: str) -> str:
    return f'''#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
#define GCM_CONTROL_GUEST_ADDR 0xC0002000u

static SRWLOCK s_gcm_fifo_lock = SRWLOCK_INIT;
static u32 s_gcm_context_ea;
static int s_gcm_syscall_mode;
static u32 s_fifo_getoff = 0x88u;
static u32 fixture_put = 0x1234u;
static volatile LONG guest_read_count;
static volatile LONG frame_query_count;
static volatile LONG walker_calls;
static volatile LONG walk_mode;
static DWORD fixture_wait_result;
static LONG fixture_wait_calls, fixture_sleep_calls;

static DWORD WINAPI fixture_wait(HANDLE event, DWORD timeout)
{{
    if (event != (HANDLE)(uintptr_t)0x1234 || timeout != 37) abort();
    InterlockedIncrement(&fixture_wait_calls);
    return fixture_wait_result;
}}
static VOID WINAPI fixture_sleep(DWORD ms)
{{
    if (ms != 37) abort();
    InterlockedIncrement(&fixture_sleep_calls);
}}
void* cellGcm_fifo_kick_event(void) {{ return (void*)(uintptr_t)0x1234; }}

u32 vm_read32(u64 address)
{{
    if (address != GCM_CONTROL_GUEST_ADDR) abort();
    InterlockedIncrement(&guest_read_count);
    return fixture_put;
}}

u32 rsx_live_draw_get_frames(void)
{{
    InterlockedIncrement(&frame_query_count);
    return 77u;
}}

{diagnostic}

static void gcm_rsx_process_fifo_unlocked(gcm_slow_drain_event* diagnostic)
{{
    InterlockedIncrement(&walker_calls);
    gcm_slow_drain_phase_begin(diagnostic);
    if (InterlockedCompareExchange(&walk_mode, 0, 0) == 6) {{
        Sleep(12);
        gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_SETUP);
        gcm_slow_drain_set_reason(diagnostic,"no-context");
        return;
    }}
    const LONG mode=InterlockedCompareExchange(&walk_mode, 0, 0);
    if (mode == 1 || mode == 7) Sleep(12);
    gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_SETUP);
    if (InterlockedCompareExchange(&walk_mode, 0, 0) == 2) Sleep(12);
    gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_WALKER);
    gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_POSTWALK);
    if (InterlockedCompareExchange(&walk_mode, 0, 0) == 4) Sleep(12);
    gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_RECYCLE);
    gcm_slow_drain_set_reason(diagnostic,"caught-up");
    gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_POST_RECYCLE);
    if (InterlockedCompareExchange(&walk_mode, 0, 0) == 3) Sleep(12);
    gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_DRAINED_EA_PUBLISH);
    if (InterlockedCompareExchange(&walk_mode, 0, 0) == 8) Sleep(12);
    gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_GET_WRITE);
    if (InterlockedCompareExchange(&walk_mode, 0, 0) == 5) Sleep(12);
    gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_REF_LOCK_WAIT);
    gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_REF_PUBLISH);
    s_fifo_getoff += 4u;
}}

{wrapper}

#define WaitForSingleObject fixture_wait
#define Sleep fixture_sleep
{kick_wait}
#undef WaitForSingleObject
#undef Sleep

static DWORD WINAPI hold_fifo_lock(LPVOID unused)
{{
    HANDLE ready = (HANDLE)unused;
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    SetEvent(ready);
    Sleep(12);
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
    return 0;
}}

int main(int argc, char** argv)
{{
    if (argc != 2) return 2;
    const char* mode = argv[1];
    if (!strncmp(mode, "kick-", 5)) {{
        if (!strcmp(mode, "kick-signaled")) fixture_wait_result=WAIT_OBJECT_0;
        else if (!strcmp(mode, "kick-timeout")) fixture_wait_result=WAIT_TIMEOUT;
        else if (!strcmp(mode, "kick-failed")) fixture_wait_result=WAIT_FAILED;
        else if (!strcmp(mode, "kick-other")) fixture_wait_result=WAIT_ABANDONED;
        else if (strcmp(mode, "kick-fallback")) return 16;
        const unsigned result=cellGcm_fifo_kick_wait_result(37);
        const unsigned expected=!strcmp(mode,"kick-signaled") ? 0u :
            !strcmp(mode,"kick-timeout") ? 1u :
            !strcmp(mode,"kick-fallback") ? 2u : 3u;
        if (result != expected) return 17;
        if (expected==2) {{
            if (fixture_wait_calls || fixture_sleep_calls!=1) return 18;
        }} else if (fixture_wait_calls!=1 || fixture_sleep_calls) return 19;
        return 0;
    }}
    const int enabled = strcmp(mode, "disabled") != 0;
    s_gcm_context_ea = (strcmp(mode, "invalid-context") == 0) ? 0u : 0x4000u;
    s_gcm_syscall_mode = 0;
    InterlockedExchange(&walk_mode,
        !strcmp(mode,"body") || !strcmp(mode,"invalid-context") ? 1 :
        !strcmp(mode,"reference-lock") ? 5 :
        !strcmp(mode,"drained-ea") ? 3 :
        !strcmp(mode,"publish") ? 8 :
        !strcmp(mode,"recycle") ? 4 :
        !strcmp(mode,"early-return") ? 6 :
        !strcmp(mode,"get-equals-put") ? 7 : 0);
    if (!strcmp(mode,"get-equals-put")) fixture_put=0x88u;

    if (strcmp(mode, "lock") == 0) {{
        HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!ready) return 3;
        HANDLE holder = CreateThread(NULL, 0, hold_fifo_lock, ready, 0, NULL);
        if (!holder) return 4;
        if (WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0) return 5;
        /* The holder owns the SRW lock before the measured wrapper enters. */
        cellGcm_rsx_process_fifo();
        WaitForSingleObject(holder, 5000);
        CloseHandle(holder);
        CloseHandle(ready);
    }} else {{
        if (enabled) {{
            char path[MAX_PATH];
            if (!GetEnvironmentVariableA("GCM_SLOW_DRAIN_CSV", path, sizeof(path))) return 6;
        }} else if (getenv("GCM_SLOW_DRAIN_CSV")) return 7;
        cellGcm_rsx_process_fifo();
    }}

    if (walker_calls != 1 ||
        s_fifo_getoff != (!strcmp(mode,"early-return") ? 0x88u : 0x8Cu)) return 8;
    if (strcmp(mode, "disabled") == 0) {{
        if (guest_read_count || frame_query_count) return 9;
        return 0;
    }}
    if (frame_query_count != 1) return 10;
    if (strcmp(mode, "invalid-context") == 0) {{
        if (guest_read_count != 0) return 11;
    }} else if (guest_read_count != 2) return 12;

    /* A second dump must not append or rewrite another copy of the events. */
    cellGcm_fifo_slow_drain_dump();
    const char* csv_path = getenv("GCM_SLOW_DRAIN_CSV");
    if (!csv_path) return 13;
    FILE* sentinel_file = fopen(csv_path, "ab");
    if (!sentinel_file) return 14;
    const int sentinel_written = fputs("# second-dump-sentinel\\n", sentinel_file) >= 0;
    const int sentinel_closed = fclose(sentinel_file) == 0;
    if (!sentinel_written || !sentinel_closed) return 15;
    cellGcm_fifo_slow_drain_dump();
    return 0;
}}
'''


def compiler_command(compiler: str, source: Path, exe: Path) -> list[str]:
    name = Path(compiler).name.lower()
    if name in ("clang-cl", "clang-cl.exe"):
        return [compiler, "/nologo", "/TC", "/std:c11", "/O2", "/W4",
                "/wd4100", "/D_WIN32", str(source), f"/Fe:{exe}",
                f"/Fo:{exe.with_suffix('.obj')}"]
    return [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-D_WIN32",
            str(source), "-o", str(exe)]


def read_csv(path: Path) -> tuple[dict[str, str], list[str], list[dict[str, str]]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[-1] != "# second-dump-sentinel":
        raise AssertionError(f"second dump rewrote or appended to the CSV: {path}")
    lines.pop()
    if len(lines) < 2 or not lines[0].startswith("#qpc_frequency,"):
        raise AssertionError(f"missing slow-drain metadata in {path}")
    meta_values = lines[0][1:].split(",")
    meta = dict(zip(meta_values[0::2], meta_values[1::2]))
    header = lines[1].split(",")
    rows = [dict(zip(header, line.split(","))) for line in lines[2:]]
    return meta, header, rows


def run_case(exe: Path, mode: str, csv_path: Path | None) -> None:
    env = os.environ.copy()
    env.pop("GCM_SLOW_DRAIN_CSV", None)
    if csv_path is not None:
        env["GCM_SLOW_DRAIN_CSV"] = str(csv_path)
    result = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(
            f"{mode} failed ({result.returncode})\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if csv_path is None:
        return
    meta, header, rows = read_csv(csv_path)
    if int(meta.get("recorded", "-1")) != 1 or int(meta.get("overwritten", "-1")) != 0:
        raise AssertionError(f"unexpected event counts: {meta}")
    if len(rows) != 1:
        raise AssertionError(f"expected one retained event, got {len(rows)}")
    row = rows[0]
    if row["renderer_frame"] != "77" or row["get_before"] != "0x00000088":
        raise AssertionError(f"bad FIFO/frame snapshots: {row}")
    expected_get_after = "0x00000088" if mode == "early-return" else "0x0000008C"
    if row["get_after"] != expected_get_after:
        raise AssertionError(f"bad post-drain get snapshot: {row}")
    if mode == "invalid-context":
        if row["context_valid"] != "0" or row["put_before"] != "0x00000000" or row["put_after"] != "0x00000000":
            raise AssertionError(f"invalid context should suppress PUT reads: {row}")
    else:
        expected_put = "0x00000088" if mode == "get-equals-put" else "0x00001234"
        if row["context_valid"] != "1" or row["put_before"] != expected_put or row["put_after"] != expected_put:
            raise AssertionError(f"valid context PUT snapshots missing: {row}")
    lock_ms = float(row["lock_wait_ms"])
    body_ms = float(row["body_ms"])
    if mode == "lock" and not (lock_ms >= 5.0 and body_ms < 10.0):
        raise AssertionError(f"held lock not separated from body: lock={lock_ms} body={body_ms}")
    if mode in ("body", "invalid-context") and not (body_ms >= 5.0 and lock_ms < 10.0):
        raise AssertionError(f"sleeping walker not separated from lock wait: lock={lock_ms} body={body_ms}")
    if not row["thread_cycles_valid"] in ("0", "1"):
        raise AssertionError(f"invalid cycle-query validity field: {row}")
    expected_mask = 1 if mode == "early-return" else 511
    if int(row["phase_mask"]) != expected_mask:
        raise AssertionError(f"phase instrumentation incomplete: {row}")
    phase_fields = ("prewalk_setup_ticks", "walker_ticks", "postwalk_ticks",
                    "recycle_ticks", "post_recycle_ticks", "drained_ea_publish_ticks",
                    "get_write_ticks",
                    "reference_lock_wait_ticks", "reference_publish_ticks")
    body_ticks = int(row["qpc_body_end"]) - int(row["qpc_lock_acquired"])
    phase_sum = sum(int(row[field]) for field in phase_fields)
    if int(row["phase_unaccounted_ticks"]) != body_ticks - phase_sum:
        raise AssertionError(f"inner phases do not reconcile with body: {row}")
    targeted = {"body": "prewalk_setup_ticks", "publish": "get_write_ticks",
                "drained-ea": "drained_ea_publish_ticks",
                "recycle": "recycle_ticks", "reference-lock": "reference_lock_wait_ticks",
                "early-return": "prewalk_setup_ticks"}
    if mode in targeted and float(row[targeted[mode]]) < 5.0:
        raise AssertionError(f"simulated {mode} stall was not attributed: {row}")
    expected_reason = "no-context" if mode == "early-return" else "caught-up"
    if row["stopped_reason"] != expected_reason:
        raise AssertionError(f"unexpected stopped reason: {row}")
    if header[-1] != "stopped_reason":
        raise AssertionError(f"unexpected CSV schema: {header}")


def run_kick_case(exe: Path, mode: str) -> None:
    env = os.environ.copy()
    env.pop("GCM_SLOW_DRAIN_CSV", None)
    if mode == "kick-fallback":
        env["GCM_FIFO_KICK"] = "0"
    else:
        env.pop("GCM_FIFO_KICK", None)
    result = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(
            f"{mode} failed ({result.returncode})\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, help="clang-cl executable")
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "local/test-fifo-slow-drain")
    args = parser.parse_args()
    source_path = args.sdk / "libs/video/cellGcmSys.c"
    source_bytes = source_path.read_bytes()
    source = source_bytes.decode("utf-8")
    diagnostic = extract_marked(source, DIAG_BEGIN, DIAG_END)
    wrapper = extract_marked(source, WRAPPER_BEGIN, WRAPPER_END)
    kick_wait = extract_marked(source, KICK_BEGIN, KICK_END)
    walker_signature = "static void gcm_rsx_process_fifo_unlocked(gcm_slow_drain_event* diagnostic)"
    declaration = source.index(walker_signature)
    walker_start = source.index(walker_signature, declaration + len(walker_signature))
    walker_end = source.index("/* FIFO command-buffer-full callback body", walker_start)
    walker = source[walker_start:walker_end]
    landmarks = (
        "gcm_slow_drain_phase_begin(diagnostic);",
        "gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_SETUP);",
        "while (s_fifo_getoff != put && budget-- > 0)",
        "gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_WALKER);",
        "gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_POSTWALK);",
        "gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_RECYCLE);",
        "gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_POST_RECYCLE);",
        "gcm_fifo_publish_drained_ea(gcm_io2ea(s_fifo_getoff));",
        "gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_DRAINED_EA_PUBLISH);",
        "gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_GET_WRITE);",
        "AcquireSRWLockExclusive(&s_ref_pub_lock);",
        "gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_REF_LOCK_WAIT);",
        "gcm_slow_drain_phase_end(diagnostic,GCM_SLOW_PHASE_REF_PUBLISH);",
    )
    positions = [walker.find(landmark) for landmark in landmarks]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise RuntimeError("slow-drain phase callsites are missing or out of order")
    if "context_valid ? vm_read32(GCM_CONTROL_GUEST_ADDR) : 0" not in wrapper:
        raise RuntimeError("wrapper no longer guards guest PUT reads by context validity")
    if "AcquireSRWLockExclusive(&s_gcm_fifo_lock)" not in wrapper:
        raise RuntimeError("wrapper no longer acquires the production FIFO lock")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    fixture = args.output_dir / "fifo_slow_drain_fixture.c"
    fixture.write_text(fixture_source(diagnostic, wrapper, kick_wait), encoding="utf-8")
    exe = args.output_dir / "fifo_slow_drain_fixture.exe"
    completed = subprocess.run(compiler_command(args.compiler, fixture, exe), capture_output=True, text=True)
    if completed.returncode:
        raise RuntimeError(f"compile failed ({completed.returncode})\n{completed.stdout}\n{completed.stderr}")
    print(f"SDK source sha256: {hashlib.sha256(source_bytes).hexdigest()}")
    print(f"fixture: {fixture}")
    with tempfile.TemporaryDirectory(prefix="haze-fifo-slow-drain-") as temp:
        temp_path = Path(temp)
        run_case(exe, "disabled", None)
        print("PASS disabled diagnostic avoids guest/frame reads")
        for mode in ("invalid-context", "lock", "body", "drained-ea", "publish", "recycle",
                     "reference-lock", "get-equals-put", "early-return"):
            csv_path = temp_path / f"{mode}.csv"
            run_case(exe, mode, csv_path)
            print(f"PASS {mode} attribution and retained/idempotent CSV")
        for mode in ("kick-signaled", "kick-timeout", "kick-failed", "kick-other", "kick-fallback"):
            run_kick_case(exe, mode)
            print(f"PASS kick wait result: {mode}")
    print("15 focused FIFO slow-drain/kick-wait cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
