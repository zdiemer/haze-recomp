"""Windows native Haze instruction sampling and per-thread CPU counters.

Usage: python scripts/diagnose.py native-cpu PID [SECONDS]
Briefly suspends each Haze thread to read its instruction pointer, then resumes
it before resolving output symbols. Optional native stack walking runs while
the thread is suspended and can substantially perturb execution. Never use
this sampler inside FPS benchmark windows. RIP/stack counts include waits and are NOT
percentages of CPU utilization. CPU milliseconds come from GetThreadTimes.
Requires the matching executable/PDB in build/haze or --symbol-path. No guest state is written.
Guest-address counts sample only a recognized fast vm_read32 load instruction.
"""
import ctypes as c
from ctypes import wintypes as w
import sys
import argparse
parser_note = 'Timeline samples perturb execution and are not FPS benchmark evidence.'
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('pid',type=int)
parser.add_argument('seconds',type=float,nargs='?',default=20)
parser.add_argument('--scan-every',type=int,default=1,
    help='Thread enumeration cadence in sampling iterations; 1 keeps short jobs in proportion.')
parser.add_argument('--tid',type=int,action='append',help='Sample only these thread IDs, after checking process ownership.')
parser.add_argument('--window-thread',action='store_true',
    help='Select the thread owning this process\'s ps3recomp_null window (the native render pump).')
parser.add_argument('--skip-idle',action='store_true',
    help='Do not suspend threads that used under ~0.2 ms of cycles since the previous round; keeps rounds short when sampling all threads, but drops most wait locations.')
parser.add_argument('--interval-ms',type=float,default=9,
    help='Mean randomized interval between sample rounds; use separately from FPS benchmarks.')
parser.add_argument('--raw-limit',type=int,default=20,help='Maximum individual instruction locations per thread.')
parser.add_argument('--stack-limit',type=int,
    help='Maximum recorded call chains per thread; defaults to raw-limit. Increase to retain rare active callers among many waiting chains.')
parser.add_argument('--symbol-path',type=Path,default=Path(__file__).resolve().parents[2]/'build'/'haze')
parser.add_argument('--stack-depth',type=int,default=0,
    help='Optional AMD64 unwind depth, 0 disables it. Build scripts/native_stackwalk.c first.')
parser.add_argument('--stack-every',type=int,default=10,help='Unwind every N sampling iterations.')
parser.add_argument('--stack-library',type=Path,
    default=Path(__file__).resolve().parents[2]/'local'/'native_stackwalk.dll')
parser.add_argument('--timeline-output',type=Path,
    help='Write a bounded QPC-stamped sample suffix after resuming threads; diagnostic only.')
parser.add_argument('--timeline-limit',type=int,default=100000,
    help='Maximum retained timeline samples; overwritten samples are counted.')
parser.add_argument('--stall-ms',type=float,
    help='Suspend only after a uint32 frame counter stops progressing for this long; diagnostic only.')
parser.add_argument('--stall-symbol',default='g_frames_presented',
    help='Exact PDB name of the uint32 frame counter watched by --stall-ms.')
parser.add_argument('--stall-samples',type=int,default=3,
    help='Maximum sampling rounds per unchanged frame counter; requires progress to rearm.')
args=parser.parse_args()
if args.window_thread and args.tid:
    parser.error('--window-thread and --tid are mutually exclusive')
if not 1<=args.scan_every<=1000:
    parser.error('scan-every must be 1..1000')
if not .5<=args.interval_ms<=100 or not 1<=args.raw_limit<=1000:
    parser.error('interval-ms must be .5..100 and raw-limit must be 1..1000')
if args.stack_limit is not None and not 1<=args.stack_limit<=10000:
    parser.error('stack-limit must be 1..10000')
if not 0<=args.stack_depth<=64 or not 1<=args.stack_every<=1000:
    parser.error('stack-depth must be 0..64 and stack-every must be 1..1000')
if args.stack_depth and not args.stack_library.is_file():
    parser.error('Build the native stackwalk helper DLL before requesting stacks')
if not 1<=args.timeline_limit<=1000000:
    parser.error('timeline-limit must be 1..1000000')
if args.skip_idle and args.stall_ms is not None:
    parser.error('--skip-idle cannot be combined with stall sampling')
if args.stall_ms is not None:
    if not 10<=args.stall_ms<=1000 or not 1<=args.stall_samples<=10:
        parser.error('stall-ms must be 10..1000 and stall-samples must be 1..10')
    if not args.timeline_output or not (args.window_thread or args.tid and len(args.tid)==1):
        parser.error('stall sampling requires a timeline and exactly one selected thread')

