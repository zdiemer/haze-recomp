"""Analyze retained slow FIFO-drain and optional RSX-flush diagnostics.

All joins use QPC intervals. These thresholded, bounded rings are diagnostic
events, not complete frame accounting. Thread-cycle deltas remain raw cycles.
"""
import argparse
import bisect
import csv
import json
import math
from pathlib import Path


FIFO_FIELDS = {
    'sequence', 'host_tid', 'renderer_frame', 'context_valid', 'qpc_begin',
    'qpc_lock_acquired', 'qpc_body_end', 'total_ms', 'lock_wait_ms',
    'body_ms', 'thread_cycles_valid', 'thread_cycles_delta', 'get_before',
    'put_before', 'get_after', 'put_after',
}
FIFO_PHASE_FIELDS = {
    'phase_mask', 'prewalk_setup_ticks', 'walker_ticks', 'postwalk_ticks',
    'recycle_ticks', 'post_recycle_ticks', 'drained_ea_publish_ticks',
    'get_write_ticks',
    'reference_lock_wait_ticks', 'reference_publish_ticks',
    'phase_unaccounted_ticks', 'stopped_reason',
}
FIFO_PHASE_TICKS = (
    'prewalk_setup_ticks', 'walker_ticks', 'postwalk_ticks', 'recycle_ticks',
    'post_recycle_ticks', 'drained_ea_publish_ticks', 'get_write_ticks',
    'reference_lock_wait_ticks',
    'reference_publish_ticks',
)
FLUSH_STAGES = (
    'close_ticks', 'execute_ticks', 'signal_ticks', 'completed_value_ticks',
    'set_event_ticks', 'wait_ticks', 'info_queue_ticks',
    'retired_release_ticks', 'alloc_reset_ticks', 'list_reset_ticks',
)
FLUSH_FIELDS = {
    'sequence', 'frame', 'host_tid', 'reason', 'qpc_begin', 'qpc_end',
    'total_ticks', 'thread_cycles_valid', 'thread_cycles_delta', 'fence_value',
    'completed_value', 'retired_count', 'waited', 'wait_result', 'wait_error',
    'stage_mask', 'close_hr', 'signal_hr', 'set_event_hr', 'alloc_reset_hr',
    'list_reset_hr', 'failed_stage', 'unaccounted_ticks', *FLUSH_STAGES,
}


def _metadata(line, expected):
    parts = next(csv.reader([line.removeprefix('#').strip()]))
    if len(parts) % 2:
        raise ValueError('Malformed diagnostic metadata')
    values = dict(zip(parts[::2], parts[1::2]))
    if values.get('qpc_frequency') is None:
        raise ValueError('Missing QPC frequency metadata')
    try:
        result = {k: int(v, 16) if v.lower().startswith('0x') else int(v, 10)
                  for k, v in values.items()}
    except ValueError as exc:
        raise ValueError('Invalid diagnostic metadata integer') from exc
    if result['qpc_frequency'] <= 0:
        raise ValueError('Invalid QPC frequency')
    for key in expected:
        if key not in result or result[key] < 0:
            raise ValueError(f'Missing or invalid metadata field: {key}')
    return result


def _read_capture(path, kind):
    path = Path(path)
    with path.open(newline='', encoding='utf-8-sig') as stream:
        metadata_line = stream.readline()
        if not metadata_line.startswith('#'):
            raise ValueError(f'{kind}: missing metadata row')
        metadata = _metadata(metadata_line, ('overwritten',))
        if kind == 'fifo':
            if 'recorded' not in metadata:
                raise ValueError('fifo: missing recorded metadata')
        else:
            if 'capacity' not in metadata or 'retained' not in metadata:
                raise ValueError('flush: missing capacity/retained metadata')
            metadata['recorded'] = metadata['retained'] + metadata['overwritten']
        reader = csv.DictReader(stream)
        if not reader.fieldnames:
            raise ValueError(f'{kind}: missing CSV header')
        rows = list(reader)
    required = FIFO_FIELDS if kind == 'fifo' else FLUSH_FIELDS
    if None in reader.fieldnames or not required.issubset(reader.fieldnames):
        raise ValueError(f'{kind}: missing or malformed CSV columns')
    if kind == 'fifo':
        phase_columns = set(reader.fieldnames) & FIFO_PHASE_FIELDS
        if phase_columns and phase_columns != FIFO_PHASE_FIELDS:
            raise ValueError('fifo: incomplete nested-phase schema')
    parsed = []
    for raw in rows:
        if None in raw or any(v is None for v in raw.values()):
            raise ValueError(f'{kind}: nonrectangular CSV')
        row = {}
        for key, value in raw.items():
            if key == 'stopped_reason':
                row[key] = value
            elif key.endswith('_hr'):
                row[key] = int(value, 16)
            else:
                try:
                    row[key] = (float(value) if key.endswith('_ms') else
                                int(value, 16) if value.lower().startswith('0x') else int(value))
                except ValueError as exc:
                    raise ValueError(f'{kind}: invalid value in {key}') from exc
        parsed.append(row)
    return metadata, parsed


