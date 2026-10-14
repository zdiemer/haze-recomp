"""Summarize diagnostic Haze job spans without adding nested elapsed times."""
import argparse
from bisect import bisect_right
import csv
import json
from collections import defaultdict
from pathlib import Path

KINDS = {1: 'submit', 2: 'dispatch_including_trampolines', 3: 'transform_kernel',
         4: 'completion_wait', 5: 'transform_handler', 6: 'state_publication',
         7: 'spu_submit_point', 8: 'spu_create_thread', 9: 'spu_setup',
         10: 'spu_execute', 11: 'spu_finish', 12: 'spu_cleanup',
         13: 'spu_create_failed', 14: 'spu_setup_failed', 15: 'spu_reuse_worker',
         16: 'spu_dispatch_discovery'}

DISPATCH_CAUSES = {0: 'unknown', 1: 'create', 2: 'task_exit', 3: 'poll', 4: 'ready_wake'}


def union_ticks(intervals):
    total = 0
    end = None
    for lo, hi in sorted(intervals):
        total += max(0, hi - max(lo, end if end is not None else lo))
        end = max(hi, end if end is not None else hi)
    return total


def read_trace(path):
    events, summary = [], None
    with Path(path).open(newline='') as stream:
        for raw in csv.DictReader(stream):
            if None in raw or any(v is None for v in raw.values()):
                raise ValueError('Nonrectangular trace CSV')
            kind = raw.pop('record_type')
            row = {k: int(v) if v else 0 for k, v in raw.items()}
            if kind == 'summary':
                if summary is not None:
                    raise ValueError('Repeated summary')
                summary = row
            elif kind == 'event':
                if summary is not None or row['end_qpc'] < row['begin_qpc'] or row['kind'] not in KINDS:
                    raise ValueError('Invalid span')
                if events and row['ticket'] <= events[-1]['ticket']:
                    raise ValueError('Nonmonotonic completion tickets')
                events.append(row)
            else:
                raise ValueError('Unknown trace record')
    if not events or summary is None or summary['qpc_frequency'] <= 0:
        raise ValueError('Missing events, summary, or clock frequency')
    by_id = {row['span_id']: row for row in events}
    if len(by_id) != len(events):
        raise ValueError('Repeated span ID')
    for row in events:
        parent = by_id.get(row['parent_id'])
        if bool(row['parent_missing_in_dump']) != bool(row['parent_id'] and parent is None):
            raise ValueError('Parent retention annotation mismatch')
        if parent and (row['host_tid'] != parent['host_tid'] or row['depth'] != parent['depth'] + 1
                       or not parent['begin_qpc'] <= row['begin_qpc'] <= row['end_qpc'] <= parent['end_qpc']):
            raise ValueError('Invalid nested span')
    return events, summary


def join_async_jobs(events):
    """Join immutable SDK dispatch identities, allowing incomplete ring history."""
    jobs = defaultdict(dict)
    for row in events:
        if row['kind'] < 7:
            continue
        job_id = row['arg0']
        if not job_id or row['kind'] in jobs[job_id]:
            raise ValueError('Invalid or duplicate SDK job phase')
        previous = next(iter(jobs[job_id].values()), None)
        identity_args = 4 if row['kind'] == 16 or (previous and previous['kind'] == 16) else 8
        if previous and any(previous[f'arg{i}'] != row[f'arg{i}'] for i in range(identity_args)):
            raise ValueError('SDK snapshot identity changed across phases')
        jobs[job_id][row['kind']] = row
    for phases in jobs.values():
        # Discovery carries cause/claim time instead of R3 in args 4..7.
        # Validate all ordinary phase snapshots independently, including when
        # discovery is the first retained event for this job.
        ordinary = [row for kind, row in phases.items() if kind != 16]
        if ordinary and any(any(row[f'arg{i}'] != ordinary[0][f'arg{i}'] for i in range(8))
                            for row in ordinary[1:]):
            raise ValueError('SDK snapshot identity changed across phases')
        discovery = phases.get(16)
        if discovery:
            if (discovery['arg4'] not in DISPATCH_CAUSES or
                    discovery['arg5'] != discovery['begin_qpc'] or
                    discovery['arg6'] > discovery['begin_qpc'] or discovery['arg7']):
                raise ValueError('Invalid SDK dispatch provenance')
            submit = phases.get(7)
            if submit and (discovery['end_qpc'] != submit['begin_qpc'] or
                           discovery['host_tid'] != submit['host_tid']):
                raise ValueError('SDK discovery/submit endpoint or thread mismatch')
        if 8 in phases and 13 in phases:
            raise ValueError('SDK job reports both successful and failed creation')
        if 15 in phases and 13 in phases:
            raise ValueError('SDK job reports both worker reuse and failed creation')
        if 8 in phases and 15 in phases:
            raise ValueError('SDK job reports both thread creation and worker reuse')
        if 14 in phases and 10 in phases:
            raise ValueError('Failed setup reports execution')
        for before, after in ((9, 10), (10, 11), (11, 12)):
            if before in phases and after in phases:
                a, b = phases[before], phases[after]
                if a['host_tid'] != b['host_tid'] or a['end_qpc'] > b['begin_qpc']:
                    raise ValueError('SDK worker phase order/thread mismatch')
        # A worker may finish before CreateThread returns; do not order it after
        # the submitting thread's end timestamp.
        startup = phases.get(8, phases.get(15))
        if startup and 9 in phases and phases[9]['begin_qpc'] < startup['begin_qpc']:
            raise ValueError('SDK worker started before thread creation began')
    return jobs