k=c.WinDLL('kernel32',use_last_error=True)
d=c.WinDLL('dbghelp',use_last_error=True)
for name in ('OpenProcess','OpenThread'):
    f=getattr(k,name); f.argtypes=[w.DWORD,w.BOOL,w.DWORD]; f.restype=w.HANDLE
k.SuspendThread.argtypes=[w.HANDLE]; k.SuspendThread.restype=w.DWORD
k.ResumeThread.argtypes=[w.HANDLE]
k.ResumeThread.restype=w.DWORD
k.CloseHandle.argtypes=[w.HANDLE]
k.GetThreadContext.argtypes=[w.HANDLE,c.c_void_p]
class SYMBOL_INFO(c.Structure):
    _fields_=[('SizeOfStruct',w.ULONG),('TypeIndex',w.ULONG),('Reserved',c.c_ulonglong*2),('Index',w.ULONG),('Size',w.ULONG),('ModBase',c.c_ulonglong),('Flags',w.ULONG),('Value',c.c_ulonglong),('Address',c.c_ulonglong),('Register',w.ULONG),('Scope',w.ULONG),('Tag',w.ULONG),('NameLen',w.ULONG),('MaxNameLen',w.ULONG),('Name',c.c_char*1)]
d.SymInitializeW.argtypes=[w.HANDLE,w.LPCWSTR,w.BOOL]
d.SymFromAddr.argtypes=[w.HANDLE,c.c_ulonglong,c.POINTER(c.c_ulonglong),c.POINTER(SYMBOL_INFO)]
d.SymFromName.argtypes=[w.HANDLE,c.c_char_p,c.POINTER(SYMBOL_INFO)]
h=k.OpenProcess(0x410,False,args.pid); assert h,c.get_last_error()
assert d.SymInitializeW(h,str(args.symbol_path.resolve()),True),c.get_last_error()
stackwalk=None
if args.stack_depth:
    stack_library=c.WinDLL(str(args.stack_library.resolve()),use_last_error=True)
    stackwalk=stack_library.haze_profile_stackwalk
    stackwalk.argtypes=[w.HANDLE,w.HANDLE,c.c_void_p,c.POINTER(c.c_ulonglong),w.UINT]
    stackwalk.restype=w.UINT
def symbol(addr):
    b=c.create_string_buffer(c.sizeof(SYMBOL_INFO)+2048)
    info=c.cast(b,c.POINTER(SYMBOL_INFO)); info.contents.SizeOfStruct=c.sizeof(SYMBOL_INFO); info.contents.MaxNameLen=2048
    disp=c.c_ulonglong()
    if d.SymFromAddr(h,addr,c.byref(disp),info) and disp.value < 0x10000:
        return c.string_at(c.addressof(b)+SYMBOL_INFO.Name.offset).decode(errors='replace')+f'+0x{disp.value:X}'


import collections,time,json,random
timeline=collections.deque(maxlen=args.timeline_limit)
timeline_count=0
k.QueryPerformanceCounter.argtypes=[c.POINTER(c.c_longlong)]
k.QueryPerformanceFrequency.argtypes=[c.POINTER(c.c_longlong)]
frequency=c.c_longlong()
assert k.QueryPerformanceFrequency(c.byref(frequency)) and frequency.value>0
def qpc():
    value=c.c_longlong()
    if not k.QueryPerformanceCounter(c.byref(value)): raise c.WinError(c.get_last_error())
    return value.value
k.GetCurrentProcess.restype=w.HANDLE
k.DuplicateHandle.argtypes=[w.HANDLE,w.HANDLE,w.HANDLE,c.POINTER(w.HANDLE),w.DWORD,w.BOOL,w.DWORD]
k.GetThreadTimes.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p]
k.GetProcessTimes.argtypes=k.GetThreadTimes.argtypes
k.GetExitCodeThread.argtypes=[w.HANDLE,c.POINTER(w.DWORD)]
k.GetProcessIdOfThread.argtypes=[w.HANDLE]
k.GetProcessIdOfThread.restype=w.DWORD
k.GetSystemTimeAsFileTime.argtypes=[c.POINTER(c.c_ulonglong)]
pid=args.pid; duration=args.seconds
handles={}; active={}; live_ids={}; first={}; last={}; raw=collections.Counter()
stacks=collections.Counter(); suspension_count=0; suspension_total=0; suspension_max=0
scan_stats=collections.Counter(); seen_ids=set()
started_filetime=c.c_ulonglong()
def running(th):
    code=w.DWORD()
    return bool(k.GetExitCodeThread(th,c.byref(code))) and code.value==259