def _validate_ring(metadata, rows, kind, capacity=None):
    recorded = metadata['recorded']
    overwritten = metadata['overwritten']
    if overwritten > recorded:
        raise ValueError(f'{kind}: overwritten count exceeds recorded count')
    if kind == 'fifo':
        capacity = 16384
    else:
        capacity = metadata['capacity'] if capacity is None else capacity
        if capacity <= 0 or metadata['capacity'] != capacity:
            raise ValueError('flush: invalid ring capacity')
    if len(rows) != min(recorded, capacity) or overwritten != max(0, recorded-capacity):
        raise ValueError(f'{kind}: retained/recorded/overwritten counts disagree')
    seqs = [r['sequence'] for r in rows]
    if any(s <= 0 for s in seqs) or len(seqs) != len(set(seqs)):
        raise ValueError(f'{kind}: invalid or duplicate sequence')
    # Ring insertion can complete out of sequence across host threads. Sequence
    # order itself is still monotonic; validate it without assuming CSV row order.
    return capacity


def _validate_fifo(metadata, rows):
    _validate_ring(metadata, rows, 'fifo')
    frequency = metadata['qpc_frequency']
    for r in rows:
        if r['sequence'] <= 0 or r['host_tid'] <= 0 or r['renderer_frame'] < 0:
            raise ValueError('fifo: invalid event identity')
        if r['context_valid'] not in (0, 1) or r['thread_cycles_valid'] not in (0, 1):
            raise ValueError('fifo: invalid boolean field')
        begin, locked, end = r['qpc_begin'], r['qpc_lock_acquired'], r['qpc_body_end']
        if not (0 < begin <= locked <= end):
            raise ValueError('fifo: invalid phase QPC intervals')
        if any(r[k] < 0 for k in ('total_ms', 'lock_wait_ms', 'body_ms', 'thread_cycles_delta')):
            raise ValueError('fifo: negative duration or cycle count')
        if not all(math.isfinite(r[k]) for k in ('total_ms', 'lock_wait_ms', 'body_ms')):
            raise ValueError('fifo: nonfinite duration')
        if not r['thread_cycles_valid'] and r['thread_cycles_delta'] != 0:
            raise ValueError('fifo: invalid cycle query has nonzero delta')
        scale = 1000.0/frequency
        if abs(r['total_ms']-(end-begin)*scale) > 0.000002:
            raise ValueError('fifo: total duration disagrees with QPC')
        if abs(r['lock_wait_ms']-(locked-begin)*scale) > 0.000002:
            raise ValueError('fifo: lock duration disagrees with QPC')
        if abs(r['body_ms']-(end-locked)*scale) > 0.000002:
            raise ValueError('fifo: body duration disagrees with QPC')
        if abs(r['total_ms']-r['lock_wait_ms']-r['body_ms']) > 0.000003:
            raise ValueError('fifo: phases do not partition total')
        phase_keys = set(r) & FIFO_PHASE_FIELDS
        if phase_keys:
            if phase_keys != FIFO_PHASE_FIELDS:
                raise ValueError('fifo: incomplete nested-phase schema')
            if r['phase_mask'] & ~0x1ff:
                raise ValueError('fifo: unknown nested phase-mask bits')
            phase_total = sum(r[key] for key in FIFO_PHASE_TICKS)
            body_ticks = end-locked
            if r['phase_unaccounted_ticks'] < 0 or any(r[key] < 0 for key in FIFO_PHASE_TICKS):
                raise ValueError('fifo: negative nested phase ticks')
            if phase_total+r['phase_unaccounted_ticks'] != body_ticks:
                raise ValueError('fifo: nested phases do not partition body')
            for bit, key in enumerate(FIFO_PHASE_TICKS):
                if not r['phase_mask'] & (1 << bit) and r[key] != 0:
                    raise ValueError('fifo: unvisited nested phase has nonzero ticks')
            if r['phase_mask'] and not r['stopped_reason']:
                raise ValueError('fifo: missing stopped reason')
    # Sequence is assigned while holding the FIFO lock, so lock-acquired QPC
    # must increase with sequence, though begin timestamps can reorder.
    ordered = sorted(rows, key=lambda r: r['sequence'])
    if any(a['qpc_lock_acquired'] > b['qpc_lock_acquired'] for a, b in zip(ordered, ordered[1:])):
        raise ValueError('fifo: sequence does not follow lock acquisition')


