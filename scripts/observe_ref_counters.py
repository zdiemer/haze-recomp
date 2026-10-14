"""Read existing PDB scalar counters from one exact owned Haze executable.

No thread suspension or target writes. Samples are non-atomic and diagnostic;
the observer does not attribute elapsed frame time or identify guest callers.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--pid', type=int, required=True)
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--seconds', type=float, default=120)
parser.add_argument('--interval', type=float, default=.25)
args = parser.parse_args()
if not 1 <= args.seconds <= 300 or not .05 <= args.interval <= 5:
    parser.error('Duration 1..300 seconds and interval .05..5 seconds required')
if args.output.exists() or args.output.with_suffix('.ready.json').exists():
    parser.error('Output already exists; use a fresh path')
exe = args.exe.resolve(strict=True)
if exe.name.lower() != 'hazerecomp.exe' or not exe.with_suffix('.pdb').is_file():
    parser.error('Expected HazeRecomp.exe with matching adjacent PDB')
exe_sha = hashlib.sha256(exe.read_bytes()).hexdigest().upper()
k = c.WinDLL('kernel32', use_last_error=True)
d = c.WinDLL('dbghelp', use_last_error=True)
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenProcess.restype = w.HANDLE
k.CloseHandle.argtypes = [w.HANDLE]
k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
k.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(c.c_ulonglong)] * 4
k.GetExitCodeProcess.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
k.QueryPerformanceCounter.argtypes = [c.POINTER(c.c_longlong)]
k.QueryPerformanceFrequency.argtypes = [c.POINTER(c.c_longlong)]

class Symbol(c.Structure):
    _fields_ = [('SizeOfStruct', w.ULONG), ('TypeIndex', w.ULONG),
        ('Reserved', c.c_ulonglong * 2), ('Index', w.ULONG), ('Size', w.ULONG),
        ('ModBase', c.c_ulonglong), ('Flags', w.ULONG), ('Value', c.c_ulonglong),
        ('Address', c.c_ulonglong), ('Register', w.ULONG), ('Scope', w.ULONG),
        ('Tag', w.ULONG), ('NameLen', w.ULONG), ('MaxNameLen', w.ULONG), ('Name', c.c_char * 1)]

d.SymInitializeW.argtypes = [w.HANDLE, w.LPCWSTR, w.BOOL]
d.SymFromName.argtypes = [w.HANDLE, c.c_char_p, c.POINTER(Symbol)]
d.SymCleanup.argtypes = [w.HANDLE]

def checked(ok):
    if not ok:
        raise c.WinError(c.get_last_error())

def qpc():
    value = c.c_longlong()
    checked(k.QueryPerformanceCounter(c.byref(value)))
    return value.value

h = k.OpenProcess(0x410, False, args.pid)
checked(h)
initialized = False
try:
    path = c.create_unicode_buffer(32768)
    length = w.DWORD(len(path))
    checked(k.QueryFullProcessImageNameW(h, 0, path, c.byref(length)))
    if Path(path.value).resolve() != exe:
        raise RuntimeError(f'Exact owned executable mismatch: {path.value}')
    created, exited, kernel, user = [c.c_ulonglong() for _ in range(4)]
    checked(k.GetProcessTimes(h, c.byref(created), c.byref(exited), c.byref(kernel), c.byref(user)))
    checked(d.SymInitializeW(h, str(exe.parent), True))
    initialized = True
    symbols = {}
    unresolved = []
    for name, size, required in [('g_gcm_ref_pub_count', 8, True),
                                  ('g_frames_presented', 4, True),
                                  ('s_ref_qhead', 4, False), ('s_ref_qtail', 4, False)]:
        buf = c.create_string_buffer(c.sizeof(Symbol) + 2048)
        info = c.cast(buf, c.POINTER(Symbol))
        info.contents.SizeOfStruct = c.sizeof(Symbol)
        info.contents.MaxNameLen = 2048
        found = d.SymFromName(h, ('HazeRecomp!' + name).encode(), info)
        if not found or info.contents.Size != size:
            if required:
                raise RuntimeError(f'Required symbol missing or wrong width: {name}')
            unresolved.append(name)
            continue
        symbols[name] = dict(address=info.contents.Address, size=size, module_base=info.contents.ModBase)
    freq = c.c_longlong()
    checked(k.QueryPerformanceFrequency(c.byref(freq)))
    metadata = dict(pid=args.pid, exe=str(exe), sha256=exe_sha,
                    process_creation_filetime=created.value, qpc_frequency=freq.value,
                    symbols=symbols, unresolved_optional=unresolved,
                    requested_seconds=args.seconds, interval_seconds=args.interval,
                    note=__doc__)
    args.output.with_suffix('.ready.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    print('READY: scalar symbols resolved; collecting read-only snapshots', flush=True)
    samples = []
    started = qpc()
    status = 'duration_complete'
    failure = None
    while qpc() - started < args.seconds * freq.value:
        code = w.DWORD()
        checked(k.GetExitCodeProcess(h, c.byref(code)))
        if code.value != 259:
            status = 'target_exited'
            metadata['exit_code'] = code.value
            break
        sample = dict(qpc_begin=qpc())
        for name, descriptor in symbols.items():
            value = c.c_ulonglong() if descriptor['size'] == 8 else c.c_uint32()
            got = c.c_size_t()
            if not k.ReadProcessMemory(h, descriptor['address'], c.byref(value), descriptor['size'], c.byref(got)) or got.value != descriptor['size']:
                failure = dict(symbol=name, error=c.get_last_error(), read_bytes=got.value)
                status = 'read_failed'
                break
            sample[name] = value.value
        sample['qpc_end'] = qpc()
        samples.append(sample)
        if failure:
            break
        time.sleep(args.interval)
    metadata.update(status=status, failure=failure, qpc_begin=started, qpc_end=qpc(), samples=samples)
    args.output.write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    print(json.dumps(dict(status=status, samples=len(samples), output=str(args.output))))
    if failure:
        raise RuntimeError(str(failure))
finally:
    if initialized:
        d.SymCleanup(h)
    k.CloseHandle(h)
