"""Validate filtered draw-callback timings; raw thread cycles are not CPU ms."""
import argparse
import csv
import json
import math
import re
from pathlib import Path

STAGES = ('geometry_preparation_ms', 'pipeline_constants_ms',
          'resource_resolution_ms', 'command_recording_ms')
FLOATS = ('wall_ms', *STAGES)
MASKS = ('texture_mask', 'vertex_texture_mask')
PSO_LOOKUP_UNCALLED = 0
PSO_LOOKUP_HIT = 1
PSO_LOOKUP_MISS = 2
PSO_DETAIL_PHASES = {'build_pso_ms', 'shader_disk_load_ms',
                     'd3d_compile_ms', 'create_graphics_pso_ms'}
RESOURCE_KINDS = ('surface', 'depth', 'texture', 'vertex_texture')
RESOURCE_DETAIL_PHASES = {'resource_residual_ms'} | {
    f'{kind}_{field}'
    for kind in RESOURCE_KINDS
    for field in ('calls', 'total_ms', 'max_ms', 'max_location', 'max_offset',
                  'max_format', 'max_width', 'max_height', 'max_slot')
}
RESOURCE_ELAPSED_FIELDS = {'resource_residual_ms'} | {
    f'{kind}_{field}' for kind in RESOURCE_KINDS
    for field in ('total_ms', 'max_ms')
}
TEXTURE_DETAIL_PHASES = {
    'texture_slot_lookup_ms', 'texture_slot_watch_ms', 'texture_slot_hash_ms',
    'texture_slot_decode_ms', 'texture_slot_srv_ms', 'texture_slot_retire_ms',
    'texture_slot_log_ms', 'texture_slot_residual_ms',
    'texture_resource_create_ms',
}
TEXTURE_DETAIL_FLAGS = {
    'texture_outcome', 'texture_decode_attempted', 'texture_decode_succeeded', 'texture_watch_skipped',
    'texture_cache_full', 'texture_resource_create_calls',
}
TEXTURE_SKIP_DETAIL_FIELDS = {
    'texture_skip_evaluated', 'texture_skip_override_changed',
    'texture_skip_watch_valid', 'texture_skip_entry_valid',
    'texture_skip_stamp_match', 'texture_skip_hash_kind_match',
    'texture_skip_validation', 'texture_skip_rejection_mask',
    'texture_source_span', 'texture_mipmaps', 'texture_hash_kind',
    'texture_hash_cycle_attempts', 'texture_hash_cycle_successes',
    'texture_hash_cycle_failures', 'texture_hash_cycle_valid',
    'texture_hash_cycle_delta_not_ms',
}
TEXTURE_SKIP_BOOLEAN_FIELDS = {
    'texture_skip_evaluated', 'texture_skip_override_changed',
    'texture_skip_watch_valid', 'texture_skip_entry_valid',
    'texture_skip_stamp_match', 'texture_skip_hash_kind_match',
    'texture_skip_validation', 'texture_hash_cycle_valid',
}
ELAPSED_FIELDS = (set(FLOATS) | {'get_pso_ms'} | PSO_DETAIL_PHASES |
                  RESOURCE_ELAPSED_FIELDS | TEXTURE_DETAIL_PHASES)