def _validate_flush(metadata, rows):
    _validate_ring(metadata, rows, 'flush')
    if not 0 < metadata.get('threshold_us', 0) <= 1000000:
        raise ValueError('flush: invalid threshold metadata')
    frequency = metadata['qpc_frequency']
    for r in rows:
        if r['sequence'] <= 0 or r['host_tid'] <= 0 or r['frame'] < 0:
            raise ValueError('flush: invalid event identity')
        if r['thread_cycles_valid'] not in (0, 1) or r['waited'] not in (0, 1):
            raise ValueError('flush: invalid boolean field')
        if r['qpc_begin'] <= 0 or r['qpc_end'] < r['qpc_begin']:
            raise ValueError('flush: invalid QPC interval')
        if r['total_ticks'] != r['qpc_end']-r['qpc_begin']:
            raise ValueError('flush: total ticks disagree with QPC')
        if any(r[k] < 0 for k in ('thread_cycles_delta', 'unaccounted_ticks', *FLUSH_STAGES)):
            raise ValueError('flush: negative stage or cycle count')
        if not r['thread_cycles_valid'] and r['thread_cycles_delta'] != 0:
            raise ValueError('flush: invalid cycle query has nonzero delta')
        if any(r[k] > r['total_ticks'] for k in (*FLUSH_STAGES, 'unaccounted_ticks')):
            raise ValueError('flush: stage exceeds event interval')
        if r['stage_mask'] & ~0x3ff:
            raise ValueError('flush: unknown stage-mask bits')
        if not 0 <= r['failed_stage'] <= 10:
            raise ValueError('flush: invalid failed-stage value')
        stages_total = sum(r[k] for k in FLUSH_STAGES)
        if stages_total + r['unaccounted_ticks'] != r['total_ticks']:
            raise ValueError('flush: stages and unaccounted ticks do not partition total')
        for bit, stage in enumerate(FLUSH_STAGES):
            if not r['stage_mask'] & (1 << bit) and r[stage] != 0:
                raise ValueError('flush: unvisited stage has nonzero ticks')
        wait_reached = bool(r['stage_mask'] & (1 << 5))
        if bool(r['waited']) != wait_reached:
            raise ValueError('flush: waited flag disagrees with wait stage')
    seqs = [r['sequence'] for r in rows]
    if any(s <= 0 for s in seqs) or any(b != a+1 for a, b in zip(seqs, seqs[1:])):
        raise ValueError('flush: event sequences are not contiguous in publication order')


def _stats(rows, value_key):
    values = sorted(r[value_key] for r in rows)
    if not values:
        return {'count': 0, 'mean_ms': None, 'p95_ms': None, 'max_ms': None}
    index = (len(values)-1)*0.95
    lo = int(index)
    p95 = values[lo] + (values[min(lo+1, len(values)-1)]-values[lo])*(index-lo)
    return {'count': len(values), 'mean_ms': sum(values)/len(values),
            'p95_ms': p95, 'max_ms': values[-1]}


