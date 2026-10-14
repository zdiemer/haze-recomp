"""Measure selected owned Windows threads without suspending them.

Use outside FPS windows. CONTROL selects a previously configured experimental
mode; its native mode log must be checked separately before attributing results.
Retained handles and process/executable checks prevent sampling a reused TID.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--tid', type=int, action='append', required=True)
    parser.add_argument('--control', type=Path, required=True)
    parser.add_argument('--flags', type=int, nargs='+', required=True)
    parser.add_argument('--warmup', type=float, default=2)
    parser.add_argument('--seconds', type=float, default=8)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 0 <= args.warmup <= 60 or not 0 < args.seconds <= 180:
        parser.error('warmup must be 0..60 and seconds must be 0..180')
    if len(set(args.tid)) != len(args.tid):
        parser.error('select each thread once')

    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenThread.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
    kernel.GetProcessIdOfThread.argtypes = [w.HANDLE]
    kernel.GetProcessIdOfThread.restype = w.DWORD
    kernel.GetExitCodeThread.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
    kernel.GetThreadTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
    kernel.QueryThreadCycleTime.argtypes = [w.HANDLE, c.POINTER(c.c_ulonglong)]
    kernel.QueryPerformanceCounter.argtypes = [c.POINTER(c.c_longlong)]
    kernel.QueryPerformanceFrequency.argtypes = [c.POINTER(c.c_longlong)]

    def check(success):
        if not success:
            raise c.WinError(c.get_last_error())

    def ticks(value):
        return (value.dwHighDateTime << 32) | value.dwLowDateTime

    def counters(handle):
        status = w.DWORD()
        check(kernel.GetExitCodeThread(handle, c.byref(status)))
        if status.value != 259:
            raise RuntimeError('Selected thread has exited')
        created, exited, system, user = (w.FILETIME() for _ in range(4))
        check(kernel.GetThreadTimes(handle, c.byref(created), c.byref(exited), c.byref(system), c.byref(user)))
        if ticks(exited):
            raise RuntimeError('Selected thread has exited')
        cycles = c.c_ulonglong()
        check(kernel.QueryThreadCycleTime(handle, c.byref(cycles)))
        return ticks(created), ticks(system) + ticks(user), cycles.value

    def qpc():
        value = c.c_longlong()
        check(kernel.QueryPerformanceCounter(c.byref(value)))
        return value.value

    handles = {}
    process = kernel.OpenProcess(0x1000, False, args.pid)
    check(process)
    try:
        path = c.create_unicode_buffer(32768)
        size = w.DWORD(len(path))
        check(kernel.QueryFullProcessImageNameW(process, 0, path, c.byref(size)))
        if os.path.normcase(path.value) != os.path.normcase(str(args.exe.resolve())):
            raise ValueError('Owned process executable does not match --exe')
        for tid in args.tid:
            handle = kernel.OpenThread(0x40, False, tid)
            check(handle)
            handles[tid] = handle
            if kernel.GetProcessIdOfThread(handle) != args.pid:
                raise ValueError(f'Thread {tid} does not belong to owned process')
            counters(handle)
        frequency = c.c_longlong()
        check(kernel.QueryPerformanceFrequency(c.byref(frequency)))
        result = dict(pid=args.pid, exe=path.value, qpc_frequency=frequency.value,
                      warmup_s=args.warmup, measure_s=args.seconds, complete=False,
                      note=__doc__, blocks=[])

        def wait(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                time.sleep(min(.1, max(0, deadline - time.monotonic())))

        for flags in args.flags:
            for attempt in range(100):
                try:
                    args.control.write_text(f'{flags}\n')
                    break
                except PermissionError:
                    if attempt == 99:
                        raise
                    time.sleep(.01)
            wait(args.warmup)
            begin = qpc()
            before = {tid: counters(handle) for tid, handle in handles.items()}
            wait(args.seconds)
            after = {tid: counters(handle) for tid, handle in handles.items()}
            end = qpc()
            elapsed = (end - begin) / frequency.value
            threads = []
            for tid in args.tid:
                if before[tid][0] != after[tid][0]:
                    raise RuntimeError('Thread creation time changed')
                cpu_ms = (after[tid][1] - before[tid][1]) / 10000
                threads.append(dict(tid=tid, created_filetime=before[tid][0],
                                    cpu_ms=cpu_ms, mean_cores=cpu_ms / elapsed / 1000,
                                    cycles=after[tid][2] - before[tid][2]))
            result['blocks'].append(dict(flags=flags, start_qpc=begin, end_qpc=end,
                                         elapsed_s=elapsed, threads=threads))
            args.output.write_text(json.dumps(result, indent=2) + '\n')
        result['complete'] = True
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    finally:
        for handle in handles.values():
            kernel.CloseHandle(handle)
        kernel.CloseHandle(process)


if __name__ == '__main__':
    main()
