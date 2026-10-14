"""Read-only observer that stops shortly after a stalled frame counter recovers.

The watcher validates a HazeRecomp.exe PID and the exact four-byte
g_frames_presented PDB symbol before starting its QPC observation window. It
only reads that counter; it never suspends threads or writes guest memory.
"""
from __future__ import annotations

import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import math
import os
from pathlib import Path
import time

from frame_stall_trigger import FrameStallTrigger


class FrameRecoveryWatcher:
    """Pure QPC/counter state machine; Win32 polling is kept outside it."""

    def __init__(self, frequency: int, start_qpc: int, stall_ms: float = 50,
                 minimum_seconds: float = 20, post_progress_seconds: float = 5):
        if frequency <= 0 or start_qpc < 0:
            raise ValueError("invalid QPC clock")
        for name, value in (("stall_ms", stall_ms),
                            ("minimum_seconds", minimum_seconds),
                            ("post_progress_seconds", post_progress_seconds)):
            if not math.isfinite(value) or value < 0:
                raise ValueError(f"invalid {name}")
        if stall_ms <= 0:
            raise ValueError("stall_ms must be positive")
        self.frequency = frequency
        self.start_qpc = start_qpc
        self.minimum_ticks = math.ceil(frequency * minimum_seconds)
        self.post_ticks = math.ceil(frequency * post_progress_seconds)
        self.gate = FrameStallTrigger(frequency, stall_ms, 1)
        self.pre_min = True
        self.first_sample: tuple[int, int] | None = None
        self.previous_sample: tuple[int, int] | None = None
        self.last_change_pair: tuple[tuple[int, int], tuple[int, int]] | None = None
        self.raw_progress = 0
        self.polls = 0
        self.previous_qpc: int | None = None
        self.max_poll_gap = 0
        self.trigger: dict | None = None
        self.recovered_qpc: int | None = None
        self.recovered_counter: int | None = None
        self.completion_reason: str | None = None

    def observe(self, counter: int, qpc: int) -> str | None:
        if not 0 <= counter <= 0xFFFFFFFF:
            raise ValueError("frame counter must be uint32")
        if qpc < self.start_qpc or (self.previous_qpc is not None and qpc < self.previous_qpc):
            raise ValueError("counter observations went backward")
        if self.previous_qpc is not None:
            self.max_poll_gap = max(self.max_poll_gap, qpc - self.previous_qpc)
        self.previous_qpc = qpc
        self.polls += 1

        sample = (counter, qpc)
        if self.first_sample is None:
            self.first_sample = sample
        if self.previous_sample is not None and counter != self.previous_sample[0]:
            self.raw_progress += 1
            self.last_change_pair = (self.previous_sample, sample)
        self.previous_sample = sample

        if self.trigger is None:
            if qpc - self.start_qpc < self.minimum_ticks:
                return None
            if self.pre_min:
                self.pre_min = False
                # Seed the trigger with the last observed real transition so a
                # stall spanning the minimum-time boundary is still detected.
                if self.last_change_pair is None:
                    self.gate.observe(counter, qpc)
                else:
                    before, after = self.last_change_pair
                    self.gate.observe(before[0], before[1])
                    self.gate.observe(after[0], after[1])
                    evidence = self.gate.observe(counter, qpc)
                    if evidence is not None:
                        self._set_trigger(evidence, qpc)
                        return self.completion_reason
            else:
                evidence = self.gate.observe(counter, qpc)
                if evidence is not None:
                    self._set_trigger(evidence, qpc)
                    return self.completion_reason

        if self.trigger is not None:
            if self.recovered_qpc is None:
                if counter != self.trigger["frame_counter"]:
                    self.recovered_qpc = qpc
                    self.recovered_counter = counter
            elif qpc - self.recovered_qpc >= self.post_ticks:
                self.completion_reason = "triggered-and-recovered"
        return self.completion_reason

    def _set_trigger(self, evidence: dict, qpc: int) -> None:
        self.trigger = dict(evidence)
        self.trigger["observed_qpc"] = qpc

    def finish_timeout(self, end_qpc: int) -> None:
        if end_qpc < self.start_qpc or (self.previous_qpc is not None and end_qpc < self.previous_qpc):
            raise ValueError("timeout QPC went backward")
        self.completion_reason = "timeout/unfinished"

    def result(self, end_qpc: int, process: dict, error: str | None = None) -> dict:
        if self.completion_reason is None:
            self.finish_timeout(end_qpc)
        gate_stats = self.gate.summary()
        return {
            "qpc_frequency": self.frequency,
            "observation_start_qpc": self.start_qpc,
            "observation_end_qpc": end_qpc,
            "process": process,
            "trigger": self.trigger,
            "recovered_qpc": self.recovered_qpc,
            "recovered_counter": self.recovered_counter,
            "trigger_stats": {
                "watch_polls": self.polls,
                "gate_observations": gate_stats["polls"],
                "trigger_episodes": gate_stats["episodes"],
                "trigger_samples": gate_stats["sample_rounds"],
                "trigger_observed_progress": gate_stats["observed_progress"],
                "observed_progress_total": self.raw_progress,
                "max_poll_gap_ticks": self.max_poll_gap,
                "max_poll_gap_ms": self.max_poll_gap * 1000.0 / self.frequency,
                "minimum_seconds": self.minimum_ticks / self.frequency,
                "post_progress_seconds": self.post_ticks / self.frequency,
            },
            "completion_reason": self.completion_reason,
            "observer_error": error,
        }