def _event_summary(rows, frequency, kind):
    if kind == 'fifo':
        for row in rows:
            row['_total_ms'] = row['total_ms']
            row['_lock_ms'] = row['lock_wait_ms']
            row['_body_ms'] = row['body_ms']
            row['_begin'] = row['qpc_begin']
            row['_end'] = row['qpc_body_end']
            row['_lock_begin'] = row['qpc_begin']
            row['_lock_end'] = row['qpc_lock_acquired']
            row['_body_begin'] = row['qpc_lock_acquired']
            row['_body_end'] = row['qpc_body_end']
        durations = ('_total_ms', '_lock_ms', '_body_ms')
        labels = ('total', 'lock_wait', 'body')
    else:
        for row in rows:
            row['_total_ms'] = row['total_ticks']*1000/frequency
            row['_begin'] = row['qpc_begin']
            row['_end'] = row['qpc_end']
        durations, labels = ('_total_ms',), ('total',)
    worst = {}
    tails = {}
    for key, label in zip(durations, labels):
        worst[label] = sorted(rows, key=lambda r: r[key], reverse=True)[:10]
        tails[label] = {f'over_{limit}_ms': sum(r[key] > limit for r in rows)
                        for limit in (2, 5, 10, 25, 50, 100)}
    result = {'events': len(rows), 'tail_counts': tails,
              'worst': {label: [{k: v for k, v in r.items() if not k.startswith('_')}
                                for r in values] for label, values in worst.items()}}
    result['durations'] = {label: _stats(rows, key) for key, label in zip(durations, labels)}
    if kind == 'fifo' and rows and FIFO_PHASE_FIELDS.issubset(rows[0]):
        result['nested_phase_ticks'] = {
            phase: {'sum_raw_ticks': sum(r[phase] for r in rows),
                    'max_raw_ticks': max(r[phase] for r in rows),
                    'sum_ms': sum(r[phase] for r in rows)*1000/frequency}
            for phase in FIFO_PHASE_TICKS}
        result['nested_phase_mask_counts'] = {
            str(mask): sum(r['phase_mask'] == mask for r in rows)
            for mask in sorted({r['phase_mask'] for r in rows})}
    if kind == 'flush':
        result['stage_ticks'] = {
            stage: {'sum_raw_ticks': sum(r[stage] for r in rows),
                    'max_raw_ticks': max((r[stage] for r in rows), default=0),
                    'sum_ms': sum(r[stage] for r in rows)*1000/frequency}
            for stage in FLUSH_STAGES}
        result['unaccounted_ticks'] = {
            'sum_raw_ticks': sum(r['unaccounted_ticks'] for r in rows),
            'max_raw_ticks': max((r['unaccounted_ticks'] for r in rows), default=0),
            'sum_ms': sum(r['unaccounted_ticks'] for r in rows)*1000/frequency,
        }
        result['failure_counts'] = {
            'failed_stage': sum(r['failed_stage'] != 0 for r in rows),
            'failed_close_hr': sum(bool(r['close_hr'] & 0x80000000) for r in rows),
            'failed_signal_hr': sum(bool(r['signal_hr'] & 0x80000000) for r in rows),
            'failed_set_event_hr': sum(bool(r['set_event_hr'] & 0x80000000) for r in rows),
            'failed_alloc_reset_hr': sum(bool(r['alloc_reset_hr'] & 0x80000000) for r in rows),
            'failed_list_reset_hr': sum(bool(r['list_reset_hr'] & 0x80000000) for r in rows),
            'wait_errors': sum(r['wait_error'] != 0 for r in rows),
            'wait_failed': sum(r['waited'] and r['wait_result'] == 0xffffffff for r in rows),
        }
    return result


def _overlap(begin, end, lo, hi):
    left, right = max(begin, lo), min(end, hi)
    return None if right <= left else (left, right)


def _event_index(rows):
    ordered = sorted(rows, key=lambda r: (r['_begin'], r['_end']))
    starts, prefix_max_end = [], []
    maximum = 0
    for row in ordered:
        starts.append(row['_begin'])
        maximum = max(maximum, row['_end'])
        prefix_max_end.append(maximum)
    return ordered, starts, prefix_max_end


