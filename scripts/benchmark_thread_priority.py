"""Test one identified Haze worker's scheduling priority without changing guest work.

Use a visually verified stationary scene in a normal build with
RSX_PRESENT_CSV set. Run separately from sampling, captures and builds.
The thread handle is checked against the selected executable's PID and retained
throughout; its original priority is restored even if recording fails.
After closing Haze normally, use benchmark_present_modes.py report on the CSV
and this recording. CPU-work flags are validated at both present endpoints.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import time

from benchmark_cpu_work import process_clock


class OwnedThreadPriority:
    def __init__(self, pid, tid):
        self.kernel = k = c.WinDLL('kernel32', use_last_error=True)
        k.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        k.OpenThread.restype = w.HANDLE
        k.GetProcessIdOfThread.argtypes = [w.HANDLE]
        k.GetProcessIdOfThread.restype = w.DWORD
        k.GetThreadPriority.argtypes = [w.HANDLE]
        k.GetThreadPriority.restype = c.c_int
        k.SetThreadPriority.argtypes = [w.HANDLE, c.c_int]
        k.GetThreadTimes.argtypes = [w.HANDLE] + [c.c_void_p] * 4
        k.GetExitCodeThread.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
        k.CloseHandle.argtypes = [w.HANDLE]
        self.handle = k.OpenThread(0x60, False, tid)  # query/set information only
        if not self.handle:
            raise c.WinError(c.get_last_error())
        try:
            if k.GetProcessIdOfThread(self.handle) != pid:
                raise RuntimeError('Thread does not belong to the selected process')
            self.original = k.GetThreadPriority(self.handle)
            if self.original == 0x7fffffff:
                raise c.WinError(c.get_last_error())
            self.cpu()
        except BaseException:
            k.CloseHandle(self.handle)
            raise

    def cpu(self):
        created, exited, kernel, user = [c.c_ulonglong() for _ in range(4)]
        if not self.kernel.GetThreadTimes(self.handle, c.byref(created),
                c.byref(exited), c.byref(kernel), c.byref(user)):
            raise c.WinError(c.get_last_error())
        if exited.value:
            raise RuntimeError('Selected worker has exited')
        self.created_filetime = created.value
        return (kernel.value + user.value) / 10000

    def set(self, priority):
        self.cpu()
        if not self.kernel.SetThreadPriority(self.handle, priority):
            raise c.WinError(c.get_last_error())
        self.verify(priority)

    def verify(self, priority):
        if self.kernel.GetThreadPriority(self.handle) != priority:
            raise RuntimeError('Selected worker priority changed during recording')

    def close(self):
        try:
            code = w.DWORD()
            if not self.kernel.GetExitCodeThread(self.handle, c.byref(code)):
                raise c.WinError(c.get_last_error())
            if code.value == 259:  # STILL_ACTIVE; retained handle prevents ID reuse
                if not self.kernel.SetThreadPriority(self.handle, self.original):
                    raise c.WinError(c.get_last_error())
                self.verify(self.original)
                return True
            return False
        finally:
            self.kernel.CloseHandle(self.handle)


def record(args):
    cpu, close_process = process_clock(args.pid)
    worker = None
    result = None
    completed = False
    try:
        k = c.WinDLL('kernel32', use_last_error=True)
        k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        k.OpenProcess.restype = w.HANDLE
        k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
        k.CloseHandle.argtypes = [w.HANDLE]
        handle = k.OpenProcess(0x1000, False, args.pid)
        if not handle:
            raise c.WinError(c.get_last_error())
        try:
            path = c.create_unicode_buffer(32768)
            size = w.DWORD(len(path))
            if not k.QueryFullProcessImageNameW(handle, 0, path, c.byref(size)):
                raise c.WinError(c.get_last_error())
            if Path(path.value).resolve() != args.exe.resolve():
                raise RuntimeError('PID differs from the explicitly selected executable')
        finally:
            k.CloseHandle(handle)
        worker = OwnedThreadPriority(args.pid, args.tid)
        frequency = c.c_longlong()
        if not k.QueryPerformanceFrequency(c.byref(frequency)):
            raise c.WinError(c.get_last_error())

        def qpc():
            value = c.c_longlong()
            if not k.QueryPerformanceCounter(c.byref(value)):
                raise c.WinError(c.get_last_error())
            return value.value

        def wait(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                cpu()
                worker.cpu()
                time.sleep(min(.25, max(0, deadline - time.monotonic())))

        result = dict(pid=args.pid, exe=str(args.exe.resolve()), tid=args.tid,
            thread_created_filetime=worker.created_filetime,
            original_priority=worker.original, qpc_frequency=frequency.value,
            warmup_s=args.warmup, measure_s=args.seconds, mode_column='cpu_work',
            blocks=[], complete=False, restored_priority=False, note=__doc__)
        args.output.write_text(json.dumps(result, indent=2) + '\n')
        for priority in args.priorities:
            worker.set(priority)
            wait(args.warmup)
            worker.verify(priority)
            start = qpc()
            process_begin, thread_begin = cpu(), worker.cpu()
            wait(args.seconds)
            process_ms, thread_ms = cpu() - process_begin, worker.cpu() - thread_begin
            end = qpc()
            worker.verify(priority)
            block = dict(flags=args.cpu_work, priority=priority,
                start_qpc=start, end_qpc=end, process_cpu_ms=process_ms,
                thread_cpu_ms=thread_ms,
                mean_cores=process_ms * frequency.value / (end-start) / 1000,
                thread_mean_cores=thread_ms * frequency.value / (end-start) / 1000)
            result['blocks'].append(block)
            args.output.write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(block), flush=True)
        completed = True
    finally:
        try:
            if worker:
                restored = worker.close()
                if result is not None:
                    result['restored_priority'] = restored
                    result['complete'] = completed and restored
        finally:
            close_process()
            if result is not None:
                args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--tid', type=int, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--priorities', type=int, nargs='+', default=[0,-1,0,-1,0,-1], choices=[0,-1])
    parser.add_argument('--cpu-work', type=int, default=1283)
    parser.add_argument('--warmup', type=float, default=10)
    parser.add_argument('--seconds', type=float, default=12)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.warmup < 5 or args.seconds < 10 or not 0 <= args.cpu_work <= 8191:
        parser.error('Use at least 5 warmup seconds, 10 measured seconds and CPU flags 0..8191')
    record(args)