def summarize_async(jobs, lo, hi, scale, frames):
    images = defaultdict(lambda: {'phases': defaultdict(list), 'startup_gap_ms': []})
    for phases in jobs.values():
        any_row = next(iter(phases.values()))
        image = any_row['arg1'] & 0xffffffff
        for kind, row in phases.items():
            if row['begin_qpc'] < hi and row['end_qpc'] >= lo:
                images[image]['phases'][kind].append(row)
        startup = phases.get(8, phases.get(15))
        if startup and 9 in phases:
            setup = phases[9]
            if lo <= startup['begin_qpc'] <= setup['begin_qpc'] <= hi:
                images[image]['startup_gap_ms'].append(
                    (setup['begin_qpc'] - startup['begin_qpc']) * scale)
    result = []
    for image, data in images.items():
        phases = {}
        for kind, rows in data['phases'].items():
            clipped = sum(min(hi, r['end_qpc']) - max(lo, r['begin_qpc']) for r in rows) * scale
            full = [(r['end_qpc'] - r['begin_qpc']) * scale for r in rows
                    if lo <= r['begin_qpc'] <= r['end_qpc'] <= hi]
            phases[KINDS[kind]] = {'spans': len(rows), 'clipped_elapsed_ms_per_frame': clipped / frames,
                                   'full_span_mean_ms': sum(full) / len(full) if full else None,
                                   'full_span_max_ms': max(full, default=None)}
        gaps = data['startup_gap_ms']
        result.append({'image': image, 'phases': phases, 'startup_samples': len(gaps),
                       'startup_gap_mean_ms': sum(gaps) / len(gaps) if gaps else None,
                       'startup_gap_max_ms': max(gaps, default=None)})
    return sorted(result, key=lambda r: r['phases'].get('spu_execute', {}).get('clipped_elapsed_ms_per_frame', 0), reverse=True)