def _query_index(index, lo, hi):
    ordered, starts, prefix_max_end = index
    stop = bisect.bisect_left(starts, hi)
    i = stop - 1
    matches = []
    while i >= 0 and prefix_max_end[i] > lo:
        row = ordered[i]
        if row['_end'] > lo:
            matches.append(row)
        i -= 1
    return matches


def _union_ticks(intervals):
    intervals = sorted(intervals)
    if not intervals:
        return 0
    total, lo, hi = 0, *intervals[0]
    for begin, end in intervals[1:]:
        if begin > hi:
            total += hi-lo
            lo, hi = begin, end
        else:
            hi = max(hi, end)
    return total+hi-lo


def _join_events(fifo_rows, flush_rows):
    max_pairs = 100000
    pairs = []
    pair_count = same_thread_count = 0
    per_fifo = {r['sequence']: 0 for r in fifo_rows}
    per_flush = {r['sequence']: 0 for r in flush_rows}
    flush_index = _event_index(flush_rows)
    for fr in fifo_rows:
        for sr in _query_index(flush_index, fr['_begin'], fr['_end']):
            interval = _overlap(fr['qpc_begin'], fr['qpc_body_end'], sr['qpc_begin'], sr['qpc_end'])
            if interval:
                same_tid = fr['host_tid'] == sr['host_tid']
                fifo_contains = fr['qpc_begin'] <= sr['qpc_begin'] and sr['qpc_end'] <= fr['qpc_body_end']
                flush_contains = sr['qpc_begin'] <= fr['qpc_begin'] and fr['qpc_body_end'] <= sr['qpc_end']
                pair = {'fifo_sequence': fr['sequence'], 'flush_sequence': sr['sequence'],
                        'fifo_tid': fr['host_tid'], 'flush_tid': sr['host_tid'],
                        'overlap_begin_qpc': interval[0], 'overlap_end_qpc': interval[1],
                        'overlap_ticks': interval[1]-interval[0], 'same_tid': same_tid,
                        'same_thread_fifo_contains_flush': bool(same_tid and fifo_contains),
                        'same_thread_flush_contains_fifo': bool(same_tid and flush_contains)}
                pair_count += 1
                per_fifo[fr['sequence']] += 1
                per_flush[sr['sequence']] += 1
                same_thread_count += int(pair['same_thread_fifo_contains_flush'] or
                                         pair['same_thread_flush_contains_fifo'])
                if len(pairs) < max_pairs:
                    pairs.append(pair)
    return {'overlapping_pairs': pair_count, 'pairs': pairs,
            'pairs_omitted': pair_count-len(pairs),
            'fifo_event_matches': {str(k): v for k, v in per_fifo.items()},
            'flush_event_matches': {str(k): v for k, v in per_flush.items()},
            'same_thread_containment_pairs': same_thread_count}


def _window_join(lo, hi, fifo_index, flush_index, frequency, label):
    def events(index, kind):
        out = []
        intervals = []
        lock_intervals, body_intervals = [], []
        matches = _query_index(index, lo, hi)
        for r in matches:
            interval = _overlap(r['_begin'], r['_end'], lo, hi)
            if not interval:
                continue
            clipped = interval != (r['_begin'], r['_end'])
            intervals.append(interval)
            event = {'sequence': r['sequence'], 'host_tid': r['host_tid'],
                        'begin_qpc': interval[0], 'end_qpc': interval[1],
                        'clipped': clipped, 'overlap_ms': (interval[1]-interval[0])*1000/frequency}
            if kind == 'fifo':
                lock = _overlap(r['_lock_begin'], r['_lock_end'], lo, hi)
                body = _overlap(r['_body_begin'], r['_body_end'], lo, hi)
                if lock:
                    lock_intervals.append(lock)
                if body:
                    body_intervals.append(body)
                event['lock_wait_overlap_ms'] = ((lock[1]-lock[0])*1000/frequency) if lock else 0
                event['body_overlap_ms'] = ((body[1]-body[0])*1000/frequency) if body else 0
            out.append(event)
        return {'event_count': len(out), 'clipped_event_count': sum(e['clipped'] for e in out),
                'union_overlap_ms': _union_ticks(intervals)*1000/frequency,
                'phase_union_overlap_ms': ({
                    'lock_wait': _union_ticks(lock_intervals)*1000/frequency,
                    'body': _union_ticks(body_intervals)*1000/frequency,
                } if kind == 'fifo' else None),
                'events': sorted(out, key=lambda e: e['overlap_ms'], reverse=True)[:20]}
    return {'label': label, 'start_qpc': lo, 'end_qpc': hi,
            'fifo': events(fifo_index, 'fifo'), 'flush': events(flush_index, 'flush')}