def _win32_types():
    if os.name != "nt" or not hasattr(c, "WinDLL"):
        raise RuntimeError("watch_frame_progress.py requires Windows")

    class SYMBOL_INFO(c.Structure):
        _fields_ = [
            ("SizeOfStruct", w.ULONG), ("TypeIndex", w.ULONG),
            ("Reserved", c.c_ulonglong * 2), ("Index", w.ULONG),
            ("Size", w.ULONG), ("ModBase", c.c_ulonglong),
            ("Flags", w.ULONG), ("Value", c.c_ulonglong),
            ("Address", c.c_ulonglong), ("Register", w.ULONG),
            ("Scope", w.ULONG), ("Tag", w.ULONG), ("NameLen", w.ULONG),
            ("MaxNameLen", w.ULONG), ("Name", c.c_char * 1),
        ]
    return SYMBOL_INFO


def _resolve_process(pid: int, expected_exe: Path, symbol_path: Path, symbol_name: str):
    SYMBOL_INFO = _win32_types()
    kernel = c.WinDLL("kernel32", use_last_error=True)
    dbghelp = c.WinDLL("dbghelp", use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.CloseHandle.restype = w.BOOL
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = w.BOOL
    kernel.GetProcessTimes.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p]
    kernel.GetProcessTimes.restype = w.BOOL
    kernel.GetExitCodeProcess.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
    kernel.GetExitCodeProcess.restype = w.BOOL
    kernel.QueryPerformanceFrequency.argtypes = [c.POINTER(c.c_longlong)]
    kernel.QueryPerformanceFrequency.restype = w.BOOL
    kernel.QueryPerformanceCounter.argtypes = [c.POINTER(c.c_longlong)]
    kernel.QueryPerformanceCounter.restype = w.BOOL
    kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    kernel.ReadProcessMemory.restype = w.BOOL

    dbghelp.SymInitializeW.argtypes = [w.HANDLE, w.LPCWSTR, w.BOOL]
    dbghelp.SymInitializeW.restype = w.BOOL
    dbghelp.SymCleanup.argtypes = [w.HANDLE]
    dbghelp.SymCleanup.restype = w.BOOL
    dbghelp.SymFromName.argtypes = [w.HANDLE, c.c_char_p, c.POINTER(SYMBOL_INFO)]
    dbghelp.SymFromName.restype = w.BOOL

    # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION
    process = kernel.OpenProcess(0x1410, False, pid)
    if not process:
        raise c.WinError(c.get_last_error())
    initialized = False
    try:
        pathbuf = c.create_unicode_buffer(32768)
        pathlen = w.DWORD(len(pathbuf))
        if not kernel.QueryFullProcessImageNameW(process, 0, pathbuf, c.byref(pathlen)):
            raise c.WinError(c.get_last_error())
        image = Path(pathbuf.value).resolve()
        expected_image = expected_exe.resolve()
        if image.name.lower() != "hazerecomp.exe" or image != expected_image:
            raise RuntimeError(f"PID {pid} is {image}, expected exact executable {expected_image}")
        created, exited, kernel_time, user_time = [c.c_ulonglong() for _ in range(4)]
        if not kernel.GetProcessTimes(process, c.byref(created), c.byref(exited),
                                      c.byref(kernel_time), c.byref(user_time)):
            raise c.WinError(c.get_last_error())
        if not dbghelp.SymInitializeW(process, str(symbol_path.resolve()), True):
            raise c.WinError(c.get_last_error())
        initialized = True
        buffer = c.create_string_buffer(c.sizeof(SYMBOL_INFO) + 2048)
        info = c.cast(buffer, c.POINTER(SYMBOL_INFO))
        info.contents.SizeOfStruct = c.sizeof(SYMBOL_INFO)
        info.contents.MaxNameLen = 2048
        if not dbghelp.SymFromName(process, symbol_name.encode("utf-8"), info):
            raise RuntimeError(f"PDB symbol {symbol_name!r} not found for {image}")
        if info.contents.Size != 4:
            raise RuntimeError(f"PDB symbol {symbol_name!r} has size {info.contents.Size}, expected 4")
        symbol_address = int(info.contents.Address)
        frequency = c.c_longlong()
        if not kernel.QueryPerformanceFrequency(c.byref(frequency)) or frequency.value <= 0:
            raise c.WinError(c.get_last_error())

        def qpc() -> int:
            value = c.c_longlong()
            if not kernel.QueryPerformanceCounter(c.byref(value)):
                raise c.WinError(c.get_last_error())
            return value.value

        identity = {
            "pid": pid,
            "image_path": str(image.resolve()),
            "creation_filetime": int(created.value),
            "counter_symbol": symbol_name,
            "counter_address": symbol_address,
            "counter_size": int(info.contents.Size),
        }
        return kernel, dbghelp, process, initialized, int(frequency.value), qpc, symbol_address, identity
    except Exception:
        if initialized:
            dbghelp.SymCleanup(process)
        kernel.CloseHandle(process)
        raise