def summarize_wait_phases(events, jobs, lo, hi, scale, frames):
    """Observed slot/time join, not an SDK-carried title generation or CPU time.

    Only fully contained waits with one retained submit, SDK job, and state-2
    publication are partitioned. Never silently reuse an older slot generation.
    """
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
        rows.sort(key=lambda r: r['begin_qpc'])
        starts[base] = [r['begin_qpc'] for r in rows]
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
            matched[(submit['slot'], submit['generation'])].append(phases)
    groups, skipped, matched_waits = {}, defaultdict(int), defaultdict(list)
    phase_names = ('before_sdk_submit', 'worker_startup', 'worker_setup',
                   'abi_execution', 'after_abi', 'unattributed')
    for wait in events:
        if wait['kind'] != 4 or not lo <= wait['begin_qpc'] <= wait['end_qpc'] <= hi:
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
        elif not all(k in candidates[0] for k in (7, 9, 10)):
            reason = 'missing_worker_phases'
        if reason:
            skipped[reason] += 1
            continue
        phases = candidates[0]
        sdk, setup, execute = (phases[k] for k in (7, 9, 10))
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
        # Publication may precede the end of the ABI function's epilogue.
        identity = (wait['caller'], wait['guest_thread'], wait['host_tid'], submit['arg0'], sdk['arg1'] & 0xffffffff)
        if identity not in groups:
            groups[identity] = {'waits': [], **{name: [] for name in phase_names}}
        group = groups[identity]
        a, b = wait['begin_qpc'], wait['end_qpc']
        discovery = phases.get(16)
        provenance = group.setdefault('discovery', defaultdict(lambda: {
            'waits': 0, 'before_claim': [], 'claim_to_submit': [],
            'observed_ready_waits': 0, 'before_ready': [], 'ready_to_claim': [],
            'unknown_ready_before_claim': []}))
        cause = DISPATCH_CAUSES[discovery['arg4']] if discovery else 'missing'
        bucket = provenance[cause]
        bucket['waits'] += 1
        if discovery:
            ready = discovery['arg6']
            bucket['observed_ready_waits'] += bool(ready)
            readiness_segments = (
                ('before_ready', submit['begin_qpc'], ready),
                ('ready_to_claim', ready, discovery['begin_qpc'])) if ready else (
                ('unknown_ready_before_claim', submit['begin_qpc'], discovery['begin_qpc']),)
            for name, start, end in readiness_segments:
                start, end = max(a, start), min(b, end)
                if end > start:
                    bucket[name].append((start, end))
            for name, start, end in (
                    ('before_claim', submit['begin_qpc'], discovery['begin_qpc']),
                    ('claim_to_submit', discovery['begin_qpc'], sdk['begin_qpc'])):
                start, end = max(a, start), min(b, end)
                if end > start:
                    bucket[name].append((start, end))
        group['waits'].append((a, b))
        matched_waits[wait['host_tid']].append((a, b))
        segments = (
            ('before_sdk_submit', submit['begin_qpc'], sdk['begin_qpc']),
            ('worker_startup', sdk['begin_qpc'], setup['begin_qpc']),
            ('worker_setup', setup['begin_qpc'], setup['end_qpc']),
            ('abi_execution', execute['begin_qpc'], execute['end_qpc']),
            ('after_abi', execute['end_qpc'], b))
        covered = []
        for name, start, end in segments:
            start, end = max(a, start), min(b, end)
            if end > start:
                group[name].append((start, end))
                covered.append((start, end))
        cursor = a
        for start, end in sorted(covered):
            if start > cursor:
                group['unattributed'].append((cursor, start))
            cursor = max(cursor, end)
        if cursor < b:
            group['unattributed'].append((cursor, b))
    for intervals in matched_waits.values():
        if union_ticks(intervals) != sum(b - a for a, b in intervals):
            raise ValueError('Overlapping attributed waits on one host thread')
    result = []
    for (caller, guest, host, job_key, image), group in groups.items():
        # Overlapping waits could attribute one interval to different jobs.
        if union_ticks(group['waits']) != sum(b - a for a, b in group['waits']):
            raise ValueError('Overlapping waits in an attribution group')
        result.append({'caller': f'0x{caller:08X}', 'guest_thread': guest, 'host_tid': host,
                       'job_key': job_key, 'image': image, 'waits': len(group['waits']),
                       'wait_ms_per_frame': union_ticks(group['waits']) * scale / frames,
                       'phase_ms_per_frame': {name: union_ticks(group[name]) * scale / frames
                                              for name in phase_names},
                       'dispatch_provenance': {
                           cause: {'waits': bucket['waits'],
                                   'observed_ready_waits': bucket['observed_ready_waits'],
                                   'before_ready_wait_ms_per_frame': union_ticks(bucket['before_ready']) * scale / frames,
                                   'ready_to_claim_wait_ms_per_frame': union_ticks(bucket['ready_to_claim']) * scale / frames,
                                   'unknown_ready_before_claim_wait_ms_per_frame': union_ticks(bucket['unknown_ready_before_claim']) * scale / frames,
                                   'before_claim_wait_ms_per_frame': union_ticks(bucket['before_claim']) * scale / frames,
                                   'claim_to_submit_wait_ms_per_frame': union_ticks(bucket['claim_to_submit']) * scale / frames}
                           for cause, bucket in group['discovery'].items()}})
    return {'note': 'Exact slot address plus latest preceding title submit, observed generation, unique SDK job and state-2 publication. Wall-time overlaps are not CPU time or removable savings. Only fully contained, validated waits are partitioned.',
            'skipped_waits': dict(skipped),
            'groups': sorted(result, key=lambda r: r['wait_ms_per_frame'], reverse=True)}