def _pump_frames(rows, fifo_index, flush_index, frequency):
    result = []
    for pump in rows:
        lo, hi = pump['begin_qpc'], pump['end_qpc']
        f = _window_join(lo, hi, fifo_index, flush_index, frequency,
                         f'pump_frame_{pump["frame"]}')
        result.append({'pump_frame': pump['frame'], 'begin_qpc': lo, 'end_qpc': hi,
                       'pump_dt_ms': pump['dt_ms'], 'wait_ms': pump['wait_ms'],
                       'fifo_ms': pump['fifo_ms'], 'present_ms': pump['present_ms'],
                       'waits': pump['waits'], 'drains': pump['drains'],
                       'fifo': f['fifo'], 'flush': f['flush']})
    return result


def analyze(fifo_path, windows_path=None, pump_path=None, flush_path=None):
    fifo_meta, fifo = _read_capture(fifo_path, 'fifo')
    _validate_fifo(fifo_meta, fifo)
    frequency = fifo_meta['qpc_frequency']
    fifo_summary = _event_summary(fifo, frequency, 'fifo')
    fifo_index = _event_index(fifo)
    flush_meta, flush = ({}, [])
    if flush_path:
        flush_meta, flush = _read_capture(flush_path, 'flush')
        _validate_flush(flush_meta, flush)
        if flush_meta['qpc_frequency'] != frequency:
            raise ValueError('FIFO and flush QPC frequencies differ')
    flush_summary = _event_summary(flush, frequency, 'flush') if flush_path else None
    flush_index = _event_index(flush)
    pairs = _join_events(fifo, flush) if flush_path else None

    windows = []
    window_source = None
    if windows_path:
        data = json.loads(Path(windows_path).read_text(encoding='utf-8-sig'))
        if data.get('qpc_frequency') != frequency:
            raise ValueError('Present window QPC frequency mismatch')
        if not data.get('complete', False):
            raise ValueError('Present window manifest is incomplete')
        for i, block in enumerate(data.get('blocks', [])):
            lo, hi = int(block['start_qpc']), int(block['end_qpc'])
            if lo <= 0 or hi <= lo:
                raise ValueError('Invalid present window')
            windows.append(_window_join(lo, hi, fifo_index, flush_index, frequency,
                                        f'window_{i}_flags_{block.get("flags", "")}'))
        window_source = str(windows_path)

    pump = []
    pump_source = None
    if pump_path:
        from .summarize_pump import read_pump
        pump = read_pump(pump_path)
        if pump[0]['qpc_frequency'] != frequency:
            raise ValueError('Pump and FIFO QPC frequencies differ')
        pump_source = str(pump_path)
        pump = _pump_frames(pump, fifo_index, flush_index, frequency)
    worst_pump = sorted(pump, key=lambda r: r['pump_dt_ms'], reverse=True)[:20]
    selected_pump = [p for p in pump if any(_overlap(
        p['begin_qpc'], p['end_qpc'], w['start_qpc'], w['end_qpc']) for w in windows)]
    worst_pump_in_windows = sorted(selected_pump, key=lambda r: r['pump_dt_ms'], reverse=True)[:20]

    fifo_qpcs = [r['_begin'] for r in fifo] + [r['_end'] for r in fifo]
    flush_qpcs = ([r['_begin'] for r in flush] + [r['_end'] for r in flush]) if flush_path else []
    qpcs = fifo_qpcs + flush_qpcs
    fifo_extent = {'start_qpc': min((r['_begin'] for r in fifo), default=None),
                    'end_qpc': max((r['_end'] for r in fifo), default=None)}
    flush_extent = {'start_qpc': min((r['_begin'] for r in flush), default=None),
                    'end_qpc': max((r['_end'] for r in flush), default=None)} if flush_path else None
    coverage = []
    for window in windows:
        lo, hi = window['start_qpc'], window['end_qpc']
        fifo_inside = bool(fifo_qpcs and fifo_extent['start_qpc'] <= lo and fifo_extent['end_qpc'] >= hi)
        flush_inside = bool(flush_qpcs and flush_extent['start_qpc'] <= lo and flush_extent['end_qpc'] >= hi)
        inside = fifo_inside or flush_inside
        coverage.append({'start_qpc': lo, 'end_qpc': hi,
                         'inside_retained_event_extent': inside,
                         'fifo_inside_retained_event_extent': fifo_inside,
                         'flush_inside_retained_event_extent': flush_inside,
                         'retention_complete': fifo_meta['overwritten'] == 0 and
                             (not flush_path or flush_meta['overwritten'] == 0),
                         'interpretation': 'Extent only: thresholded event rings do not provide continuous time coverage.'})

    return {
        'note': ('Thresholded, bounded diagnostics only. FIFO total/lock/body are QPC scopes; '
                 'flush stage values are converted from raw QPC ticks using the CSV frequency. '
                 'Thread cycle deltas remain raw cycles and are never converted to time. '
                 'QPC overlap is association, not causation. Different TIDs are not treated as nested. '
                 'An unmatched FIFO drain and flush pair does not establish that no flush wait occurred: '
                 'the flush ring has its own threshold and can also be overwritten. No retained event '
                 'establishes neither absence of sub-threshold work nor absence of time outside these scopes.'),
        'qpc_frequency': frequency,
        'fifo_capture': {'source': str(fifo_path), 'recorded': fifo_meta['recorded'],
                         'retained': len(fifo), 'overwritten': fifo_meta['overwritten'],
                         'retained_extent': {'start_qpc': min((r['_begin'] for r in fifo), default=None),
                                             'end_qpc': max((r['_end'] for r in fifo), default=None)},
                         **fifo_summary},
        'flush_capture': ({'source': str(flush_path), 'recorded': flush_meta['recorded'],
                           'retained': len(flush), 'overwritten': flush_meta['overwritten'],
                           'threshold_us': flush_meta.get('threshold_us'),
                           **flush_summary} if flush_path else None),
        'fifo_flush_overlap': pairs,
        'windows_source': window_source, 'pump_source': pump_source,
        'pump_frames': pump, 'worst_pump_frames_entire_capture': worst_pump,
        'worst_pump_frames_in_windows': worst_pump_in_windows,
        'pump_coverage': {
            'frame_count': len(pump),
            'within_fifo_retained_event_extent': sum(
                bool(fifo_extent['start_qpc'] is not None and
                     fifo_extent['start_qpc'] <= p['begin_qpc'] and
                     fifo_extent['end_qpc'] >= p['end_qpc']) for p in pump),
            'within_flush_retained_event_extent': sum(
                bool(flush_extent and flush_extent['start_qpc'] is not None and
                     flush_extent['start_qpc'] <= p['begin_qpc'] and
                     flush_extent['end_qpc'] >= p['end_qpc']) for p in pump),
            'interpretation': 'Retained event extent is not continuous event coverage; thresholded rings are sparse.'},
        'windows': windows, 'coverage': coverage,
        'retained_event_extent_all_sources': {
            'start_qpc': min(qpcs) if qpcs else None,
            'end_qpc': max(qpcs) if qpcs else None,
            'fifo': fifo_extent, 'flush': flush_extent,
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fifo', type=Path)
    parser.add_argument('--windows', type=Path)
    parser.add_argument('--pump', type=Path)
    parser.add_argument('--flush', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = analyze(args.fifo, args.windows, args.pump, args.flush)
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8')
    print(text, end='')


if __name__ == '__main__':
    main()