def observe_process(pid: int, expected_exe: Path, symbol_path: Path, symbol_name: str, seconds: float,
                    minimum_seconds: float, stall_ms: float, poll_ms: float,
                    post_progress_seconds: float) -> dict:
    kernel, dbghelp, process, initialized, frequency, qpc, address, identity = _resolve_process(
        pid, expected_exe, symbol_path, symbol_name)
    start_qpc = qpc()
    watch = FrameRecoveryWatcher(frequency, start_qpc, stall_ms,
                                 minimum_seconds, post_progress_seconds)
    deadline = start_qpc + math.ceil(seconds * frequency)
    error = None
    try:
        while True:
            before_read = qpc()
            if before_read >= deadline:
                watch.finish_timeout(before_read)
                break
            exit_code = w.DWORD()
            if not kernel.GetExitCodeProcess(process, c.byref(exit_code)):
                error = f"GetExitCodeProcess failed: {c.get_last_error()}"
                break
            if exit_code.value != 259:  # STILL_ACTIVE
                error = f"process exited with code {exit_code.value}"
                break
            value = c.c_uint32()
            got = c.c_size_t()
            if not kernel.ReadProcessMemory(process, c.c_void_p(address), c.byref(value), 4, c.byref(got)) or got.value != 4:
                error = f"frame counter read failed: {c.get_last_error()} bytes={got.value}"
                break
            observed_qpc = qpc()
            if observed_qpc >= deadline:
                watch.finish_timeout(observed_qpc)
                break
            if watch.observe(int(value.value), observed_qpc) == "triggered-and-recovered":
                break
            remaining_ticks = deadline - qpc()
            if remaining_ticks <= 0:
                watch.finish_timeout(qpc())
                break
            time.sleep(min(poll_ms / 1000.0, remaining_ticks / frequency))
        end_qpc = qpc()
        result = watch.result(end_qpc, identity, error)
        if error:
            result["observer_error_reason"] = "process-exit" if error.startswith("process exited") else "read-failure"
        return result
    finally:
        if initialized:
            dbghelp.SymCleanup(process)
        kernel.CloseHandle(process)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int)
    parser.add_argument("--seconds", type=float, default=180,
                        help="hard maximum observation duration (default 180 seconds)")
    parser.add_argument("--minimum-seconds", type=float, default=20,
                        help="ignore stall triggers before this elapsed time")
    parser.add_argument("--stall-ms", type=float, default=50)
    parser.add_argument("--poll-ms", type=float, default=6,
                        help="counter read cadence; default is within the 5-8 ms target")
    parser.add_argument("--post-progress-seconds", type=float, default=5)
    parser.add_argument("--symbol", default="g_frames_presented")
    parser.add_argument("--exe", type=Path, required=True,
                        help="exact executable path expected for this PID")
    parser.add_argument("--symbol-path", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "haze")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.pid <= 0:
        parser.error("pid must be positive")
    if not 1 <= args.seconds <= 3600:
        parser.error("seconds must be 1..3600")
    if not 0 <= args.minimum_seconds < args.seconds:
        parser.error("minimum-seconds must be nonnegative and less than seconds")
    if not 10 <= args.stall_ms <= 1000:
        parser.error("stall-ms must be 10..1000")
    if not 1 <= args.poll_ms <= 50:
        parser.error("poll-ms must be 1..50")
    if not 0 <= args.post_progress_seconds <= 300:
        parser.error("post-progress-seconds must be 0..300")
    if not args.symbol_path.exists():
        parser.error(f"symbol path does not exist: {args.symbol_path}")
    if not args.exe.is_file():
        parser.error(f"expected executable does not exist: {args.exe}")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = observe_process(args.pid, args.exe, args.symbol_path, args.symbol,
                                 args.seconds, args.minimum_seconds, args.stall_ms,
                                 args.poll_ms, args.post_progress_seconds)
    except Exception as exc:
        result = {
            "qpc_frequency": None,
            "observation_start_qpc": None,
            "observation_end_qpc": None,
            "process": {"pid": args.pid, "counter_symbol": args.symbol},
            "trigger": None,
            "recovered_qpc": None,
            "recovered_counter": None,
            "trigger_stats": None,
            "completion_reason": "timeout/unfinished",
            "observer_error": str(exc),
            "observer_error_reason": "process-exit-or-read/setup-failure",
        }
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 1 if result.get("observer_error") else 0


if __name__ == "__main__":
    raise SystemExit(main())