def validate_rows(rows):
    previous_id = 0
    frequency = None
    for row in rows:
        if any(not math.isfinite(row[key]) or row[key] < 0 for key in FLOATS):
            raise ValueError('Invalid elapsed time')
        if row['callback_id'] <= previous_id:
            raise ValueError('Nonmonotonic callback IDs')
        previous_id = row['callback_id']
        if row['qpc_frequency'] <= 0 or (frequency and frequency != row['qpc_frequency']):
            raise ValueError('Invalid or changing QPC frequency')
        frequency = row['qpc_frequency']
        if row['qpc_end'] < row['qpc_begin']:
            raise ValueError('Reversed callback interval')
        wall = (row['qpc_end'] - row['qpc_begin']) * 1000 / frequency
        if abs(row['wall_ms'] - wall) > 0.000001:
            raise ValueError('QPC and reported wall time disagree')
        if abs(sum(row[key] for key in STAGES) - row['wall_ms']) > 0.000003:
            raise ValueError('Stages do not partition callback wall time')
        has_pso_ms = 'get_pso_ms' in row
        has_pso_lookup = 'pso_lookup' in row
        if has_pso_ms != has_pso_lookup:
            raise ValueError('Incomplete nested PSO detail fields')
        present_phases = PSO_DETAIL_PHASES.intersection(row)
        if present_phases and present_phases != PSO_DETAIL_PHASES:
            raise ValueError('Incomplete nested PSO phase fields')
        if has_pso_ms:
            pso_ms = row['get_pso_ms']
            if not math.isfinite(pso_ms) or pso_ms < 0:
                raise ValueError('Invalid nested get_pso time')
            if pso_ms > row['pipeline_constants_ms'] + 0.000003:
                raise ValueError('Nested get_pso interval exceeds pipeline/constants stage')
            if row['pso_lookup'] not in (PSO_LOOKUP_UNCALLED,
                                         PSO_LOOKUP_HIT,
                                         PSO_LOOKUP_MISS):
                raise ValueError('Invalid PSO lookup outcome')
            if present_phases:
                phase_names = ('build_pso_ms', 'shader_disk_load_ms',
                               'd3d_compile_ms', 'create_graphics_pso_ms')
                phases = [row[name] for name in phase_names]
                if any(not math.isfinite(value) or value < 0 for value in phases):
                    raise ValueError('Invalid nested PSO phase time')
                build_ms, disk_ms, compile_ms, create_ms = phases
                if build_ms > pso_ms + 0.000003:
                    raise ValueError('Nested build_pso interval exceeds get_pso interval')
                if disk_ms + compile_ms + create_ms > build_ms + 0.000003:
                    raise ValueError('Nested PSO phases exceed build_pso interval')
        elif present_phases:
            raise ValueError('PSO phase fields require get_pso detail fields')
        present_resources = RESOURCE_DETAIL_PHASES.intersection(row)
        if present_resources and present_resources != RESOURCE_DETAIL_PHASES:
            raise ValueError('Incomplete nested resource detail fields')
        if present_resources:
            nested_ms = 0.0
            for kind in RESOURCE_KINDS:
                calls = row[f'{kind}_calls']
                total_ms = row[f'{kind}_total_ms']
                max_ms = row[f'{kind}_max_ms']
                if (not isinstance(calls, int) or calls < 0 or
                        not math.isfinite(total_ms) or not math.isfinite(max_ms) or
                        total_ms < 0 or max_ms < 0 or max_ms > total_ms + 0.000003):
                    raise ValueError(f'Invalid nested {kind} resource detail')
                if calls == 0 and (total_ms != 0 or max_ms != 0):
                    raise ValueError(f'Empty {kind} resource bucket has nonzero timing')
                for suffix in ('location', 'offset', 'format', 'width', 'height', 'slot'):
                    if row[f'{kind}_max_{suffix}'] < 0:
                        raise ValueError(f'Invalid nested {kind} maximum identity')
                nested_ms += total_ms
            residual = row['resource_residual_ms']
            if not math.isfinite(residual) or residual < 0:
                raise ValueError('Invalid resource residual')
            if abs(nested_ms + residual - row['resource_resolution_ms']) > 0.000004:
                raise ValueError('Nested resource detail and residual do not partition resource phase')
        present_texture_detail = TEXTURE_DETAIL_PHASES.intersection(row)
        present_texture_flags = TEXTURE_DETAIL_FLAGS.intersection(row)
        present_texture_skip = TEXTURE_SKIP_DETAIL_FIELDS.intersection(row)
        if present_texture_detail or present_texture_flags:
            if (present_texture_detail != TEXTURE_DETAIL_PHASES or
                    present_texture_flags != TEXTURE_DETAIL_FLAGS):
                raise ValueError('Incomplete nested texture-call detail fields')
            if present_resources != RESOURCE_DETAIL_PHASES:
                raise ValueError('Texture-call detail requires the complete outer resource schema')
            for key in TEXTURE_DETAIL_FLAGS:
                if not isinstance(row[key], int) or row[key] < 0:
                    raise ValueError('Invalid nested texture-call metadata')
            if (row['texture_outcome'] not in range(6) or
                    row['texture_decode_attempted'] not in (0, 1) or
                    row['texture_decode_succeeded'] not in (0, 1) or
                    row['texture_watch_skipped'] not in (0, 1) or
                    row['texture_cache_full'] not in (0, 1)):
                raise ValueError('Invalid nested texture-call outcome')
            phases = [row[name] for name in TEXTURE_DETAIL_PHASES]
            if any(not math.isfinite(value) or value < 0 for value in phases):
                raise ValueError('Invalid nested texture-call elapsed time')
            if row['texture_resource_create_ms'] > row['texture_slot_decode_ms'] + 0.000003:
                raise ValueError('Nested texture resource creation exceeds decode interval')
            if (row['texture_resource_create_calls'] == 0 and
                    row['texture_resource_create_ms'] != 0):
                raise ValueError('Empty nested texture resource creation has nonzero time')
            if (row['texture_decode_succeeded'] and
                    not row['texture_decode_attempted']):
                raise ValueError('Texture decode succeeded without an attempt')
            if (row['texture_outcome'] in (1, 2) and
                    row['texture_decode_attempted']) or (
                    row['texture_outcome'] in (3, 4, 5) and
                    not row['texture_decode_attempted']):
                raise ValueError('Texture outcome disagrees with decode attempt flag')
            if (row['texture_outcome'] in (1, 2) and
                    row['texture_decode_succeeded']):
                raise ValueError('Cache hit cannot report a new decode success')
            if row['texture_cache_full'] and row['texture_outcome'] != 5:
                raise ValueError('Full texture cache is only valid for a miss')
            if row['texture_calls'] == 0:
                if (row['texture_outcome'] != 0 or row['texture_decode_attempted'] or
                        row['texture_decode_succeeded'] or row['texture_watch_skipped'] or
                        row['texture_cache_full'] or row['texture_resource_create_calls'] or
                        any(row[name] != 0 for name in TEXTURE_DETAIL_PHASES)):
                    raise ValueError('No texture calls may carry no per-call detail')
            elif row['texture_outcome'] == 0:
                raise ValueError('Texture call has no classified outcome')
            slot_partition = sum(row[name] for name in (
                'texture_slot_lookup_ms', 'texture_slot_watch_ms',
                'texture_slot_hash_ms', 'texture_slot_decode_ms',
                'texture_slot_srv_ms', 'texture_slot_retire_ms',
                'texture_slot_log_ms', 'texture_slot_residual_ms'))
            if abs(slot_partition - row['texture_max_ms']) > 0.00001:
                raise ValueError('Nested texture-call detail does not partition maximum texture call')
        if present_texture_skip:
            if present_texture_skip != TEXTURE_SKIP_DETAIL_FIELDS:
                raise ValueError('Incomplete texture skip-reason detail fields')
            if (present_texture_detail != TEXTURE_DETAIL_PHASES or
                    present_texture_flags != TEXTURE_DETAIL_FLAGS):
                raise ValueError('Texture skip-reason detail requires complete texture-call detail')
            for key in TEXTURE_SKIP_DETAIL_FIELDS:
                if not isinstance(row[key], int) or row[key] < 0:
                    raise ValueError('Invalid texture skip-reason metadata')
            for key in TEXTURE_SKIP_BOOLEAN_FIELDS:
                if row[key] not in (0, 1):
                    raise ValueError('Invalid texture skip predicate flag')
            if row['texture_skip_rejection_mask'] & ~31:
                raise ValueError('Invalid texture skip rejection mask')
            if row['texture_hash_kind'] not in (0, 1, 2):
                raise ValueError('Invalid texture hash kind')
            if (row['texture_hash_cycle_successes'] + row['texture_hash_cycle_failures'] !=
                    row['texture_hash_cycle_attempts']):
                raise ValueError('Texture hash cycle query counts do not balance')
            if row['texture_hash_cycle_valid'] != int(
                    row['texture_hash_cycle_attempts'] > 0 and
                    row['texture_hash_cycle_failures'] == 0):
                raise ValueError('Texture hash cycle validity disagrees with query counts')
            if (row['texture_hash_cycle_successes'] == 0 and
                    row['texture_hash_cycle_delta_not_ms'] != 0):
                raise ValueError('Texture hash cycle delta without successful samples')
            if row['texture_skip_evaluated']:
                expected_mask = (
                    (1 if row['texture_skip_override_changed'] else 0) |
                    (2 if not row['texture_skip_watch_valid'] else 0) |
                    (4 if not row['texture_skip_entry_valid'] else 0) |
                    (8 if not row['texture_skip_stamp_match'] else 0) |
                    (16 if not row['texture_skip_hash_kind_match'] else 0))
                if row['texture_skip_rejection_mask'] != expected_mask:
                    raise ValueError('Texture skip rejection mask disagrees with predicate inputs')
                if row['texture_watch_skipped'] and expected_mask:
                    raise ValueError('Texture watch skip accepted despite rejected predicate')
            elif any(row[key] for key in (
                    'texture_skip_override_changed', 'texture_skip_watch_valid',
                    'texture_skip_entry_valid', 'texture_skip_stamp_match',
                    'texture_skip_hash_kind_match', 'texture_skip_validation',
                    'texture_skip_rejection_mask')):
                raise ValueError('Unevaluated texture skip has predicate inputs')
            if row['texture_calls'] == 0 and any(row[key] for key in TEXTURE_SKIP_DETAIL_FIELDS):
                raise ValueError('No texture calls may carry texture skip detail')
        if row['thread_cycles_valid'] not in (0, 1):
            raise ValueError('Invalid cycle validity flag')
        if row['thread_cycles_valid']:
            cycles = row['thread_cycles_end'] - row['thread_cycles_begin']
            if cycles < 0 or cycles != row['thread_cycles_delta_not_ms']:
                raise ValueError('Invalid raw cycle delta')
        elif row['thread_cycles_delta_not_ms'] != 0:
            raise ValueError('Invalid cycle query has nonzero reported delta')
    return frequency


