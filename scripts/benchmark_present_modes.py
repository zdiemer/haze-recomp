"""Measure renderer/queue variants in an ordinary, nonprofiling native build.

Record timed blocks in a visually verified scene with a documented input trajectory.
With --camera-control, the audited native hook supplies a repeating absolute-yaw
trajectory. Its actual-angle CSV is mandatory when reporting. Otherwise the
caller supplies input; --scenario only describes it.
The selected mode's file must point to CONTROL and RSX_PRESENT_CSV must be set before
launch. For whole-launch comparisons, --fixed-mode omits control-file writes;
the report still validates the requested mode against native evidence.
Close the owned game normally afterward, then summarize its fixed-ring
CSV against the recorded QPC windows. No screenshots or profiling may run
during these blocks. Menus/loading do not establish gameplay performance.
Queue variants require SPURS_QUEUE_WAIT_CONTROL and native mode-event evidence
for each explicitly selected critical worker host TID when reporting.
The SPU idle variant uses HAZE_SPU_IDLE_CONTROL and its persistent worker TID.
The usleep variant uses PS3_USLEEP_MODE_FILE with flags 0/8 and explicitly
selected hot PPU workers; native mode events must cover each measured block.
The frame_pacer variant uses HAZE_FRAME_PACER_FILE and the guest main-loop
thread's native mode events to compare the legacy wait with its opt-in bypass.
The spu_triplet_copy variant uses HAZE_TRIPLET_COPY_FILE and process-wide native
mode events; an explicitly selected host TID is unnecessary for that variant.
The native_cursor variant similarly uses HAZE_NATIVE_CURSOR_FILE and its own
process-wide events for the experimental paired PC cursor kernel.
The spu_worker_reuse variant uses SPU_WORKER_REUSE_FILE and process-wide
mode events for fingerprint-allowlisted idle host worker reuse.
The job0_kernel variant uses HAZE_JOB0_NATIVE_FILE and process-wide mode events
for the two guarded image-30 compute loops; hardware arithmetic must be active.
The fifo_recycle_wait variant uses GCM_FIFO_WAIT_CONTROL and requires mode
events for each explicitly selected producer host TID.
The fifo_idle_wait variant uses HAZE_FIFO_WAIT_FILE with timeout flags 0/1
and process-wide startup/change events covering each complete measurement block.
The spu_float_native variant uses SPU_FLOAT_NATIVE_FILE and process-wide
mode events; its hardware arithmetic must be built and supported by the CPU/OS.
The gpu_vertex_pull variant uses RSX_GPU_VERTEX_PULL_FILE and process-wide
renderer mode events for raw geometry decoding in the vertex shader.
The job_wait variant uses startup HAZE_NATIVE_JOB_WAIT=0/1 with --fixed-mode
and main-thread mode events for the guarded job-completion delay. Its former
live control file is removed. Other gameplay/audio delays are unaffected.
The vp_packets variant uses HAZE_VP_PACKET_FILE and main-thread mode events
for the guarded vertex-program command packing operation.
The vp_consumer variant uses RSX_VP_BURST_FILE and FIFO-thread mode events
for guarded vertex-program packet consumption.
Optional --observe-pid reads another Haze process's CPU use; it must remain
alive throughout recording. Shared CPU/GPU contention still prevents treating
that session as an isolated performance measurement.
"""
import argparse
import csv
import ctypes as c
import json
from pathlib import Path
import statistics
import time
import re

from benchmark_cpu_work import process_clock
from summarize_presents import percentile
from validate_camera_sweep import validate_camera_sweep


