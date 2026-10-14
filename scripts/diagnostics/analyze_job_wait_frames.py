"""Join retained job waits to pump frames by QPC and summarize phase attribution."""
import argparse
from bisect import bisect_right
from collections import defaultdict
import json
from math import ceil
from pathlib import Path

from .analyze_job_trace import join_async_jobs, read_trace, union_ticks
from .summarize_pump import read_pump

PHASES = ('before_sdk_submit', 'worker_startup', 'worker_setup', 'abi_execution',
          'after_abi', 'unattributed')


def validate_retained_ring(events, summary):
    """Require a gap-free, non-dropped retained suffix and report its bounds."""
    required = ('tickets_before_retained_window', 'missing_tickets', 'dropped_busy',
                'dropped_stale', 'dropped_frozen', 'incomplete_scopes',
                'max_discarded_end_qpc', 'qpc_frequency')
    if any(key not in summary for key in required):
        raise ValueError('Trace summary lacks ring-retention evidence')
    if not events or events[0]['ticket'] != summary['tickets_before_retained_window']:
        raise ValueError('Retained ring start ticket does not match summary')
    if any(row['ticket'] != events[0]['ticket'] + index for index, row in enumerate(events)):
        raise ValueError('Retained ring contains a ticket gap')
    if summary['missing_tickets'] or any(summary[key] for key in
            ('dropped_busy', 'dropped_stale', 'dropped_frozen', 'incomplete_scopes')):
        raise ValueError('Trace ring reports missing, dropped, or incomplete spans')
    if any(row['parent_missing_in_dump'] for row in events):
        raise ValueError('Trace ring contains spans with missing parents')
    return {'retained_event_count': len(events), 'first_ticket': events[0]['ticket'],
            'last_ticket': events[-1]['ticket'],
            'tickets_before_retained_window': summary['tickets_before_retained_window'],
            'missing_tickets': summary['missing_tickets'],
            'dropped_busy': summary['dropped_busy'],
            'dropped_stale': summary['dropped_stale'],
            'dropped_frozen': summary['dropped_frozen'],
            'incomplete_scopes': summary['incomplete_scopes'],
            'parent_missing_in_dump': 0,
            'max_discarded_end_qpc': summary['max_discarded_end_qpc']}


def validate_trace_window_coverage(events, summary, lo, hi, frequency):
    if hi <= lo or summary['qpc_frequency'] != frequency:
        raise ValueError('Invalid window or trace/pump clock mismatch')
    if summary['max_discarded_end_qpc'] > lo:
        raise ValueError('Trace retention does not cover window start')
    return {'start_qpc': lo, 'end_qpc': hi, 'qpc_frequency': frequency,
            'discarded_end_qpc_precedes_window': True,
            'retained_ring': validate_retained_ring(events, summary)}


