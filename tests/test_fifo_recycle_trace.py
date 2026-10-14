#!/usr/bin/env python3
"""Exercise actual SDK producer-side FIFO recycle tracing and acknowledgements."""

from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "external/ps3recomp/libs/video/cellGcmSys.c"
DIAG_BEGIN = "/* BEGIN GCM SLOW DRAIN DIAGNOSTIC */"
DIAG_END = "/* END GCM SLOW DRAIN DIAGNOSTIC */"
CALLBACK_BEGIN = "/* BEGIN GCM FIFO RECYCLE CALLBACK */"
CALLBACK_END = "/* END GCM FIFO RECYCLE CALLBACK */"


def marked(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError(f"expected unique source markers: {begin!r}, {end!r}")
    first = source.index(begin) + len(begin)
    last = source.index(end, first)
    return source[first:last]


PREFIX = r"""
#define WIN32_LEAN_AND_MEAN
#define GCM_CONTROL_GUEST_ADDR 0xC0002000u
#include <assert.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
static volatile LONG qpc_calls, frame_calls, wait_calls;
static LONGLONG fake_qpc = 1000;
static LARGE_INTEGER fake_qpf(void) { LARGE_INTEGER x; x.QuadPart=10000000; return x; }
static BOOL WINAPI fixture_qpf(LARGE_INTEGER* out) { *out=fake_qpf(); return TRUE; }
static BOOL WINAPI fixture_qpc(LARGE_INTEGER* out) {
    InterlockedIncrement(&qpc_calls); fake_qpc+=100; out->QuadPart=fake_qpc; return TRUE;
}
#define QueryPerformanceFrequency fixture_qpf
#define QueryPerformanceCounter fixture_qpc
static atomic_uint g_gcm_fifo_drained_ea;
static atomic_uint s_fifo_recycle_active;
static HANDLE s_gcm_kick_ev;
static u16 s_io_address_table[65536];
static const u32 ctx = 0x1000, begin_ea = 0x2000, current_ea = 0x3000;
static const u32 io_begin = 0x00502000, io_current = 0x00503000;
static u32 memory_begin, memory_current;
static u32 expected_after_put, stage_wait_count, stage_release_after;
static u32 current_release_after, begin_release_after;
static u32 vm_read32(u32 ea) {
    if (ea==ctx) return memory_begin;
    if (ea==ctx+8) return memory_current;
    abort();
}
static void vm_write32(u32 ea,u32 value) {
    if (ea==GCM_CONTROL_GUEST_ADDR) {
        if (value==io_current) { expected_after_put=current_ea; stage_release_after=current_release_after; }
        else if (value==io_begin) { expected_after_put=begin_ea; stage_release_after=begin_release_after; }
        else abort();
        stage_wait_count=0;
        if (!stage_release_after) atomic_store_explicit(&g_gcm_fifo_drained_ea,expected_after_put,memory_order_release);
        return;
    }
    if (ea==ctx+8) { memory_current=value; return; }
    if (ea==current_ea) return; /* emitted JUMP word */
    abort();
}
static u32 rsx_live_draw_get_frames(void) { InterlockedIncrement(&frame_calls); return 77; }
static int gcm_fifo_native_wait_enabled(void) { return 1; }
static void gcm_fifo_wait_for_publication(u32 snapshot,int native_wait) {
    (void)snapshot;
    if (!native_wait || !expected_after_put) abort();
    InterlockedIncrement(&wait_calls);
    ++stage_wait_count;
    /* The first polls model native timeout returns. Publish only after the
       configured number of waits, so the original loop must recheck the EA. */
    if (stage_wait_count>=stage_release_after)
        atomic_store_explicit(&g_gcm_fifo_drained_ea,expected_after_put,memory_order_release);
}
"""


MAIN = r"""
int main(int argc,char** argv) {
    if (argc<2) return 2;
    const int enabled=!strcmp(argv[1],"enabled");
    const int overflow=!strcmp(argv[1],"overflow");
    const int path_long=!strcmp(argv[1],"path_long");
    if (enabled || overflow || path_long) {
        if (argc!=3 || !getenv("GCM_SLOW_DRAIN_CSV")) return 3;
    } else {
        if (getenv("GCM_SLOW_DRAIN_CSV")) return 4;
    }
    if (path_long) {
        s_io_address_table[0]=5;
        memory_begin=begin_ea; memory_current=begin_ea;
        cellGcm_fifo_recycle(ctx);
        assert(qpc_calls==0 && frame_calls==0);
        assert(s_gcm_fifo_recycle_started==0 && s_gcm_fifo_recycle_count==0);
        cellGcm_fifo_slow_drain_dump();
        assert(strstr(s_gcm_fifo_recycle_path,".recycle.csv") == NULL);
        return 0;
    }
    if (overflow) {
        s_io_address_table[0]=5;
        memory_begin=begin_ea; memory_current=begin_ea;
        for (u32 i=0;i<GCM_FIFO_RECYCLE_TRACE_CAP+3u;++i)
            cellGcm_fifo_recycle(ctx);
        assert(s_gcm_fifo_recycle_started==GCM_FIFO_RECYCLE_TRACE_CAP+3u);
        assert(s_gcm_fifo_recycle_completed==GCM_FIFO_RECYCLE_TRACE_CAP+3u);
        assert(s_gcm_fifo_recycle_inflight==0);
        cellGcm_fifo_slow_drain_dump();
        return 0;
    }
    if (enabled) {
        /* Active trace records each early exit without changing guest writes. */
        cellGcm_fifo_recycle(0);
        memory_begin=begin_ea; memory_current=begin_ea;
        cellGcm_fifo_recycle(ctx);
        memory_begin=begin_ea; memory_current=current_ea;
        s_io_address_table[0]=0xFFFF;
        cellGcm_fifo_recycle(ctx);
        assert(!s_fifo_recycle_active);

        /* Both ack stages survive delayed (timeout-like) host waits. */
        s_io_address_table[0]=5;
        current_release_after=3; begin_release_after=2;
        expected_after_put=0; stage_wait_count=0;
        atomic_store_explicit(&g_gcm_fifo_drained_ea,0,memory_order_release);
        memory_begin=begin_ea; memory_current=current_ea;
        cellGcm_fifo_recycle(ctx);
        assert(memory_current==begin_ea && !s_fifo_recycle_active);
        assert(wait_calls==5);
        assert(qpc_calls>0 && frame_calls==4);
        cellGcm_fifo_slow_drain_dump();
        char recycle_path[MAX_PATH];
        if (snprintf(recycle_path,sizeof(recycle_path),"%s.recycle.csv",argv[2])<0) return 4;
        FILE* sentinel=fopen(recycle_path,"ab");
        if (!sentinel || fputs("#sentinel\n",sentinel)<0 || fclose(sentinel)!=0) return 5;
        cellGcm_fifo_slow_drain_dump();
        return 0;
    }
    s_io_address_table[0]=5;
    current_release_after=0; begin_release_after=0;
    memory_begin=begin_ea; memory_current=current_ea;
    cellGcm_fifo_recycle(ctx);
    assert(memory_current==begin_ea && !s_fifo_recycle_active);
    assert(qpc_calls==0 && frame_calls==0);
    assert(s_gcm_fifo_recycle_count==0);
    cellGcm_fifo_slow_drain_dump();
    assert(qpc_calls==0 && frame_calls==0);
    return 0;
}
"""


def source_text() -> str:
    return SOURCE.read_text(encoding="utf-8")


def make_fixture() -> str:
    source = source_text()
    diagnostic = marked(source, DIAG_BEGIN, DIAG_END)
    callback = marked(source, CALLBACK_BEGIN, CALLBACK_END)
    # The source-level SDK helpers are real; only memory and publication
    # services are controlled by PREFIX above.
    stubs = r"""
static void gcm_rsx_process_fifo_unlocked(gcm_slow_drain_event* event) { (void)event; }
"""
    return PREFIX + diagnostic + stubs + callback + MAIN


def run(exe: Path, mode: str, base: Path | None, *, env_extra: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    args = [str(exe), mode]
    env=os.environ.copy()
    if base is not None:
        args.append(str(base))
        env["GCM_SLOW_DRAIN_CSV"]=str(base)
    else:
        env.pop("GCM_SLOW_DRAIN_CSV",None)
    if env_extra:
        env.update(env_extra)
    result = subprocess.run(args, env=env, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f"{mode} failed ({result.returncode})\n{result.stderr}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=None)
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp",
                        help="SDK source root (defaults to external/ps3recomp)")
    parser.add_argument("--output-dir", type=Path,
                        default=Path("local/fifo-recycle-trace-test"))
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("clang-cl")
    if not compiler:
        parser.error("clang-cl not found; pass --compiler")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    c_path = args.output_dir / "fifo_recycle_trace_fixture.c"
    exe = args.output_dir / "fifo_recycle_trace_fixture.exe"
    global SOURCE
    SOURCE = args.sdk / "libs/video/cellGcmSys.c"
    c_path.write_text(make_fixture(), encoding="utf-8")
    cmd = [compiler, "/nologo", "/TC", "/std:c11", "/O1", "/W0",
           "/D_WIN32", str(c_path), f"/Fe:{exe}", f"/Fo:{exe.with_suffix('.obj')}"]
    subprocess.run(cmd, check=True)

    run(exe, "disabled", None)
    base = args.output_dir / "trace.csv"
    for path in (base, Path(str(base) + ".recycle.csv")):
        if path.exists(): path.unlink()
    run(exe, "enabled", base)
    recycle = Path(str(base) + ".recycle.csv")
    lines = recycle.read_text(encoding="utf-8").splitlines()
    if not lines or lines[-1]!="#sentinel":
        raise AssertionError("one-shot dump rewrote or appended the file")
    lines.pop()
    if not lines or lines[0] != "#qpc_frequency,10000000,recorded,4,overwritten,0,started,4,completed,4,inflight_at_dump,0":
        raise AssertionError(f"bad ring metadata: {lines[:1]}")
    if len(lines)<6: raise AssertionError("missing recycle rows")
    header=lines[1].split(",")
    rows=[dict(zip(header,line.split(","))) for line in lines[2:]]
    if [int(row["outcome"]) for row in rows] != [1,2,3,0]:
        raise AssertionError("early return/unmapped/success outcome order changed")
    done=rows[-1]
    if done["host_tid"]=="0" or done["renderer_frame"]!="77":
        raise AssertionError("missing producer identity");
    if (int(done["ack_current_expected_ea"],16)!=0x3000 or
        int(done["ack_current_initial_observed_ea"],16)!=0 or
        int(done["ack_current_observed_ea"],16)!=0x3000 or
        int(done["ack_current_wait_calls"])!=3 or
        int(done["ack_begin_expected_ea"],16)!=0x2000 or
        int(done["ack_begin_initial_observed_ea"],16)!=0x3000 or
        int(done["ack_begin_observed_ea"],16)!=0x2000 or
        int(done["ack_begin_wait_calls"])!=2 or
        done["ack_current_completed"]!="1" or done["ack_begin_completed"]!="1"):
        raise AssertionError(f"ack stages lost or completed early: {done}")
    if float(done["ack_current_ms"])<=0 or float(done["ack_begin_ms"])<=0:
        raise AssertionError("ack elapsed QPC fields are empty")

    overflow_base=args.output_dir / "overflow.csv"
    for path in (overflow_base,Path(str(overflow_base)+".recycle.csv")):
        if path.exists(): path.unlink()
    run(exe,"overflow",overflow_base)
    overflow_lines=Path(str(overflow_base)+".recycle.csv").read_text(encoding="utf-8").splitlines()
    if overflow_lines[0] != f"#qpc_frequency,10000000,recorded,16387,overwritten,3,started,16387,completed,16387,inflight_at_dump,0":
        raise AssertionError(f"overflow metadata/order failure: {overflow_lines[:1]}")
    overflow_rows=overflow_lines[2:]
    if len(overflow_rows)!=16384:
        raise AssertionError(f"expected 16384 retained rows, got {len(overflow_rows)}")
    sequences=[int(row.split(",",1)[0]) for row in overflow_rows]
    if sequences[0]!=4 or sequences[-1]!=16387 or sequences!=list(range(4,16388)):
        raise AssertionError("ring overflow did not preserve retained chronological order")

    too_long="C:\\"+"x"*4100
    long_result=run(exe,"path_long",args.output_dir / "path-long.csv",
                    env_extra={"GCM_SLOW_DRAIN_CSV":too_long})
    if "output path too long" not in long_result.stderr:
        raise AssertionError("overlong derived output path was not reported")
    print("FIFO recycle trace: disabled no-op, delayed ack stages, early exits, one-shot dump, 16384-row chronological retention, and path-length failure passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