def verify_process_observation(path, windows, max_gap_s):
    """Check sampled Haze isolation, including observations bracketing each block.

    This cannot exclude a process whose entire lifetime falls between samples.
    Do not describe it as evidence of zero system-wide CPU/GPU contention.
    """
    observations = [json.loads(line) for line in
                    path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    frequency = windows['qpc_frequency']
    if not observations or any(row['qpc_frequency'] != frequency for row in observations):
        raise ValueError('Process observer QPC frequency differs or log is empty')
    if any(b['qpc'] <= a['qpc'] for a, b in zip(observations, observations[1:])):
        raise ValueError('Process observations are not strictly chronological')
    result = []
    for block in windows['blocks']:
        before = [i for i, row in enumerate(observations) if row['qpc'] <= block['start_qpc']]
        after = [i for i, row in enumerate(observations) if row['qpc'] >= block['end_qpc']]
        if not before or not after:
            raise ValueError('Process observations do not bracket the entire block')
        selected = observations[before[-1]:after[0]+1]
        gaps = [(b['qpc'] - a['qpc']) / frequency for a, b in zip(selected, selected[1:])]
        if not gaps or max(gaps) > max_gap_s:
            raise ValueError('Process observation coverage has an excessive gap')
        for row in selected:
            processes = row['processes']
            if len(processes) != 1:
                raise ValueError('Another Haze process or missing owned process observed')
            process = processes[0]
            if (process['ProcessId'] != windows['pid'] or not process['ExecutablePath'] or
                    Path(process['ExecutablePath']).resolve() != Path(windows['exe']).resolve()):
                raise ValueError('Observed Haze does not match the owned benchmark executable')
        result.append(dict(samples=len(selected), max_gap_s=max(gaps)))
    return dict(source=str(path), blocks=result,
                note='Only the owned Haze was observed; process lifetimes between samples and other applications are not excluded.')


def record(args):
    from ctypes import wintypes as w
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, args.pid)
    if not handle:
        raise c.WinError(c.get_last_error())
    try:
        path = c.create_unicode_buffer(32768)
        size = w.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, c.byref(size)):
            raise c.WinError(c.get_last_error())
        if Path(path.value).resolve() != args.exe.resolve():
            raise RuntimeError('PID does not match the explicitly selected benchmark executable')
    finally:
        kernel.CloseHandle(handle)
    frequency = c.c_longlong()
    if not kernel.QueryPerformanceFrequency(c.byref(frequency)):
        raise c.WinError(c.get_last_error())

    def qpc():
        value = c.c_longlong()
        if not kernel.QueryPerformanceCounter(c.byref(value)):
            raise c.WinError(c.get_last_error())
        return value.value

    cpu, close = process_clock(args.pid)
    result = dict(pid=args.pid, exe=str(args.exe.resolve()),
                  qpc_frequency=frequency.value, warmup_s=args.warmup,
                  measure_s=args.seconds, mode_column=args.mode_column,
                  observed_pids=args.observe_pid, scenario=args.scenario,
                  fixed_mode=args.fixed_mode,
                  control_file=str(args.control) if args.control else None,
                  blocks=[], complete=False, note=__doc__)
    observers = []

    def wait(seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            cpu()  # Fail promptly if the explicitly owned run exits.
            time.sleep(min(.25, max(0, deadline - time.monotonic())))

    try:
        for pid in args.observe_pid:
            if pid == args.pid:
                raise ValueError('Observed process must differ from the benchmark process')
            clock, release = process_clock(pid)
            observers.append((pid, clock, release))
        for block_index, flags in enumerate(args.flags):
            camera_token = args.camera_token_base + block_index if args.camera_control else None
            if args.camera_control:
                args.camera_control.write_text(f'{camera_token}\n')
            if not args.fixed_mode:
                args.control.write_text(f'{flags}\n')
            wait(args.warmup)
            start = qpc()
            cpu_start = cpu()
            observed_start = [clock() for _, clock, _ in observers]
            wait(args.seconds)
            cpu_ms = cpu() - cpu_start
            end = qpc()
            observed_cores = {str(pid): (clock() - initial) * frequency.value /
                              (end - start) / 1000
                              for (pid, clock, _), initial in zip(observers, observed_start)}
            block = dict(flags=flags, start_qpc=start, end_qpc=end,
                         camera_token=camera_token,
                         process_cpu_ms=cpu_ms,
                         observed_mean_cores=observed_cores,
                         mean_cores=cpu_ms * frequency.value / (end - start) / 1000)
            result['blocks'].append(block)
            args.output.write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(block), flush=True)
        if args.camera_control:
            # Keep the audit alive beyond the final window before disabling it.
            # This settling time is outside every measured interval.
            wait(.5)
        result['complete'] = True
    finally:
        for _, _, release in observers:
            release()
        close()
        if not args.fixed_mode:
            args.control.write_text(f'{args.flags[0]}\n')
        if args.camera_control:
            args.camera_control.write_text('0\n')
        args.output.write_text(json.dumps(result, indent=2) + '\n')


