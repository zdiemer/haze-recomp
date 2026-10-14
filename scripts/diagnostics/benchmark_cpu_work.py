"""Alternate measured renderer CPU variants within one verified native scene.

Requires a profiling build, RSX_CPU_WORK_FILE and RSX_PROFILE_CSV. Do not move
the camera or request readbacks during the blocks. Flags: 1 bulk references,
2 direct dense remap, 4 XXH3 texture checks. Bit 8 also selects the experimental
sleep backend if PS3_USLEEP_MODE_FILE points to this same control file.
Bit 16 selects private SPU history if SPU_DEPTH_RING_FILE points to it too;
without that diagnostic control, the corrected private history is always used.
Zero selects the previous paths.
Bit 32 selects heap SPU contexts if SPU_EXECUTION_MODE_FILE uses the same file.
Bit 64 selects SIMD SPU bytes with -CompareSpuByteOps and that control file;
otherwise the normal SIMD helpers run without comparison branches.
Bit 128 selects coherent row blits if GCM_ROW_BLIT_MODE_FILE uses the same file.
Bit 256 selects specialized SIMD vertex attribute loops in the renderer.
Bit 512 selects the full-content-verified vertex-program analysis cache.
Bit 1024 selects bulk copying of validated triangle-list remap indices.
Bit 2048 selects specialized signed-16 vertex loops.
Bit 4096 selects registered native texture write tracking.
With --mode-column vertex_cache or geometry_cache, flags instead select 0/1
in RSX_VERTEX_CACHE_FILE or RSX_GEOMETRY_CACHE_FILE. Keep CPU-work flags fixed
through a separate RSX_CPU_WORK_FILE when comparing those caches.
With --mode-column vertex_workers, flags select 0..3 extra native workers
through RSX_VERTEX_WORKERS_FILE; keep the renderer CPU-work file separate.
The runner stays muted. Compare individual stage costs as well as FPS: guest
work and NPC activity can drift even when the camera remains stationary.
"""
import argparse
import csv
import io
import json
from pathlib import Path
import time


def process_clock(pid):
    """Return a read-only CPU clock; its window includes warmup and propagation."""
    import ctypes as c
    from ctypes import wintypes as w
    k=c.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD]; k.OpenProcess.restype=w.HANDLE
    k.GetProcessTimes.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p]
    k.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD)]
    k.CloseHandle.argtypes=[w.HANDLE]
    handle=k.OpenProcess(0x1000,False,pid)
    if not handle: raise c.WinError(c.get_last_error())
    path=c.create_unicode_buffer(32768); length=w.DWORD(len(path))
    if not k.QueryFullProcessImageNameW(handle,0,path,c.byref(length)) or Path(path.value).name.lower()!='hazerecomp.exe':
        k.CloseHandle(handle); raise RuntimeError('PID must identify HazeRecomp.exe')
    def clock():
        created,exited,kernel,user=[c.c_ulonglong() for _ in range(4)]
        if not k.GetProcessTimes(handle,c.byref(created),c.byref(exited),c.byref(kernel),c.byref(user)):
            raise c.WinError(c.get_last_error())
        if exited.value: raise RuntimeError('Owned benchmark process has exited')
        return (kernel.value+user.value)/10000
    return clock,lambda:k.CloseHandle(handle)


def tail_rows(path):
    with path.open('rb') as stream:
        header = stream.readline().decode('utf-8').strip().split(',')
        stream.seek(0, 2)
        end = stream.tell()
        start = max(0, end - 256 * 1024)
        stream.seek(start)
        if start:
            stream.readline()  # Skip the partial first row.
        else:
            stream.readline()  # Skip the header.
        data = stream.read()
    data = data[:data.rfind(b'\n') + 1]
    rows = []
    for row in csv.DictReader(io.StringIO(data.decode('utf-8')), fieldnames=header):
        try:
            values = {k: float(v) for k, v in row.items()}
            if 'cpu_work' not in values:
                raise RuntimeError('This CSV lacks CPU variant markers; rebuild first.')
            rows.append(values)
        except (ValueError, TypeError):
            continue
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('csv', type=Path)
    p.add_argument('control', type=Path)
    p.add_argument('--flags', type=int, nargs='+', default=[0,1,0,2,0,4,0,7,0,7])
    p.add_argument('--warmup', type=int, default=80)
    p.add_argument('--frames', type=int, default=180)
    p.add_argument('--mode-column', choices=['cpu_work', 'vertex_cache', 'geometry_cache', 'vertex_workers'], default='cpu_work',
                   help='Set the corresponding RSX_CPU_WORK_FILE, RSX_VERTEX_CACHE_FILE, RSX_GEOMETRY_CACHE_FILE, or RSX_VERTEX_WORKERS_FILE control.')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--pid',type=int,help='Optional native process CPU accounting, including warmup/propagation.')
    a = p.parse_args()
    if any(f < 0 or f > 8191 for f in a.flags) or a.frames < 60 or a.warmup < 60:
        p.error('Flags must be 0..8191; use at least 60 warmup and measured frames.')
    if a.mode_column in ('vertex_cache', 'geometry_cache') and any(f not in (0, 1) for f in a.flags):
        p.error('Cache modes must be 0/1.')
    if a.mode_column == 'vertex_workers' and any(f not in (0, 1, 2, 3) for f in a.flags):
        p.error('Vertex worker count must be 0..3.')
    blocks = []
    cpu_clock,close_clock=process_clock(a.pid) if a.pid else (None,None)
    try:
        for flags in a.flags:
            rows = tail_rows(a.csv)
            previous = int(rows[-1]['frame'])
            cpu_begin=cpu_clock() if cpu_clock else None
            wall_begin=time.perf_counter()
            a.control.write_text(f'{flags}\n')
            first = None
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                if cpu_clock: cpu_clock()  # Detect an exited process before waiting out the block.
                rows = tail_rows(a.csv)
                matched = [r for r in rows if r['frame'] > previous
                           and r[a.mode_column] == flags]
                if matched and first is None:
                    first = int(matched[0]['frame']) + a.warmup
                if first is not None and matched and matched[-1]['frame'] >= first+a.frames-1:
                    selected = [r for r in matched if first <= r['frame'] < first+a.frames]
                    if len(selected) != a.frames:
                        raise RuntimeError('Missing gameplay frames inside benchmark block.')
                    if sum(r['draws'] for r in selected)/len(selected) < 500:
                        raise RuntimeError('Block does not match the verified gameplay workload.')
                    block = dict(flags=flags, first_frame=first, last_frame=first+a.frames-1,
                                 frames=a.frames, fps=1000/(sum(r['dt_ms'] for r in selected)/len(selected)))
                    if cpu_clock:
                        wall=time.perf_counter()-wall_begin
                        cpu_ms=cpu_clock()-cpu_begin
                        block['process_cpu_window']=dict(cpu_ms=cpu_ms,wall_s=wall,
                            mean_cores=cpu_ms/1000/wall,
                            note='Includes mode propagation and warmup; distinct from measured frame bounds.')
                    blocks.append(block)
                    a.output.write_text(json.dumps(dict(blocks=blocks, mode_column=a.mode_column, note=__doc__),indent=2))
                    print(json.dumps(block), flush=True)
                    break
                time.sleep(.25)
            else:
                raise TimeoutError(f'Variant {flags} did not finish in 180 seconds')
    finally:
        if close_clock: close_clock()
        a.control.write_text('0\n')


if __name__ == '__main__':
    main()