def summarize_window(events, summary, block, frames):
    lo, hi = block['start_qpc'], block['end_qpc']
    if hi <= lo or frames <= 0:
        raise ValueError('Invalid measurement window')
    scale = 1000 / summary['qpc_frequency']
    selected = [r for r in events if r['begin_qpc'] < hi and r['end_qpc'] > lo]
    groups = {}
    for kind, name in KINDS.items():
        rows = [r for r in selected if r['kind'] == kind]
        by_thread = defaultdict(list)
        for row in rows:
            by_thread[row['host_tid']].append((max(lo, row['begin_qpc']), min(hi, row['end_qpc'])))
        union_ms = sum(union_ticks(v) for v in by_thread.values()) * scale
        groups[name] = {
            'overlapping_spans': len(rows),
            'fully_contained_spans': sum(lo <= r['begin_qpc'] <= r['end_qpc'] <= hi for r in rows),
            'clipped_thread_union_ms': union_ms,
            'clipped_thread_union_ms_per_frame': union_ms / frames,
            'max_full_span_ms': max(((r['end_qpc'] - r['begin_qpc']) * scale for r in rows
                                     if lo <= r['begin_qpc'] <= r['end_qpc'] <= hi), default=0),
        }
    dispatches = defaultdict(list)
    for row in selected:
        if row['kind'] == 2:
            dispatches[(row['dispatch_code'], row['dispatch_toc'])].append(row)
    targets = []
    for (code, toc), rows in dispatches.items():
        targets.append({'code': f'0x{code:08X}', 'toc': f'0x{toc:08X}', 'spans': len(rows),
                        'inclusive_clipped_ms': sum(min(hi, r['end_qpc']) - max(lo, r['begin_qpc'])
                                                    for r in rows) * scale})
    # Completed spans only. Unfinished work and discarded intervals prevent a completeness claim.
    cutoff = summary['max_discarded_end_qpc']
    complete = cutoff <= lo and summary['incomplete_scopes'] == 0
    submissions = {(r['slot'], r['generation']) for r in events
                   if r['kind'] == 1 and r['slot'] < 512 and r['generation']}
    waits = [r for r in selected if r['kind'] == 4]
    routes = defaultdict(int)
    for row in selected:
        if row['kind'] == 1:
            routes[row.get('effective_route', 0xffffffff)] += 1
    callers = defaultdict(list)
    for row in waits:
        callers[(row['caller'], row['guest_thread'], row['host_tid'])].append(row)
    wait_callers = []
    for (caller, guest_thread, host_tid), rows in callers.items():
        elapsed = union_ticks((max(lo, r['begin_qpc']), min(hi, r['end_qpc'])) for r in rows) * scale
        wait_callers.append({'caller': f'0x{caller:08X}', 'guest_thread': guest_thread,
                             'host_tid': host_tid, 'spans': len(rows),
                             'clipped_union_ms_per_frame': elapsed / frames})
    async_jobs = join_async_jobs(events)
    return {'start_qpc': lo, 'end_qpc': hi, 'frames': frames,
            'completed_span_coverage': complete,
            'discarded_end_qpc_max': cutoff, 'incomplete_scopes_at_dump': summary['incomplete_scopes'],
            'groups': groups,
            'submit_effective_routes': dict(routes),
            'async_images': summarize_async(async_jobs, lo, hi, scale, frames),
            'wait_phase_attribution': summarize_wait_phases(events, async_jobs, lo, hi, scale, frames),
            'dispatch_targets': sorted(targets, key=lambda r: r['inclusive_clipped_ms'], reverse=True),
            'wait_callers': sorted(wait_callers, key=lambda r: r['clipped_union_ms_per_frame'], reverse=True),
            'waits_with_retained_submit_generation': sum((r['slot'], r['generation']) in submissions for r in waits)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--windows', required=True, type=Path)
    parser.add_argument('--pump', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    from .summarize_pump import read_pump
    events, summary = read_trace(args.trace)
    windows = json.loads(args.windows.read_text(encoding='utf-8-sig'))
    pump = read_pump(args.pump)
    if summary['qpc_frequency'] != windows['qpc_frequency'] or summary['qpc_frequency'] != pump[0]['qpc_frequency']:
        raise ValueError('Trace/window/pump clock mismatch')
    blocks = []
    for block in windows['blocks']:
        lo, hi = block['start_qpc'], block['end_qpc']
        if pump[0]['begin_qpc'] > lo or pump[-1]['end_qpc'] < hi:
            raise ValueError('Pump does not cover the window')
        frames = sum(lo <= r['begin_qpc'] < r['end_qpc'] <= hi for r in pump)
        blocks.append(summarize_window(events, summary, block, frames))
    result = {'source': str(args.trace), 'retained_spans': len(events), 'summary': summary,
              'note': 'Diagnostic wall time, not CPU time. Groups overlap: never add submit, dispatch, handler, and kernel. Thread unions remove nesting within each kind, but separate threads can overlap. Dispatch includes trampoline completion. Generation matching is an observed native snapshot, not proof of a guest dependency. Incomplete coverage gives lower bounds only.',
              'blocks': blocks}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