def report(args):
    windows = json.loads(args.windows.read_text())
    if not windows['complete']:
        raise ValueError('Timed block recording is incomplete')
    with args.csv.open(newline='') as stream:
        header = stream.readline().strip()
        if header != f"# qpc_frequency={windows['qpc_frequency']}":
            raise ValueError('Present CSV QPC frequency differs from the recording')
        rows = [{key: int(value) for key, value in row.items()}
                for row in csv.DictReader(stream)]
    summaries = []
    mode_column = windows.get('mode_column', 'cpu_work')
    worker_modes = None
    if mode_column in ('spu_triplet_copy', 'native_cursor', 'spu_float_native', 'gpu_vertex_pull', 'job_wait', 'vp_packets', 'vp_consumer', 'job0_kernel', 'spu_worker_reuse', 'fifo_idle_wait'):
        if not args.queue_mode_log:
            raise ValueError('Native mode requires its process-wide mode-event log')
        worker_modes = {'process': []}
        for line in args.queue_mode_log.read_text(errors='replace').splitlines():
            name = {'spu_triplet_copy':'haze-spu-triplet-mode',
                    'native_cursor':'haze-native-cursor-mode',
                    'job0_kernel':'haze-job0-kernel-mode',
                    'spu_worker_reuse':'spu-worker-reuse-mode',
                    'spu_float_native':'spu-float-native-mode',
                    'gpu_vertex_pull':'gpu-vertex-pull-mode',
                    'job_wait':'haze-job-wait-mode',
                    'vp_packets':'haze-vp-packet-mode',
                    'vp_consumer':'rsx-vp-burst-mode',
                    'fifo_idle_wait':'fifo-idle-wait-mode'}[mode_column]
            valid_modes = '[012]' if mode_column == 'gpu_vertex_pull' else '[01]'
            pattern = (r'\[' + name + r'\] timeout_ms=(' + valid_modes + r') qpc=(\d+)'
                       if mode_column == 'fifo_idle_wait' else
                       r'\[' + name + r'\] enabled=(' + valid_modes + r') qpc=(\d+)')
            match = re.fullmatch(pattern, line)
            if match: worker_modes['process'].append((int(match[2]), int(match[1])))
        worker_modes['process'].sort()
    elif mode_column in ('spurs_queue_wait', 'spu_idle_wait', 'usleep', 'frame_pacer', 'fifo_recycle_wait'):
        if not args.queue_mode_log or not args.queue_wait_tid:
            raise ValueError('Native mode requires its log and explicitly selected critical host TIDs')
        worker_modes = {tid: [] for tid in args.queue_wait_tid}
        pattern = (r'\[spurs-queue-wait-mode\] tid=(\d+) native=([01]) qpc=(\d+)'
                   if mode_column == 'spurs_queue_wait' else
                   r'\[gcm-fifo-wait-mode\] tid=(\d+) native=([01]) qpc=(\d+)'
                   if mode_column == 'fifo_recycle_wait' else
                   r'\[haze-spu-idle-mode\] tid=(\d+) enabled=([01]) qpc=(\d+)'
                   if mode_column == 'spu_idle_wait' else
                   r'\[haze-frame-pacer-mode\] tid=(\d+) enabled=([01]) qpc=(\d+)'
                   if mode_column == 'frame_pacer' else
                   r'\[usleep-mode\] tid=(\d+) guest_tid=\d+ timer=([01]) qpc=(\d+)')
        for line in args.queue_mode_log.read_text(errors='replace').splitlines():
            match = re.fullmatch(pattern, line)
            if match and int(match[1]) in worker_modes:
                mode = int(match[2]) * (8 if mode_column == 'usleep' else 1)
                worker_modes[int(match[1])].append((int(match[3]), mode))
        for events in worker_modes.values():
            events.sort()
    elif mode_column not in ('cpu_work', 'shadow_compare', 'depth_export', 'window_position', 'depth_clear', 'vertex_cache', 'geometry_cache', 'vertex_workers'):
        raise ValueError('Unknown recorded mode column')
    for block in windows['blocks']:
        if not rows or rows[0]['qpc'] > block['start_qpc'] or rows[-1]['qpc'] < block['end_qpc']:
            raise ValueError('Present CSV does not cover the entire measured block; its fixed ring may have wrapped')
        if worker_modes is not None:
            for tid, events in worker_modes.items():
                before = [mode for qpc, mode in events if qpc <= block['start_qpc']]
                if not before or before[-1] != block['flags']:
                    raise ValueError(f'Native mode did not settle for {tid}')
                if any(block['start_qpc'] < qpc <= block['end_qpc'] for qpc, _ in events):
                    raise ValueError(f'Native mode changed inside the measured block for {tid}')
        selected = [(prev, cur) for prev, cur in zip(rows, rows[1:])
                    if prev['qpc'] >= block['start_qpc'] and cur['qpc'] <= block['end_qpc']]
        if len(selected) < args.min_frames:
            raise ValueError('Too few complete present intervals in a recorded block')
        for prev, cur in selected:
            if cur['present_id'] != prev['present_id'] + 1:
                raise ValueError('Missing present inside a recorded block')
            if worker_modes is None and (prev[mode_column] != block['flags'] or cur[mode_column] != block['flags']):
                raise ValueError('Mode did not settle before the measured block; increase warmup')
            if cur['qpc'] <= prev['qpc']:
                raise ValueError('Nonpositive present interval')
        intervals = [(cur['qpc'] - prev['qpc']) * 1000 / windows['qpc_frequency']
                     for prev, cur in selected]
        summaries.append(dict(block, frames=len(intervals),
            first_frame=selected[0][1]['guest_frame'], last_frame=selected[-1][1]['guest_frame'],
            fps=1000 / statistics.mean(intervals),
            frame_ms=dict(mean=statistics.mean(intervals), min=min(intervals),
                          p50=statistics.median(intervals),
                          p95=percentile(intervals, .95), p99=percentile(intervals, .99), max=max(intervals)),
            frames_over_33_333_ms=sum(value > 1000 / 30 for value in intervals),
            frames_over_50_ms=sum(value > 50 for value in intervals)))
    result = dict(source=str(args.csv), windows=str(args.windows), blocks=summaries,
                  scenario=windows.get('scenario', 'stationary, no injected input'), note=__doc__)
    if any(block.get('camera_token') is not None for block in windows['blocks']):
        if not getattr(args, 'camera_log', None):
            raise ValueError('Absolute camera benchmark requires its actual-angle audit')
        result['camera_audit'] = validate_camera_sweep(args.camera_log, windows)
    if getattr(args, 'process_log', None):
        result['process_observation'] = verify_process_observation(
            args.process_log, windows, getattr(args, 'max_process_gap', 4))
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    runner = actions.add_parser('record')
    runner.add_argument('control', type=Path, nargs='?')
    runner.add_argument('--fixed-mode', action='store_true',
                        help='Observe a fixed startup mode without writing a control file; reports still require native mode evidence')
    runner.add_argument('--pid', type=int, required=True)
    runner.add_argument('--exe', type=Path, required=True)
    runner.add_argument('--observe-pid', type=int, action='append', default=[],
                        help='Read CPU activity of another Haze instance without modifying it')
    runner.add_argument('--flags', nargs='+', type=int, default=[259,771,259,771,259,771])
    runner.add_argument('--mode-column', choices=['cpu_work','shadow_compare','depth_export','window_position','depth_clear','vertex_cache','geometry_cache','vertex_workers','spurs_queue_wait','spu_idle_wait','usleep','frame_pacer','spu_triplet_copy','native_cursor','fifo_recycle_wait','fifo_idle_wait','spu_float_native','gpu_vertex_pull','job_wait','vp_packets','vp_consumer','job0_kernel','spu_worker_reuse'], default='cpu_work',
                        help='CSV state to validate; set the matching renderer control-file environment variable')
    runner.add_argument('--warmup', type=float, default=5)
    runner.add_argument('--seconds', type=float, default=12)
    runner.add_argument('--output', type=Path, required=True)
    runner.add_argument('--scenario', default='stationary, no injected input',
                        help='Describe the externally driven scene/input trajectory and its limitations')
    runner.add_argument('--camera-control', type=Path,
                        help='HAZE_BENCHMARK_CAMERA_FILE: reset the absolute angle trajectory for every block')
    runner.add_argument('--camera-token-base', type=int, default=100,
                        help='First unique camera trajectory token for this recording')
    reader = actions.add_parser('report')
    reader.add_argument('csv', type=Path)
    reader.add_argument('windows', type=Path)
    reader.add_argument('--min-frames', type=int, default=180)
    reader.add_argument('--queue-mode-log', '--mode-event-log', dest='queue_mode_log', type=Path)
    reader.add_argument('--queue-wait-tid', '--mode-tid', dest='queue_wait_tid', type=int, action='append', default=[])
    reader.add_argument('--output', type=Path, required=True)
    reader.add_argument('--camera-log', type=Path,
                        help='Actual-angle CSV emitted beside HAZE_BENCHMARK_CAMERA_FILE')
    reader.add_argument('--process-log', type=Path,
                        help='JSONL from observe_haze_processes.ps1; reject observed Haze contention or incomplete coverage')
    reader.add_argument('--max-process-gap', type=float, default=4,
                        help='Maximum allowed seconds between observations, default 4')
    args = parser.parse_args()
    if args.action == 'record':
        if args.fixed_mode:
            if args.control is not None or len(set(args.flags)) != 1:
                parser.error('Fixed mode requires identical flags and no control-file argument')
        elif args.control is None:
            parser.error('Supply a control-file argument or use --fixed-mode')
        if args.warmup < 5 or args.seconds < 10 or any(not 0 <= f <= 16383 for f in args.flags):
            parser.error('Use at least 5 warmup seconds, 10 measured seconds, and flags 0..16383')
        if not 0 < args.camera_token_base <= 1000000-len(args.flags):
            parser.error('Camera trajectory tokens must fit the range 1..1000000')
        allowed_modes = ((0,1,2,3) if args.mode_column == 'vertex_workers' else
                         (0,1,2) if args.mode_column in ('window_position','gpu_vertex_pull') else
                         (0,8) if args.mode_column == 'usleep' else (0,1))
        if args.mode_column != 'cpu_work' and any(f not in allowed_modes for f in args.flags):
            parser.error('Use shadow/depth/queue flags 0/1, vertex-worker flags 0..3, window-position flags 0/1/2, or usleep flags 0/8')
        record(args)
    else:
        if not 0 < args.max_process_gap <= 10:
            parser.error('Process observation gap must be positive and at most 10 seconds')
        if args.min_frames < 60:
            parser.error('Use at least 60 complete intervals per block')
        report(args)
