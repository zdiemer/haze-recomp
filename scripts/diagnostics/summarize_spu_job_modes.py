"""Compare completed SPU jobs using thread CPU cycles in recorded mode windows.

Use a separate diagnostic with SPU_JOB_TIMING=1 and SPU_JOB_TIMING_IMAGE set.
Logging perturbs scheduling: this is not an ordinary-build FPS benchmark.
CPU cycles exclude time descheduled but depend on CPU frequency/core placement.
Workload changes and concurrent processes can still affect the comparison.
Only jobs whose complete setup/execution falls inside a stable mode window
are included; persistent or boundary-crossing jobs are not represented.
"""
import argparse
import json
from pathlib import Path
import re
import statistics

from .summarize_presents import percentile


def summarize(lines, windows, image, min_jobs=20):
    if not windows.get('complete') or windows.get('mode_column') != 'native_cursor':
        raise ValueError('Complete native_cursor windows are required')
    frequency = windows['qpc_frequency']
    if frequency <= 0 or not windows['blocks']:
        raise ValueError('Positive QPC frequency and measurement blocks are required')
    events, jobs = [], []
    for line in lines:
        event = re.fullmatch(r'\[haze-native-cursor-mode\] enabled=([01]) qpc=(\d+)\s*', line)
        if event:
            events.append((int(event[2]), int(event[1])))
        if not line.startswith('[spu-job-time] '):
            continue
        fields = {key: int(value, 16 if key == 'taskset' else 10)
                  for key, value in re.findall(r'(\w+)=([0-9A-Fa-f]+)', line)}
        if fields.get('image') != image:
            continue
        required = ('begin_qpc', 'execute_qpc', 'end_qpc', 'setup_cycles', 'run_cycles')
        if any(key not in fields for key in required):
            raise ValueError('Incomplete selected-image job counters')
        if not 0 <= fields['begin_qpc'] <= fields['execute_qpc'] <= fields['end_qpc']:
            raise ValueError('Nonchronological job counters')
        if fields['run_cycles'] <= 0:
            raise ValueError('Selected job has no measured CPU cycles')
        jobs.append(fields)
    events.sort()
    output = []
    previous_end = -1
    for block in windows['blocks']:
        begin, end, mode = block['start_qpc'], block['end_qpc'], block['flags']
        if mode not in (0, 1) or begin < previous_end or end <= begin:
            raise ValueError('Invalid or overlapping measurement windows')
        previous_end = end
        before = [flag for qpc, flag in events if qpc <= begin]
        if not before or before[-1] != mode:
            raise ValueError('Native cursor mode did not settle before measurement')
        if any(begin < qpc <= end for qpc, _ in events):
            raise ValueError('Native cursor mode changed during measurement')
        selected = [row for row in jobs if begin <= row['begin_qpc'] and row['end_qpc'] <= end]
        if len(selected) < min_jobs:
            raise ValueError('Too few complete jobs in a measurement window')
        cycles = [row['run_cycles'] for row in selected]
        wall = [(row['end_qpc'] - row['execute_qpc']) * 1000 / frequency for row in selected]
        output.append(dict(
            flags=mode, start_qpc=begin, end_qpc=end, jobs=len(selected),
            total_run_cycles=sum(cycles), mean_run_cycles=statistics.mean(cycles),
            median_run_cycles=statistics.median(cycles), p95_run_cycles=percentile(cycles, .95),
            mean_setup_cycles=statistics.mean(row['setup_cycles'] for row in selected),
            mean_run_wall_ms=statistics.mean(wall), p95_run_wall_ms=percentile(wall, .95)))
    return dict(image=image, qpc_frequency=frequency, blocks=output, note=__doc__)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('windows', type=Path)
    parser.add_argument('--image', type=int, required=True)
    parser.add_argument('--min-jobs', type=int, default=20)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.min_jobs < 20:
        parser.error('Require at least 20 complete jobs per window')
    result = summarize(args.log.read_text(errors='replace').splitlines(),
                       json.loads(args.windows.read_text(encoding='utf-8-sig')),
                       args.image, args.min_jobs)
    result.update(log=str(args.log), windows=str(args.windows))
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