def attribute_wait_intervals(events, jobs, lo, hi):
    """Join once, return clipped disjoint phase intervals for validated waits."""
    by_base, by_key, publications = defaultdict(list), {}, defaultdict(list)
    for row in events:
        key = (row.get('slot'), row.get('generation'))
        if row['kind'] == 1 and row.get('slot_base') and key[0] < 512 and key[1]:
            if key in by_key:
                raise ValueError('Duplicate title slot generation')
            by_key[key] = row
            by_base[row['slot_base']].append(row)
        elif row['kind'] == 6 and row['arg1'] == 2:
            publications[key].append(row)

    starts = {}
    for base, rows in by_base.items():
        rows.sort(key=lambda row: row['begin_qpc'])
        starts[base] = [row['begin_qpc'] for row in rows]
        if len(set(starts[base])) != len(rows):
            raise ValueError('Ambiguous title submit ordering')

    matched = defaultdict(list)
    for phases in jobs.values():
        sdk = phases.get(7)
        if sdk is None or sdk['arg1'] >> 32 != 1:
            continue
        base = sdk['arg4']
        index = bisect_right(starts.get(base, []), sdk['begin_qpc']) - 1
        if index < 0:
            continue
        submit = by_base[base][index]
        if submit.get('effective_route') == 2:
            matched[(submit['slot'], submit['generation'])].append((submit, phases))

    records, skipped, waits_by_thread = [], defaultdict(int), defaultdict(list)
    for wait in events:
        if wait['kind'] != 4 or wait['begin_qpc'] >= hi or wait['end_qpc'] <= lo:
            continue
        key = (wait['slot'], wait['generation'])
        submit = by_key.get(key)
        candidates = matched.get(key, [])
        pubs = publications.get(key, [])
        reason = None
        if submit is None:
            reason = 'missing_title_submit'
        elif len(candidates) != 1:
            reason = 'missing_or_ambiguous_sdk_job'
        elif len(pubs) != 1:
            reason = 'missing_or_ambiguous_state2_publication'
        elif not all(kind in candidates[0][1] for kind in (7, 9, 10)):
            reason = 'missing_worker_phases'
        if reason:
            skipped[reason] += 1
            continue

        submit, phases = candidates[0]
        sdk, setup, execute = (phases[kind] for kind in (7, 9, 10))
        pub = pubs[0]
        base = submit['slot_base']
        next_index = bisect_right(starts[base], submit['begin_qpc'])
        next_submit = starts[base][next_index] if next_index < len(starts[base]) else float('inf')
        if not (submit['begin_qpc'] <= wait['begin_qpc'] <= wait['end_qpc'] <= next_submit
                and sdk['begin_qpc'] <= pub['begin_qpc'] <= pub['end_qpc'] <= wait['end_qpc']
                and pub.get('slot_base') == base
                and sdk['begin_qpc'] <= setup['begin_qpc'] <= setup['end_qpc'] <= execute['begin_qpc']):
            skipped['inconsistent_observed_lifetime'] += 1
            continue

        a, b = max(lo, wait['begin_qpc']), min(hi, wait['end_qpc'])
        if b <= a:
            continue
        segments = []
        covered = []
        for name, start, end in (
            ('before_sdk_submit', submit['begin_qpc'], sdk['begin_qpc']),
            ('worker_startup', sdk['begin_qpc'], setup['begin_qpc']),
            ('worker_setup', setup['begin_qpc'], setup['end_qpc']),
            ('abi_execution', execute['begin_qpc'], execute['end_qpc']),
            ('after_abi', execute['end_qpc'], wait['end_qpc'])):
            start, end = max(a, start), min(b, end)
            if end > start:
                segments.append((name, start, end))
                covered.append((start, end))

        cursor = a
        for start, end in sorted(covered):
            if start > cursor:
                segments.append(('unattributed', cursor, start))
            cursor = max(cursor, end)
        if cursor < b:
            segments.append(('unattributed', cursor, b))

        segments.sort(key=lambda segment: (segment[1], segment[2]))
        if any(left[2] > right[1] for left, right in zip(segments, segments[1:])):
            raise ValueError('Wait phase partition overlaps')
        if sum(end - start for _, start, end in segments) != b - a:
            raise ValueError('Wait phase partition does not conserve elapsed time')

        image = phases[7]['arg1'] & 0xffffffff
        record = {'wait_begin_qpc': a, 'wait_end_qpc': b, 'host_tid': wait['host_tid'],
                  'guest_thread': wait['guest_thread'], 'caller': wait['caller'],
                  'slot': wait['slot'], 'generation': wait['generation'],
                  'image': image, 'segments': segments}
        records.append(record)
        waits_by_thread[wait['host_tid']].append((a, b))

    for intervals in waits_by_thread.values():
        if union_ticks(intervals) != sum(end - start for start, end in intervals):
            raise ValueError('Matched waits overlap on one host thread')
    return records, dict(skipped)


