"""Native fixture for the extracted ordinary-build RSX flush diagnostic."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DIAG_BEGIN = "/* BEGIN RSX SLOW FLUSH DIAGNOSTIC */"
DIAG_END = "/* END RSX SLOW FLUSH DIAGNOSTIC */"
WRAPPER_BEGIN = "/* BEGIN RSX SLOW FLUSH WRAPPER */"
WRAPPER_END = "/* END RSX SLOW FLUSH WRAPPER */"


def extract(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise RuntimeError(f"expected unique source markers {start!r} / {end!r}")
    begin = source.index(start)
    finish = source.index(end, begin + len(start))
    if finish <= begin:
        raise RuntimeError(f"invalid marker order for {start!r}")
    return source[begin:finish]


def fixture_source(diagnostic: str, wrapper: str) -> str:
    return f'''#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef enum {{ FLUSH_TEST = 0 }} ld_flush_reason;
#define FAILED(hr) ((HRESULT)(hr) < 0)

static volatile LONG qpc_calls, cycle_calls, close_calls, execute_calls;
static volatile LONG signal_calls, completed_calls, set_event_calls, wait_calls;
static volatile LONG info_calls, release_calls, alloc_reset_calls, list_reset_calls;
static volatile LONG dred_calls;
static volatile LONG last_error_calls;
static u64 fake_qpc;
static DWORD fake_last_error;
static char call_order[64];
static LONG call_order_size;
static HRESULT close_result, signal_result, event_result;
static HRESULT alloc_reset_result, list_reset_result;
static u64 completed_result;
static DWORD wait_result = WAIT_OBJECT_0;
static void note_call(char code)
{{ LONG index = InterlockedIncrement(&call_order_size) - 1; if (index < 63) call_order[index] = code; }}

static BOOL WINAPI test_qpf(LARGE_INTEGER* value)
{{ value->QuadPart = 1000000; return TRUE; }}
static BOOL WINAPI test_qpc(LARGE_INTEGER* value)
{{ InterlockedIncrement(&qpc_calls); fake_last_error = 456; fake_qpc += 1000; value->QuadPart = fake_qpc; return TRUE; }}
static BOOL WINAPI test_cycles(HANDLE thread, PULONG64 cycles)
{{ (void)thread; InterlockedIncrement(&cycle_calls); *cycles = (u64)cycle_calls * 100; return TRUE; }}
static DWORD WINAPI test_wait(HANDLE event, DWORD timeout)
{{ (void)event; if (timeout != INFINITE) abort(); note_call('W'); InterlockedIncrement(&wait_calls); if (wait_result == WAIT_FAILED) fake_last_error = 123; return wait_result; }}
static DWORD WINAPI test_last_error(void)
{{ note_call('e'); InterlockedIncrement(&last_error_calls); return fake_last_error; }}
#define QueryPerformanceFrequency test_qpf
#define QueryPerformanceCounter test_qpc
#define QueryThreadCycleTime test_cycles
#define WaitForSingleObject test_wait
#define GetLastError test_last_error

typedef struct TestList TestList;
typedef struct TestQueue TestQueue;
typedef struct TestFence TestFence;
typedef struct TestAlloc TestAlloc;
typedef struct TestResource TestResource;
typedef TestList ID3D12CommandList;
typedef struct {{
    HRESULT (STDMETHODCALLTYPE *Close)(TestList*);
    HRESULT (STDMETHODCALLTYPE *Reset)(TestList*, TestAlloc*, void*);
}} TestListVTable;
struct TestList {{ TestListVTable* lpVtbl; }};
typedef struct {{
    void (STDMETHODCALLTYPE *ExecuteCommandLists)(TestQueue*, UINT, ID3D12CommandList**);
    HRESULT (STDMETHODCALLTYPE *Signal)(TestQueue*, TestFence*, u64);
}} TestQueueVTable;
struct TestQueue {{ TestQueueVTable* lpVtbl; }};
typedef struct {{
    u64 (STDMETHODCALLTYPE *GetCompletedValue)(TestFence*);
    HRESULT (STDMETHODCALLTYPE *SetEventOnCompletion)(TestFence*, u64, HANDLE);
}} TestFenceVTable;
struct TestFence {{ TestFenceVTable* lpVtbl; }};
typedef struct {{ HRESULT (STDMETHODCALLTYPE *Reset)(TestAlloc*); }} TestAllocVTable;
struct TestAlloc {{ TestAllocVTable* lpVtbl; }};
typedef struct {{ ULONG (STDMETHODCALLTYPE *Release)(TestResource*); }} TestResourceVTable;
struct TestResource {{ TestResourceVTable* lpVtbl; }};

static HRESULT STDMETHODCALLTYPE mock_close(TestList* self)
{{ (void)self; note_call('C'); InterlockedIncrement(&close_calls); return close_result; }}
static HRESULT STDMETHODCALLTYPE mock_list_reset(TestList* self, TestAlloc* alloc, void* init)
{{ (void)self; (void)alloc; (void)init; note_call('L'); InterlockedIncrement(&list_reset_calls); return list_reset_result; }}
static void STDMETHODCALLTYPE mock_execute(TestQueue* self, UINT count, ID3D12CommandList** lists)
{{ (void)self; if (count != 1 || !lists) abort(); note_call('X'); InterlockedIncrement(&execute_calls); }}
static HRESULT STDMETHODCALLTYPE mock_signal(TestQueue* self, TestFence* fence, u64 value)
{{ (void)self; (void)fence; (void)value; note_call('S'); InterlockedIncrement(&signal_calls); return signal_result; }}
static u64 STDMETHODCALLTYPE mock_completed(TestFence* self)
{{ (void)self; note_call('G'); InterlockedIncrement(&completed_calls); return completed_result; }}
static HRESULT STDMETHODCALLTYPE mock_set_event(TestFence* self, u64 value, HANDLE event)
{{ (void)self; (void)value; (void)event; note_call('E'); InterlockedIncrement(&set_event_calls); return event_result; }}
static HRESULT STDMETHODCALLTYPE mock_alloc_reset(TestAlloc* self)
{{ (void)self; note_call('A'); InterlockedIncrement(&alloc_reset_calls); return alloc_reset_result; }}
static ULONG STDMETHODCALLTYPE mock_release(TestResource* self)
{{ (void)self; note_call('R'); InterlockedIncrement(&release_calls); return 1; }}

static TestListVTable list_vtable = {{mock_close, mock_list_reset}};
static TestQueueVTable queue_vtable = {{mock_execute, mock_signal}};
static TestFenceVTable fence_vtable = {{mock_completed, mock_set_event}};
static TestAllocVTable alloc_vtable = {{mock_alloc_reset}};
static TestResourceVTable resource_vtable = {{mock_release}};
static TestList list_object = {{&list_vtable}};
static TestQueue queue_object = {{&queue_vtable}};
static TestFence fence_object = {{&fence_vtable}};
static TestAlloc alloc_object = {{&alloc_vtable}};
static TestResource resource_object = {{&resource_vtable}};
static struct {{
    TestList* list; TestQueue* queue; TestFence* fence; HANDLE fence_event;
    u64 fence_value; u32 ready; TestAlloc* alloc;
    u32 n_retired_textures; TestResource* retired_textures[4]; u32 upload_used;
}} g;
static u32 g_ld_frames = 19;

static void ld_drain_info_queue(const char* where)
{{ if (strcmp(where, "flush") != 0) abort(); note_call('I'); InterlockedIncrement(&info_calls); }}
static void ld_dump_dred(const char* where, HRESULT hr)
{{ (void)where; (void)hr; note_call('D'); InterlockedIncrement(&dred_calls); }}

{diagnostic}

{wrapper}

static void reset_case(void)
{{
    close_result = signal_result = event_result = 0;
    alloc_reset_result = list_reset_result = 0;
    completed_result = 0;
    wait_result = WAIT_OBJECT_0;
    fake_qpc = 0;
    call_order_size = 0;
    memset(call_order, 0, sizeof(call_order));
    g.list = &list_object; g.queue = &queue_object; g.fence = &fence_object;
    g.fence_event = (HANDLE)1; g.fence_value = 10; g.ready = 1;
    g.alloc = &alloc_object; g.n_retired_textures = 1;
    g.retired_textures[0] = &resource_object; g.upload_used = 41;
}}

int main(int argc, char** argv)
{{
    if (argc != 2) return 2;
    reset_case();
    const char* mode = argv[1];
    if (!strcmp(mode, "close_fail")) close_result = (HRESULT)0x80004005u;
    if (!strcmp(mode, "signal_fail")) signal_result = (HRESULT)0x80004005u;
    if (!strcmp(mode, "event_fail")) event_result = (HRESULT)0x80004005u;
    if (!strcmp(mode, "no_wait")) completed_result = 11;
    if (!strcmp(mode, "wait_fail")) wait_result = WAIT_FAILED;
    if (!strcmp(mode, "reset_fail")) {{
        alloc_reset_result = (HRESULT)0x80004005u;
        list_reset_result = (HRESULT)0x80004005u;
    }}
    if (!strcmp(mode, "disabled")) {{
        _putenv("RSX_SLOW_FLUSH_CSV=");
        _putenv("RSX_SLOW_FLUSH_THRESHOLD_US=");
    }} else _putenv("RSX_SLOW_FLUSH_THRESHOLD_US=1");

    ld_flush(FLUSH_TEST);
    const char* expected_order = !strcmp(mode, "close_fail") ? "CD" :
        !strcmp(mode, "signal_fail") ? "CXSD" :
        !strcmp(mode, "event_fail") ? "CXSGED" :
        !strcmp(mode, "wait_fail") ? "CXSGEWeIRAL" :
        !strcmp(mode, "no_wait") ? "CXSGIRAL" : "CXSGEWIRAL";
    if (strcmp(call_order, expected_order) != 0) {{ fprintf(stderr, "order mismatch: %s expected %s\\n", call_order, expected_order); return 12; }}
    if (strcmp(mode, "disabled") == 0) {{
        if (qpc_calls || cycle_calls) return 3;
        if (close_calls != 1 || execute_calls != 1 || signal_calls != 1 ||
            completed_calls != 1 || set_event_calls != 1 || wait_calls != 1 ||
            info_calls != 1 || release_calls != 1 || alloc_reset_calls != 1 ||
            list_reset_calls != 1) return 4;
        return 0;
    }}
    if (!getenv("RSX_SLOW_FLUSH_CSV")) return 5;
    ld_slow_flush_dump();
    if (!strcmp(mode, "close_fail") && (execute_calls || signal_calls || dred_calls != 1)) return 6;
    if (!strcmp(mode, "signal_fail") && (completed_calls || set_event_calls || wait_calls || dred_calls != 1)) return 7;
    if (!strcmp(mode, "event_fail") && (wait_calls || info_calls || release_calls || dred_calls != 1)) return 8;
    if (!strcmp(mode, "no_wait") && (set_event_calls || wait_calls)) return 9;
    if (!strcmp(mode, "wait") && (!wait_calls || !info_calls || release_calls != 1)) return 10;
    if (!strcmp(mode, "reset_fail") && (alloc_reset_calls != 1 || list_reset_calls != 1 || !release_calls)) return 11;
    return 0;
}}
'''


def compiler_command(compiler: str, source: Path, exe: Path) -> list[str]:
    name = Path(compiler).name.lower()
    if name in ("clang-cl", "clang-cl.exe"):
        return [compiler, "/nologo", "/TC", "/std:c11", "/O2", "/W4",
                "/wd4100", "/D_WIN32", str(source), f"/Fe:{exe}", f"/Fo:{exe.with_suffix('.obj')}"]
    return [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-D_WIN32",
            str(source), "-o", str(exe)]


def read_csv(path: Path) -> tuple[dict[str, str], list[str], dict[str, str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if len(lines) != 3:
        raise AssertionError(f"expected metadata, header, and one row, got {len(lines)} lines")
    meta_values = lines[0][1:].split(",")
    meta = dict(zip(meta_values[0::2], meta_values[1::2]))
    header = lines[1].split(",")
    values = lines[2].split(",")
    if len(header) != 33 or len(values) != len(header):
        raise AssertionError(f"CSV shape mismatch: header={len(header)} row={len(values)}")
    return meta, header, dict(zip(header, values))


def run_case(exe: Path, mode: str, csv_path: Path | None) -> dict[str, str] | None:
    env = os.environ.copy()
    env.pop("RSX_SLOW_FLUSH_CSV", None)
    env.pop("RSX_SLOW_FLUSH_THRESHOLD_US", None)
    if csv_path is not None:
        env["RSX_SLOW_FLUSH_CSV"] = str(csv_path)
    result = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f"{mode} failed ({result.returncode})\n{result.stdout}\n{result.stderr}")
    if csv_path is None:
        return None
    meta, header, row = read_csv(csv_path)
    if meta.get("retained") != "1" or meta.get("overwritten") != "0":
        raise AssertionError(f"unexpected ring metadata {meta}")
    if row["frame"] != "19" or row["reason"] != "0" or row["thread_cycles_valid"] != "1":
        raise AssertionError(f"bad identity/cycle metadata: {row}")
    if row["total_ticks"] != str(int(row["qpc_end"]) - int(row["qpc_begin"])):
        raise AssertionError(f"bad total QPC accounting: {row}")
    stage_names = ["close_ticks", "execute_ticks", "signal_ticks", "completed_value_ticks",
                   "set_event_ticks", "wait_ticks", "info_queue_ticks", "retired_release_ticks",
                   "alloc_reset_ticks", "list_reset_ticks"]
    if int(row["unaccounted_ticks"]) + sum(int(row[name]) for name in stage_names) != int(row["total_ticks"]):
        raise AssertionError(f"stage and residual time do not conserve total: {row}")
    expected_failures = {"close_fail": "1", "signal_fail": "3", "event_fail": "5",
                         "reset_fail": "9"}
    if mode in expected_failures and row["failed_stage"] != expected_failures[mode]:
        raise AssertionError(f"failed stage missing for {mode}: {row}")
    if mode == "wait" and (row["waited"] != "1" or row["wait_result"] != str(0)):
        raise AssertionError(f"wait result missing: {row}")
    if mode == "wait_fail" and (row["wait_result"] != str(0xFFFFFFFF) or row["wait_error"] != "123" or row["failed_stage"] != "6"):
        raise AssertionError(f"failed wait details missing: {row}")
    if mode == "no_wait" and (row["waited"] != "0" or row["wait_result"] != str(0xFFFFFFFF)):
        raise AssertionError(f"no-wait sentinel/flag incorrect: {row}")
    if mode in ("close_fail", "signal_fail", "event_fail") and row["stage_mask"] == "0":
        raise AssertionError(f"early-failure row has no phase metadata: {row}")
    if mode == "reset_fail" and (row["alloc_reset_hr"] != "0x80004005" or row["list_reset_hr"] != "0x80004005"):
        raise AssertionError(f"reset error results not retained: {row}")
    return row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, help="clang-cl executable")
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "local/test-rsx-slow-flush")
    args = parser.parse_args()
    sdk_source = (args.sdk / "libs/video/rsx_live_draw.c").read_text(encoding="utf-8")
    diagnostic = extract(sdk_source, DIAG_BEGIN, DIAG_END)
    wrapper = extract(sdk_source, WRAPPER_BEGIN, WRAPPER_END)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    fixture = args.output_dir / "rsx_slow_flush_fixture.c"
    fixture.write_text(fixture_source(diagnostic, wrapper), encoding="utf-8")
    exe = args.output_dir / "rsx_slow_flush_fixture.exe"
    built = subprocess.run(compiler_command(args.compiler, fixture, exe), capture_output=True, text=True)
    if built.returncode:
        raise RuntimeError(f"compile failed ({built.returncode})\n{built.stdout}\n{built.stderr}")
    with tempfile.TemporaryDirectory(prefix="haze-rsx-slow-flush-") as temp:
        temp_path = Path(temp)
        run_case(exe, "disabled", None)
        print("PASS disabled path makes no QPC or thread-cycle calls and preserves flush calls")
        for mode in ("close_fail", "signal_fail", "event_fail", "wait", "wait_fail", "no_wait", "reset_fail"):
            row = run_case(exe, mode, temp_path / f"{mode}.csv")
            print(f"PASS {mode}: failed_stage={row['failed_stage']} waited={row['waited']} mask={row['stage_mask']}")
    print("8 focused RSX slow-flush cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