nt=c.WinDLL('ntdll')
nt.NtGetNextThread.argtypes=[w.HANDLE,w.HANDLE,w.DWORD,w.ULONG,w.ULONG,c.POINTER(w.HANDLE)]
nt.NtGetNextThread.restype=c.c_long
k.GetThreadId.argtypes=[w.HANDLE]; k.GetThreadId.restype=w.DWORD
k.QueryThreadCycleTime.argtypes=[w.HANDLE,c.POINTER(c.c_ulonglong)]
cycles={}
def advanced(key,th):
    value=c.c_ulonglong()
    if not k.QueryThreadCycleTime(th,c.byref(value)): return True
    previous=cycles.get(key); cycles[key]=value.value
    # Suspension runs a kernel APC on the target thread, so a sampled waiting
    # thread still advances a little. Under ~0.2 ms of cycles counts as idle.
    return previous is None or value.value-previous>=1_000_000
def retire(key):
    # Record the final times and close the handle: an open handle keeps an
    # exited thread in the process list that scan() walks every round.
    th=handles[key]
    if th is None: return
    last[key]=cpu(th); k.CloseHandle(th); handles[key]=None
    active.pop(key,None)
    if live_ids.get(key[0])==key: live_ids.pop(key[0],None)
def scan():
    # Walk only this process's threads. Handles come from the process itself,
    # so thread IDs cannot be reused by another process between list and open.
    # Cheap enough to run every round, which keeps short-lived SPU job threads
    # in proportion to their lifetimes.
    scan_stats['snapshots']+=1
    current=w.HANDLE()
    while True:
        following=w.HANDLE()
        status=nt.NtGetNextThread(h,current,0x4a,0,0,c.byref(following))
        if current.value: k.CloseHandle(current)
        if status<0:
            if status&0xffffffff!=0x8000001a: scan_stats[f'enumerate_status_{status&0xffffffff:08X}']+=1
            break
        current=following; th=current.value
        tid=k.GetThreadId(th)
        if args.tid and tid not in args.tid: continue
        scan_stats['owned_entries']+=1; seen_ids.add(tid)
        old=live_ids.get(tid)
        if old is not None and running(handles[old]): continue
        if old is not None: retire(old)
        created,exited,kt,ut=[c.c_ulonglong() for _ in range(4)]
        if not k.GetThreadTimes(th,c.byref(created),c.byref(exited),c.byref(kt),c.byref(ut)):
            scan_stats['times_failed']+=1; continue
        key=(tid,created.value)
        if key in handles or not running(th): continue
        scan_stats['opened_lifetimes']+=1
        # Duplicate: the enumeration handle is closed when the walk advances.
        copy=w.HANDLE()
        if not k.DuplicateHandle(k.GetCurrentProcess(),th,k.GetCurrentProcess(),c.byref(copy),0,False,2):
            scan_stats['duplicate_failed']+=1; continue
        handles[key]=copy.value; active[key]=copy.value; live_ids[tid]=key
        total=kt.value+ut.value
        # Include work before first observation when this
        # thread was created inside the sampling interval.
        first[key]=0 if created.value>=started_filetime.value else total
        last[key]=total
def cpu(th):
    a,b,kt,ut=[c.c_ulonglong() for _ in range(4)]
    if not k.GetThreadTimes(th,c.byref(a),c.byref(b),c.byref(kt),c.byref(ut)): return 0
    return kt.value+ut.value
def process_cpu():
    a,b,kt,ut=[c.c_ulonglong() for _ in range(4)]
    if not k.GetProcessTimes(h,c.byref(a),c.byref(b),c.byref(kt),c.byref(ut)):
        raise c.WinError(c.get_last_error())
    return kt.value+ut.value
pathbuf=c.create_unicode_buffer(32768); pathlen=w.DWORD(len(pathbuf))
k.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD)]
assert k.QueryFullProcessImageNameW(h,0,pathbuf,c.byref(pathlen)),c.get_last_error()
assert Path(pathbuf.value).name.lower()=='hazerecomp.exe', 'PID must identify HazeRecomp.exe'
window_thread_evidence=None
if args.window_thread:
    user=c.WinDLL('user32',use_last_error=True)
    callback_type=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    user.EnumWindows.argtypes=[callback_type,w.LPARAM]
    user.GetWindowThreadProcessId.argtypes=[w.HWND,c.POINTER(w.DWORD)]
    user.GetWindowThreadProcessId.restype=w.DWORD
    user.GetClassNameW.argtypes=[w.HWND,w.LPWSTR,c.c_int]
    matches=[]
    @callback_type
    def visit(window,param):
        owner=w.DWORD()
        tid=user.GetWindowThreadProcessId(window,c.byref(owner))
        name=c.create_unicode_buffer(256)
        if owner.value==pid and user.GetClassNameW(window,name,len(name)) and name.value=='ps3recomp_null':
            matches.append(dict(hwnd=int(window),tid=int(tid),pid=int(owner.value),window_class=name.value))
        return True
    if not user.EnumWindows(visit,0):raise c.WinError(c.get_last_error())
    if len(matches)!=1:raise RuntimeError(f'Expected one owned renderer window, found {len(matches)}')
    window_thread_evidence=matches[0]
    args.tid=[window_thread_evidence['tid']]