def map_to_frames(records, pump_rows, lo, hi):
    frames = [row for row in pump_rows if lo <= row['begin_qpc'] < row['end_qpc'] <= hi]
    if not frames:
        raise ValueError('No complete pump frames in retained window')
    begins = [row['begin_qpc'] for row in frames]
    ends = [row['end_qpc'] for row in frames]
    if any(a['end_qpc'] != b['begin_qpc'] for a, b in zip(frames, frames[1:])):
        raise ValueError('Selected pump frames are not contiguous')

    per_frame = [[] for _ in frames]
    total_partition_ticks = 0
    mapped_partition_ticks = 0
    for record in records:
        for phase, start, end in record['segments']:
            total_partition_ticks += end - start
            index = bisect_right(ends, start)
            while index < len(frames) and begins[index] < end:
                clipped_start = max(start, begins[index])
                clipped_end = min(end, ends[index])
                if clipped_end > clipped_start:
                    per_frame[index].append((phase, record['image'], record['host_tid'],
                                             clipped_start, clipped_end))
                    mapped_partition_ticks += clipped_end - clipped_start
                index += 1

    output = []
    for row, segments in zip(frames, per_frame):
        ms_per_tick = 1000.0 / row['qpc_frequency']
        # Phase-specific unions are descriptive per-phase wall intervals and
        # can overlap if different host threads wait concurrently.
        phase_intervals = defaultdict(list)
        all_intervals = []
        abi_by_image = defaultdict(list)
        for phase, image, _tid, start, end in segments:
            phase_intervals[phase].append((start, end))
            all_intervals.append((start, end))
            if phase == 'abi_execution':
                abi_by_image[image].append((start, end))
        wait_union = union_ticks(all_intervals)
        phase_unions = {phase: union_ticks(items) for phase, items in phase_intervals.items()}
        # Derive disjoint single-phase wall time and overlap wall time with a
        # sweep. This prevents summing phases across simultaneous waiter threads.
        points = []
        for phase, _image, _tid, start, end in segments:
            points.append((start, 1, phase))
            points.append((end, -1, phase))
        points.sort(key=lambda item: (item[0], item[1]))
        active = defaultdict(int)
        cursor = points[0][0] if points else row['begin_qpc']
        single_phase = defaultdict(int)
        overlap_ticks = 0
        pos = 0
        while pos < len(points):
            tick = points[pos][0]
            active_phases = [phase for phase, count in active.items() if count > 0]
            width = tick - cursor
            if width > 0:
                if len(active_phases) == 1:
                    single_phase[active_phases[0]] += width
                elif len(active_phases) > 1:
                    overlap_ticks += width
            while pos < len(points) and points[pos][0] == tick:
                _, delta, phase = points[pos]
                active[phase] += delta
                if active[phase] == 0:
                    del active[phase]
                pos += 1
            cursor = tick
        if sum(single_phase.values()) + overlap_ticks != wait_union:
            raise ValueError('Per-frame phase sweep does not match wait union')

        output.append({
            'frame': row['frame'], 'begin_qpc': row['begin_qpc'], 'end_qpc': row['end_qpc'],
            'dt_ms': row['dt_ms'], 'wait_ms': row['wait_ms'], 'fifo_ms': row['fifo_ms'],
            'present_ms': row['present_ms'], 'other_ms': row['other_ms'],
            'thread_cpu_ms': row['thread_cpu_ms'] if row['cpu_valid'] else None,
            'observed_wait_union_ms': wait_union * ms_per_tick,
            'pump_interval_not_covered_by_observed_wait_ms':
                (row['end_qpc'] - row['begin_qpc'] - wait_union) * ms_per_tick,
            'exclusive_phase_wall_ms': {phase: single_phase.get(phase, 0) * ms_per_tick
                                        for phase in PHASES},
            'phase_union_wall_ms': {phase: phase_unions.get(phase, 0) * ms_per_tick
                                    for phase in PHASES},
            'overlap_across_phases_ms': overlap_ticks * ms_per_tick,
            'abi_union_wall_ms_by_image': {str(image): ticks * ms_per_tick
                                           for image, intervals in abi_by_image.items()
                                           if (ticks := union_ticks(intervals))},
        })

    return frames, output, {'attributed_phase_ticks_in_window': total_partition_ticks,
                            'attributed_phase_ticks_in_full_pump_frames': mapped_partition_ticks,
                            'attributed_phase_ticks_outside_full_pump_frames':
                                total_partition_ticks - mapped_partition_ticks}


def mean(rows, field):
    return sum(row[field] or 0 for row in rows) / len(rows) if rows else 0.0


def group_summary(rows):
    if not rows:
        return {'frames': 0}
    keys = ('dt_ms', 'wait_ms', 'fifo_ms', 'present_ms', 'other_ms',
            'observed_wait_union_ms', 'pump_interval_not_covered_by_observed_wait_ms',
            'overlap_across_phases_ms')
    out = {'frames': len(rows), 'means_ms': {key: mean(rows, key) for key in keys},
           'thread_cpu_valid_frames': sum(row['thread_cpu_ms'] is not None for row in rows),
           'thread_cpu_mean_ms': (sum(row['thread_cpu_ms'] for row in rows
                                      if row['thread_cpu_ms'] is not None) /
                                  sum(row['thread_cpu_ms'] is not None for row in rows)
                                  if any(row['thread_cpu_ms'] is not None for row in rows) else None)}
    for key in ('exclusive_phase_wall_ms', 'phase_union_wall_ms'):
        out[key] = {phase: sum(row[key][phase] for row in rows) / len(rows) for phase in PHASES}
    abi_images = sorted({image for row in rows for image in row['abi_union_wall_ms_by_image']},
                        key=lambda value: int(value))
    out['abi_union_wall_ms_by_image'] = {
        image: sum(row['abi_union_wall_ms_by_image'].get(image, 0.0) for row in rows) / len(rows)
        for image in abi_images}
    return out