def analyze(csv_path, windows_path, log_path, capacity=256):
    if not isinstance(capacity, int) or isinstance(capacity, bool) or capacity <= 0:
        raise ValueError('Ring capacity must be a positive integer')
    rows = []
    with Path(csv_path).open(newline='') as stream:
        for raw in csv.DictReader(stream):
            if None in raw or any(value is None for value in raw.values()):
                raise ValueError('Nonrectangular CSV')
            rows.append({key: float(value) if key in ELAPSED_FIELDS else
                         int(value, 16) if key in MASKS else int(value)
                         for key, value in raw.items()})
    frequency = validate_rows(rows)
    matches = re.findall(r'\[slow-draw\] saved=(\d+) overwritten=(\d+) callbacks=(\d+) threshold_us=(\d+)',
                         Path(log_path).read_text(encoding='utf-8', errors='replace'))
    if len(matches) != 1:
        raise ValueError('Expected exactly one recorder summary')
    saved, overwritten, callbacks, threshold = map(int, matches[0])
    if saved != len(rows) or saved > capacity or saved + overwritten > callbacks:
        raise ValueError('Recorder counts disagree')
    if overwritten and saved != capacity:
        raise ValueError('Overwritten ring is not full')
    if rows and max(row['callback_id'] for row in rows) > callbacks:
        raise ValueError('Callback ID exceeds final count')
    if any(row['threshold_us'] != threshold or row['wall_ms'] + 0.000001 < threshold / 1000
           for row in rows):
        raise ValueError('Threshold mismatch')
    windows = json.loads(Path(windows_path).read_text(encoding='utf-8-sig'))
    if not windows['complete'] or (frequency and windows['qpc_frequency'] != frequency):
        raise ValueError('Incomplete windows or clock mismatch')
    blocks = []
    for index, block in enumerate(windows['blocks']):
        lo, hi = block['start_qpc'], block['end_qpc']
        if hi <= lo:
            raise ValueError('Empty or reversed measurement window')
        selected = [row for row in rows if lo <= row['qpc_begin'] <= row['qpc_end'] <= hi]
        edges = [row for row in rows if row['qpc_begin'] < hi and row['qpc_end'] > lo
                 and not lo <= row['qpc_begin'] <= row['qpc_end'] <= hi]
        blocks.append(dict(index=index, flags=block['flags'], retained_full_callbacks=len(selected),
                           retained_edge_callbacks=len(edges),
                           longest_retained_callbacks=sorted(selected, key=lambda r:r['wall_ms'], reverse=True)[:12]))
    return dict(note='Filtered retained callbacks only, not complete-frame costs or an isolation audit. '
                     'Stages partition each callback; PSO details are nested. Resource subphases are nested in resource_resolution_ms; residual includes uninstrumented resolution work such as zdepth_get and descriptor handling. '
                     'Texture slot subphases describe only the slowest texture bind. CreateCommittedResource is nested within its decode time; decode residual includes conversion, content-cache work, and upload/copy/barrier recording. Direct slot diagnostics are timed as log work; helper diagnostics remain inside their enclosing phase. '
                     'Raw thread cycles are not CPU milliseconds; absence from this ring does not rule out a stall.',
                saved=saved, overwritten=overwritten, callbacks=callbacks, capacity=capacity,
                threshold_us=threshold,
                qpc_frequency=frequency, first_retained_begin_qpc=min((r['qpc_begin'] for r in rows), default=None),
                blocks=blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--windows', required=True, type=Path)
    parser.add_argument('--log', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--capacity', type=int, default=256,
                        help='recorder ring capacity (default: historical profile ring 256)')
    args = parser.parse_args()
    result = analyze(args.csv, args.windows, args.log, capacity=args.capacity)
    args.output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