assert 1<=duration<=120, 'Duration must be between 1 and 120 seconds'
# Recognize this build's ordinary vm_read32 load/bswap pair. RCX holds the
# guest address at these instructions. Do not interpret registers at arbitrary
# helper offsets, where the compiler may have already reused them.
symbuf=c.create_string_buffer(c.sizeof(SYMBOL_INFO)+2048)
syminfo=c.cast(symbuf,c.POINTER(SYMBOL_INFO))
syminfo.contents.SizeOfStruct=c.sizeof(SYMBOL_INFO); syminfo.contents.MaxNameLen=2048
fast_load=0; memory_reads=collections.Counter()
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
if d.SymFromName(h,b'vm_read32',syminfo):
    code=c.create_string_buffer(256); got=c.c_size_t()
    address=syminfo.contents.Address
    if k.ReadProcessMemory(h,address,code,len(code),c.byref(got)):
        offset=code.raw[:got.value].find(bytes.fromhex('8b04080fc8'))
        if offset>=0: fast_load=address+offset
stall_trigger=None
stall_address=None
if args.stall_ms is not None:
    from .frame_stall_trigger import FrameStallTrigger
    if not d.SymFromName(h,args.stall_symbol.encode('utf-8'),syminfo):
        raise RuntimeError('Frame-counter symbol not found in the matching PDB')
    if syminfo.contents.Size != 4:
        raise RuntimeError('Frame-counter symbol must be exactly four bytes')
    stall_address=syminfo.contents.Address
    stall_trigger=FrameStallTrigger(frequency.value,args.stall_ms,args.stall_samples)

def stall_metadata():
    if stall_trigger is None: return None
    return dict(stall_trigger.summary(),symbol=args.stall_symbol,address=stall_address,
                threshold_ms=args.stall_ms,
                note='Observed counter progress is not the exact present timestamp. Polling and triggered suspensions perturb execution; capped episodes can miss later locations.')

k.GetSystemTimeAsFileTime(c.byref(started_filetime))
process_begin=process_cpu()
start=time.perf_counter(); iteration=0
timeline_begin=qpc() if args.timeline_output else None
while time.perf_counter()-start<duration:
    if iteration%args.scan_every==0: scan()
    trigger_evidence=None
    if stall_trigger is not None:
        value=c.c_uint32(); got=c.c_size_t()
        if not k.ReadProcessMemory(h,stall_address,c.byref(value),4,c.byref(got)) or got.value!=4:
            raise RuntimeError('Could not read the frame counter; stopping without further suspensions')
        trigger_evidence=stall_trigger.observe(value.value,qpc())
        if trigger_evidence is None:
            iteration+=1; time.sleep(random.uniform(.5,1.5)*args.interval_ms/1000)
            continue
        if not active:
            raise RuntimeError('Stall detected without an active selected thread; capture incomplete')
    for tid,th in list(active.items()):
        ct=cpu(th); first.setdefault(tid,ct); last[tid]=ct
        if not running(th):
            if stall_trigger is not None:
                raise RuntimeError('Selected thread exited during a stall; capture incomplete')
            retire(tid)
            continue
        if args.skip_idle and not advanced(tid,th):
            scan_stats['idle_skips']+=1; continue
        buf=c.create_string_buffer(1250); cp=(c.addressof(buf)+15)&~15
        c.c_uint32.from_address(cp+0x30).value=0x100003  # CONTROL | INTEGER
        sample_begin=qpc() if args.timeline_output else None
        if k.SuspendThread(th)==0xffffffff:
            if stall_trigger is not None:
                raise c.WinError(c.get_last_error())
            continue
        suspended_begin=time.perf_counter()
        addresses=()
        stack_attempted=False
        try:
            good=k.GetThreadContext(th,cp); rip=c.c_uint64.from_address(cp+0xf8).value
            rcx=c.c_uint64.from_address(cp+0x80).value
            context_qpc=qpc() if args.timeline_output else None
            if good and stackwalk and (stall_trigger is not None or iteration%args.stack_every==0):
                stack_attempted=True
                output=(c.c_ulonglong*args.stack_depth)()
                count=stackwalk(h,th,cp,output,args.stack_depth)
                addresses=tuple(output[:count])
                stacks[(tid,addresses)]+=1
        finally:
            resume_count=k.ResumeThread(th)
            if resume_count==0xffffffff:
                raise c.WinError(c.get_last_error())
            suspended_elapsed=time.perf_counter()-suspended_begin
            suspension_count+=1; suspension_total+=suspended_elapsed
            suspension_max=max(suspension_max,suspended_elapsed)
        if args.timeline_output:
            timeline_count+=1
            timeline.append(dict(sequence=timeline_count, tid=tid[0], created_filetime=tid[1],
                begin_qpc=sample_begin, context_qpc=context_qpc, resumed_qpc=qpc(),
                context_valid=bool(good), rip=rip if good else 0,
                stack_attempted=stack_attempted, addresses=addresses,
                stall_trigger=trigger_evidence,
                thread_cpu_100ns_before_suspend=ct))
        if good:
            raw[(tid,rip)]+=1
            if fast_load and rip in (fast_load,fast_load+3):
                memory_reads[(tid,rcx&0xffffffff)]+=1
    # Avoid repeatedly sampling the same phase of a bursty per-frame job stream.
    iteration+=1; time.sleep(random.uniform(.5,1.5)*args.interval_ms/1000)