def summarize_frames(frames):
    by_time = sorted(frames, key=lambda row: row['dt_ms'])
    count = len(by_time)
    slow5_count = max(1, ceil(count * 0.05))
    slow1_count = max(1, ceil(count * 0.01))
    typical_count = max(1, ceil(count * 0.50))
    typical = by_time[:typical_count]
    slow5 = by_time[-slow5_count:]
    slow1 = by_time[-slow1_count:]
    return {
        'frame_count': count,
        'typical_definition': 'fastest half by pump dt_ms; compared as a stable baseline, not a claim about a statistical mode',
        'typical_fastest_half': group_summary(typical),
        'slowest_5_percent': group_summary(slow5),
        'slowest_1_percent': group_summary(slow1),
        'slowest_frames': sorted(frames, key=lambda row: row['dt_ms'], reverse=True)[:12],
    }


def analyze(trace_path, windows_path, pump_path):
    events, trace_summary = read_trace(trace_path)
    windows = json.loads(Path(windows_path).read_text(encoding='utf-8-sig'))
    pump = read_pump(pump_path)
    frequency = trace_summary['qpc_frequency']
    if windows['qpc_frequency'] != frequency or pump[0]['qpc_frequency'] != frequency:
        raise ValueError('Trace, windows, and pump QPC frequencies differ')
    ring = validate_retained_ring(events, trace_summary)

    jobs = join_async_jobs(events)
    blocks = []
    for block_index, block in enumerate(windows['blocks']):
        lo, hi = block['start_qpc'], block['end_qpc']
        if hi <= lo or pump[0]['begin_qpc'] > lo or pump[-1]['end_qpc'] < hi:
            raise ValueError('Invalid window or pump does not cover it')
        coverage = validate_trace_window_coverage(events, trace_summary, lo, hi, frequency)
        records, skipped = attribute_wait_intervals(events, jobs, lo, hi)
        selected_pump, frame_rows, clipping = map_to_frames(records, pump, lo, hi)
        all_waits_in_window = [row for row in events if row['kind'] == 4 and row['begin_qpc'] < hi
                               and row['end_qpc'] > lo]
        blocks.append({
            'index': block_index, 'start_qpc': lo, 'end_qpc': hi,
            'qpc_frequency': frequency, 'complete_trace_coverage': True,
            'retention': coverage,
            'pump_frames': len(selected_pump),
            'first_pump_frame': selected_pump[0]['frame'],
            'last_pump_frame': selected_pump[-1]['frame'],
            'pump_rows_clipped_at_window_edges': sum(
                not (lo <= row['begin_qpc'] < row['end_qpc'] <= hi)
                for row in pump if row['begin_qpc'] < hi and row['end_qpc'] > lo),
            'observed_wait_spans_overlapping_window': len(all_waits_in_window),
            'validated_partitioned_wait_spans': len(records),
            'skipped_wait_spans': skipped,
            'clipping': clipping,
            'wait_host_threads': sorted({record['host_tid'] for record in records}),
            'per_frame': summarize_frames(frame_rows),
        })
    return {
        'trace': str(trace_path), 'windows': str(windows_path), 'pump': str(pump_path),
        'retained_trace_events': len(events),
        'retained_ring': ring,
        'note': ('All phase values are QPC wall-time intervals from matched title waits. '
                 'Phases partition each wait; phase unions can overlap across different host threads, '
                 'reported separately. Pump scope times are disjoint only within the pump thread. '
                 'The pump-interval residual means time not covered by observed matched waits in QPC; '
                 'it is not CPU, unblocked execution, or a removable-cost estimate. Thread CPU is the '
                 'pump CSV Windows accounting and may be quantized. '
                 'Results describe this instrumented capture only, not ordinary-build FPS.'),
        'blocks': blocks,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--windows', required=True, type=Path)
    parser.add_argument('--pump', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.trace, args.windows, args.pump)
    text = json.dumps(result, indent=2) + '\n'
    args.output.write_text(text, encoding='utf-8')
    print(text)


if __name__ == '__main__':
    main()