elapsed=time.perf_counter()-start
timeline_end=qpc() if args.timeline_output else None
for tid,th in handles.items():
    if th is not None: last[tid]=cpu(th)
process_ms=(process_cpu()-process_begin)/10000
resolved={}
def sym(addr):
    if addr not in resolved: resolved[addr]=symbol(addr) or hex(addr)
    return resolved[addr]
rows=[]
for tid in sorted(handles,key=lambda t:last[t]-first[t],reverse=True):
    hist=collections.Counter()
    for (t,rip),n in raw.items():
        if t==tid: hist[sym(rip).split('+0x')[0]]+=n
    rows.append(dict(tid=tid[0],created_filetime=tid[1],cpu_ms=(last[tid]-first[tid])/10000,rips=hist.most_common(20),raw_rips=[(sym(rip),n) for (t,rip),n in raw.most_common() if t==tid][:args.raw_limit],
                     stacks=[dict(frames=[sym(pc) for pc in addresses],samples=n)
                             for (t,addresses),n in stacks.most_common() if t==tid][:(args.stack_limit or args.raw_limit)],
                     sampled_read32_addresses=[(f'0x{addr:08X}',n) for (t,addr),n in memory_reads.most_common() if t==tid][:20]))
tracked_ms=sum(row['cpu_ms'] for row in rows)
if args.timeline_output:
    events=[]
    for event in timeline:
        event=dict(event)
        event['rip_symbol']=sym(event['rip']) if event['context_valid'] else None
        event['frames']=[sym(pc) for pc in event.pop('addresses')]
        events.append(event)
    args.timeline_output.write_text(json.dumps(dict(
        note=parser_note+' QPC brackets SuspendThread/context/unwind/ResumeThread; '
             'stack samples show locations, not CPU utilization or proof of causality.',
        pid=pid,exe=pathbuf.value,qpc_frequency=frequency.value,selected_tids=args.tid,
        stall_trigger=stall_metadata(),
        begin_qpc=timeline_begin,end_qpc=timeline_end,
        capacity=args.timeline_limit,total_samples=timeline_count,
        overwritten=max(0,timeline_count-len(events)),samples=events),indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(pid=pid,exe=pathbuf.value,elapsed_s=elapsed,iterations=iteration,fast_read32_recognized=bool(fast_load),
    scan_every=args.scan_every,selected_tids=args.tid,window_thread=window_thread_evidence,interval_ms=args.interval_ms,
    symbol_path=str(args.symbol_path.resolve()),stack_depth=args.stack_depth,stack_every=args.stack_every,
    suspension=dict(count=suspension_count,total_ms=suspension_total*1000,max_ms=suspension_max*1000),
    process_cpu_ms=process_ms,tracked_thread_cpu_ms=tracked_ms,
    unattributed_cpu_ms=max(0,process_ms-tracked_ms),
    thread_scan=dict(scan_stats,unique_ids=len(seen_ids)),
    timeline_output=str(args.timeline_output) if args.timeline_output else None,
    stall_trigger=stall_metadata(),
    coverage_note='Threads that start and exit between enumerations are missed; RIP samples include waits.',
    threads=rows),indent=2))
for th in handles.values():
    if th is not None: k.CloseHandle(th)
k.CloseHandle(h)
