import copy
import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from diagnostics.analyze_slow_draws import (RESOURCE_KINDS, STAGES,
                                TEXTURE_SKIP_DETAIL_FIELDS, analyze,
                                validate_rows)


def row():
    return dict(callback_id=1, qpc_frequency=10_000_000, qpc_begin=10_000,
                qpc_end=40_000, wall_ms=3.0, thread_cycles_valid=1,
                thread_cycles_begin=100, thread_cycles_end=190,
                thread_cycles_delta_not_ms=90,
                **dict(zip(STAGES, (1.0, 0.5, 0.5, 1.0))))


def write_capture(root, events, saved, overwritten, callbacks, threshold=1000):
    source, windows, log = (root / name for name in ('draw.csv', 'windows.json', 'stderr.log'))
    rows = []
    for callback_id in events:
        event = row()
        event.update(callback_id=callback_id,
                     qpc_begin=callback_id * 30_000,
                     qpc_end=callback_id * 30_000 + 30_000,
                     threshold_us=threshold)
        rows.append(event)
    with source.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0])
        writer.writeheader()
        writer.writerows(rows)
    windows.write_text(json.dumps(dict(
        complete=True, qpc_frequency=10_000_000,
        blocks=[dict(start_qpc=0, end_qpc=(max(events) + 1) * 30_000 + 1, flags=1)])))
    log.write_text(f'[slow-draw] saved={saved} overwritten={overwritten} '
                   f'callbacks={callbacks} threshold_us={threshold}')
    return source, windows, log


class SlowDrawAnalysisTests(unittest.TestCase):
    def test_partition_and_raw_cycle_units(self):
        self.assertEqual(validate_rows([row()]), 10_000_000)

    def test_nonconserved_stage_rejected(self):
        bad = row(); bad[STAGES[0]] += 1
        with self.assertRaisesRegex(ValueError, 'partition'):
            validate_rows([bad])

    def test_nested_get_pso_detail_and_lookup_outcomes(self):
        for outcome in (0, 1, 2):
            detail = row()
            detail.update(get_pso_ms=0.4, pso_lookup=outcome,
                          build_pso_ms=0.3, shader_disk_load_ms=0.1,
                          d3d_compile_ms=0.1, create_graphics_pso_ms=0.1)
            validate_rows([detail])
        too_long = row()
        too_long.update(get_pso_ms=0.500004, pso_lookup=1)
        with self.assertRaisesRegex(ValueError, 'exceeds pipeline'):
            validate_rows([too_long])
        nonfinite = row()
        nonfinite.update(get_pso_ms=float('nan'), pso_lookup=2)
        with self.assertRaisesRegex(ValueError, 'nested get_pso'):
            validate_rows([nonfinite])
        for bad in ({'get_pso_ms': 0.1}, {'pso_lookup': 1},
                    {'get_pso_ms': 0.1, 'pso_lookup': 3}):
            detail = row(); detail.update(bad)
            with self.assertRaises(ValueError):
                validate_rows([detail])
        phase_exceeds_build = row()
        phase_exceeds_build.update(get_pso_ms=0.4, pso_lookup=2,
                                   build_pso_ms=0.2, shader_disk_load_ms=0.1,
                                   d3d_compile_ms=0.1, create_graphics_pso_ms=0.1)
        with self.assertRaisesRegex(ValueError, 'exceed build_pso'):
            validate_rows([phase_exceeds_build])
        partial_phases = row()
        partial_phases.update(get_pso_ms=0.4, pso_lookup=0, build_pso_ms=0.2)
        with self.assertRaisesRegex(ValueError, 'Incomplete nested PSO phase'):
            validate_rows([partial_phases])

    def test_nested_resource_details_partition_resource_phase(self):
        detail = row()
        detail['resource_residual_ms'] = 0.3
        for kind in RESOURCE_KINDS:
            detail.update({
                f'{kind}_calls': 1,
                f'{kind}_total_ms': 0.05,
                f'{kind}_max_ms': 0.05,
                f'{kind}_max_location': 1,
                f'{kind}_max_offset': 2,
                f'{kind}_max_format': 3,
                f'{kind}_max_width': 4,
                f'{kind}_max_height': 5,
                f'{kind}_max_slot': 6,
            })
        detail.update(texture_outcome=4, texture_decode_attempted=1,
                      texture_decode_succeeded=1,
                      texture_watch_skipped=0, texture_cache_full=0,
                      texture_slot_lookup_ms=0.005,
                      texture_slot_watch_ms=0.005,
                      texture_slot_hash_ms=0.005,
                      texture_slot_decode_ms=0.010,
                      texture_slot_srv_ms=0.005,
                      texture_slot_retire_ms=0.005,
                      texture_slot_log_ms=0.005,
                      texture_slot_residual_ms=0.010,
                      texture_resource_create_ms=0.004,
                      texture_resource_create_calls=1)
        validate_rows([detail])
        incomplete = copy.deepcopy(detail)
        del incomplete['texture_max_ms']
        with self.assertRaisesRegex(ValueError, 'Incomplete nested resource'):
            validate_rows([incomplete])
        too_long = copy.deepcopy(detail)
        too_long['resource_residual_ms'] += 0.1
        with self.assertRaisesRegex(ValueError, 'do not partition'):
            validate_rows([too_long])
        impossible_max = copy.deepcopy(detail)
        impossible_max['surface_max_ms'] = 0.06
        with self.assertRaisesRegex(ValueError, 'Invalid nested surface'):
            validate_rows([impossible_max])
        for key, value in (('texture_total_ms', -0.1),
                           ('texture_max_ms', float('inf')),
                           ('depth_max_offset', -1)):
            invalid = copy.deepcopy(detail)
            invalid[key] = value
            with self.assertRaisesRegex(ValueError, 'Invalid nested'):
                validate_rows([invalid])
        empty_with_time = copy.deepcopy(detail)
        empty_with_time.update(vertex_texture_calls=0,
                               vertex_texture_total_ms=0.01,
                               vertex_texture_max_ms=0)
        empty_with_time['resource_residual_ms'] += 0.05
        with self.assertRaisesRegex(ValueError, 'Empty vertex_texture'):
            validate_rows([empty_with_time])

    def test_nested_texture_detail_partition_and_validation(self):
        detail = row()
        detail['resource_residual_ms'] = 0.0
        for kind in RESOURCE_KINDS:
            detail.update({
                f'{kind}_calls': 0, f'{kind}_total_ms': 0.0,
                f'{kind}_max_ms': 0.0, f'{kind}_max_location': 0,
                f'{kind}_max_offset': 0, f'{kind}_max_format': 0,
                f'{kind}_max_width': 0, f'{kind}_max_height': 0,
                f'{kind}_max_slot': 0,
            })
        detail.update(resource_residual_ms=0.0,
                      texture_calls=1, texture_total_ms=0.5,
                      texture_max_ms=0.5,
                      texture_outcome=5, texture_decode_attempted=1,
                      texture_decode_succeeded=1,
                      texture_watch_skipped=0, texture_cache_full=1,
                      texture_slot_lookup_ms=0.03, texture_slot_watch_ms=0.02,
                      texture_slot_hash_ms=0.05, texture_slot_decode_ms=0.30,
                      texture_slot_srv_ms=0.04, texture_slot_retire_ms=0.01,
                      texture_slot_log_ms=0.01, texture_slot_residual_ms=0.04,
                      texture_resource_create_ms=0.15,
                      texture_resource_create_calls=1)
        validate_rows([detail])
        for key, value, message in (
            ('texture_slot_decode_ms', float('nan'), 'elapsed'),
            ('texture_slot_residual_ms', -0.1, 'elapsed'),
            ('texture_resource_create_ms', 0.31, 'exceeds decode'),
            ('texture_resource_create_calls', -1, 'metadata'),
            ('texture_outcome', 6, 'outcome'),
            ('texture_outcome', 1, 'outcome disagrees'),
            ('texture_slot_residual_ms', 0.05, 'partition'),
        ):
            invalid = copy.deepcopy(detail)
            invalid[key] = value
            with self.assertRaisesRegex(ValueError, message):
                validate_rows([invalid])
        incomplete = copy.deepcopy(detail)
        del incomplete['texture_slot_hash_ms']
        with self.assertRaisesRegex(ValueError, 'Incomplete nested texture-call'):
            validate_rows([incomplete])
        no_calls = copy.deepcopy(detail)
        no_calls.update(texture_calls=0, texture_total_ms=0.0,
                        texture_max_ms=0.0, resource_residual_ms=0.5,
                        texture_outcome=0, texture_decode_attempted=0,
                        texture_decode_succeeded=0, texture_watch_skipped=0,
                        texture_cache_full=0)
        for phase in ('lookup', 'watch', 'hash', 'decode', 'srv', 'retire',
                      'log', 'residual'):
            no_calls[f'texture_slot_{phase}_ms'] = 0.0
        no_calls['texture_resource_create_ms'] = 0.0
        no_calls['texture_resource_create_calls'] = 0
        validate_rows([no_calls])
        invalid_empty = copy.deepcopy(no_calls)
        invalid_empty['texture_outcome'] = 1
        with self.assertRaisesRegex(ValueError, 'No texture calls'):
            validate_rows([invalid_empty])
        partial_outer = row()
        partial_outer.update(texture_outcome=1, texture_decode_attempted=0,
                             texture_decode_succeeded=0, texture_watch_skipped=0,
                             texture_cache_full=0,
                             **{key: 0.0 for key in (
                                 'texture_slot_lookup_ms', 'texture_slot_watch_ms',
                                 'texture_slot_hash_ms', 'texture_slot_decode_ms',
                                 'texture_slot_srv_ms', 'texture_slot_retire_ms',
                                 'texture_slot_log_ms', 'texture_slot_residual_ms',
                                 'texture_resource_create_ms')},
                             texture_resource_create_calls=0)
        with self.assertRaisesRegex(ValueError, 'complete outer resource schema'):
            validate_rows([partial_outer])

    def test_texture_skip_reason_and_hash_cycle_evidence(self):
        detail = row()
        for kind in RESOURCE_KINDS:
            detail.update({
                f'{kind}_calls': 0, f'{kind}_total_ms': 0.0,
                f'{kind}_max_ms': 0.0, f'{kind}_max_location': 0,
                f'{kind}_max_offset': 0, f'{kind}_max_format': 0,
                f'{kind}_max_width': 0, f'{kind}_max_height': 0,
                f'{kind}_max_slot': 0,
            })
        detail.update(resource_residual_ms=0.0,
                      texture_calls=1, texture_total_ms=0.5,
                      texture_max_ms=0.5,
                      texture_outcome=1, texture_decode_attempted=0,
                      texture_decode_succeeded=0,
                      texture_watch_skipped=0, texture_cache_full=0,
                      texture_slot_lookup_ms=0.01,
                      texture_slot_watch_ms=0.02,
                      texture_slot_hash_ms=0.40,
                      texture_slot_decode_ms=0.0,
                      texture_slot_srv_ms=0.01,
                      texture_slot_retire_ms=0.0,
                      texture_slot_log_ms=0.0,
                      texture_slot_residual_ms=0.06,
                      texture_resource_create_ms=0.0,
                      texture_resource_create_calls=0)
        detail.update(texture_skip_evaluated=1,
                      texture_skip_override_changed=0,
                      texture_skip_watch_valid=1,
                      texture_skip_entry_valid=0,
                      texture_skip_stamp_match=1,
                      texture_skip_hash_kind_match=1,
                      texture_skip_validation=0,
                      texture_skip_rejection_mask=4,
                      texture_source_span=4 * 1024 * 1024,
                      texture_mipmaps=1, texture_hash_kind=0,
                      texture_hash_cycle_attempts=1,
                      texture_hash_cycle_successes=1,
                      texture_hash_cycle_failures=0,
                      texture_hash_cycle_valid=1,
                      texture_hash_cycle_delta_not_ms=123456)
        validate_rows([detail])
        invalid = copy.deepcopy(detail)
        invalid['texture_skip_rejection_mask'] = 0
        with self.assertRaisesRegex(ValueError, 'disagrees'):
            validate_rows([invalid])
        invalid = copy.deepcopy(detail)
        invalid['texture_hash_cycle_successes'] = 0
        with self.assertRaisesRegex(ValueError, 'do not balance'):
            validate_rows([invalid])
        invalid = copy.deepcopy(detail)
        invalid['texture_hash_cycle_valid'] = 0
        with self.assertRaisesRegex(ValueError, 'validity disagrees'):
            validate_rows([invalid])
        invalid = copy.deepcopy(detail)
        invalid.update(texture_hash_cycle_successes=0,
                       texture_hash_cycle_failures=1,
                       texture_hash_cycle_valid=0,
                       texture_hash_cycle_delta_not_ms=1)
        with self.assertRaisesRegex(ValueError, 'delta without successful'):
            validate_rows([invalid])
        invalid = copy.deepcopy(detail)
        invalid['texture_hash_kind'] = 3
        with self.assertRaisesRegex(ValueError, 'hash kind'):
            validate_rows([invalid])
        incomplete = copy.deepcopy(detail)
        del incomplete['texture_skip_watch_valid']
        with self.assertRaisesRegex(ValueError, 'Incomplete texture skip-reason'):
            validate_rows([incomplete])
        accepted_but_rejected = copy.deepcopy(detail)
        accepted_but_rejected['texture_watch_skipped'] = 1
        with self.assertRaisesRegex(ValueError, 'accepted despite rejected'):
            validate_rows([accepted_but_rejected])
        partial = row()
        partial.update({key: detail[key] for key in TEXTURE_SKIP_DETAIL_FIELDS})
        with self.assertRaisesRegex(ValueError, 'requires complete texture-call detail'):
            validate_rows([partial])

    def test_nan_and_qpc_mismatch_rejected(self):
        for key, value in [('wall_ms', float('nan')), ('qpc_end', 50_000)]:
            bad = row(); bad[key] = value
            with self.assertRaises(ValueError): validate_rows([bad])

    def test_cycle_failures_cannot_report_cpu_work(self):
        for change in ({'thread_cycles_end': 99}, {'thread_cycles_delta_not_ms': 1},
                       {'thread_cycles_valid': 0}):
            bad = row(); bad.update(change)
            with self.assertRaises(ValueError): validate_rows([bad])
        invalid = row(); invalid.update(thread_cycles_valid=0, thread_cycles_delta_not_ms=0)
        validate_rows([invalid])

    def test_order_and_clock_changes_rejected(self):
        a, b = row(), copy.deepcopy(row())
        with self.assertRaisesRegex(ValueError, 'IDs'): validate_rows([a, b])
        b.update(callback_id=2, qpc_frequency=1_000_000)
        with self.assertRaisesRegex(ValueError, 'frequency'): validate_rows([a, b])

    def test_report_filters_windows_and_rejects_bad_capture_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, windows, log = (root / name for name in ('draw.csv', 'windows.json', 'stderr.log'))
            event = row()
            event.update(threshold_us=1000)
            with source.open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=event)
                writer.writeheader()
                writer.writerow(event)
            capture = dict(complete=True, qpc_frequency=10_000_000,
                           blocks=[dict(start_qpc=1, end_qpc=50_000, flags=1)])
            windows.write_text(json.dumps(capture))
            log.write_text('[slow-draw] saved=1 overwritten=0 callbacks=2 threshold_us=1000')
            report = analyze(source, windows, log)
            self.assertEqual(report['blocks'][0]['retained_full_callbacks'], 1)
            capture['blocks'][0]['start_qpc'] = 20_000
            windows.write_text(json.dumps(capture))
            report = analyze(source, windows, log)
            self.assertEqual(report['blocks'][0]['retained_full_callbacks'], 0)
            self.assertEqual(report['blocks'][0]['retained_edge_callbacks'], 1)
            capture['blocks'][0]['end_qpc'] = 20_000
            windows.write_text(json.dumps(capture))
            with self.assertRaisesRegex(ValueError, 'measurement window'):
                analyze(source, windows, log)
            log.write_text('[slow-draw] saved=2 overwritten=0 callbacks=2 threshold_us=1000')
            with self.assertRaisesRegex(ValueError, 'counts'):
                analyze(source, windows, log)

    def test_analyze_reads_full_pso_schema_and_legacy_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, windows, log = (root / name for name in ('draw.csv', 'windows.json', 'stderr.log'))
            windows.write_text(json.dumps(dict(
                complete=True, qpc_frequency=10_000_000,
                blocks=[dict(start_qpc=0, end_qpc=50_000, flags=1)])))
            log.write_text('[slow-draw] saved=1 overwritten=0 callbacks=1 threshold_us=1000')

            full = row()
            full.update(threshold_us=1000, get_pso_ms=0.4, pso_lookup=2,
                        build_pso_ms=0.3, shader_disk_load_ms=0.1,
                        d3d_compile_ms=0.1, create_graphics_pso_ms=0.1)
            full.update(texture_outcome=5, texture_decode_attempted=1,
                        texture_decode_succeeded=1,
                        texture_watch_skipped=0, texture_cache_full=0,
                        texture_slot_lookup_ms=0.005,
                        texture_slot_watch_ms=0.005,
                        texture_slot_hash_ms=0.005,
                        texture_slot_decode_ms=0.020,
                        texture_slot_srv_ms=0.005,
                        texture_slot_retire_ms=0.002,
                        texture_slot_log_ms=0.003,
                        texture_slot_residual_ms=0.005,
                        texture_resource_create_ms=0.010,
                        texture_resource_create_calls=1)
            full.update(texture_skip_evaluated=1,
                        texture_skip_override_changed=0,
                        texture_skip_watch_valid=1,
                        texture_skip_entry_valid=0,
                        texture_skip_stamp_match=1,
                        texture_skip_hash_kind_match=1,
                        texture_skip_validation=0,
                        texture_skip_rejection_mask=4,
                        texture_source_span=4096, texture_mipmaps=1,
                        texture_hash_kind=0,
                        texture_hash_cycle_attempts=1,
                        texture_hash_cycle_successes=1,
                        texture_hash_cycle_failures=0,
                        texture_hash_cycle_valid=1,
                        texture_hash_cycle_delta_not_ms=1000)
            full['resource_residual_ms'] = 0.3
            for kind in RESOURCE_KINDS:
                full.update({
                    f'{kind}_calls': 1,
                    f'{kind}_total_ms': 0.05,
                    f'{kind}_max_ms': 0.05,
                    f'{kind}_max_location': 1,
                    f'{kind}_max_offset': 2,
                    f'{kind}_max_format': 3,
                    f'{kind}_max_width': 4,
                    f'{kind}_max_height': 5,
                    f'{kind}_max_slot': 6,
                })
            with source.open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=full)
                writer.writeheader()
                writer.writerow(full)
            report = analyze(source, windows, log)
            self.assertEqual(report['blocks'][0]['retained_full_callbacks'], 1)
            with source.open(newline='') as stream:
                parsed = next(csv.DictReader(stream))
            self.assertEqual(float(parsed['shader_disk_load_ms']), 0.1)

            legacy = row()
            legacy.update(threshold_us=1000)
            with source.open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=legacy)
                writer.writeheader()
                writer.writerow(legacy)
            report = analyze(source, windows, log)
            self.assertEqual(report['blocks'][0]['retained_full_callbacks'], 1)

    def test_legacy_256_capacity_default_remains_compatible(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, windows, log = write_capture(root, range(3, 259), 256, 2, 258)
            report = analyze(source, windows, log)
            self.assertEqual(report['capacity'], 256)
            self.assertEqual((report['saved'], report['overwritten']), (256, 2))

    def test_capacity_1024_no_overwrite_and_full_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, windows, log = write_capture(root, range(1, 4), 3, 0, 3)
            report = analyze(source, windows, log, capacity=1024)
            self.assertEqual((report['capacity'], report['saved'], report['overwritten']),
                             (1024, 3, 0))

            source, windows, log = write_capture(root, range(5, 1029), 1024, 4, 1028)
            report = analyze(source, windows, log, capacity=1024)
            self.assertEqual((report['capacity'], report['saved'], report['overwritten']),
                             (1024, 1024, 4))

    def test_ring_counts_must_match_selected_capacity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, windows, log = write_capture(root, range(1, 1025), 1024, 4, 1027)
            with self.assertRaisesRegex(ValueError, 'counts'):
                analyze(source, windows, log, capacity=1024)

            source, windows, log = write_capture(root, range(5, 1028), 1023, 4, 1028)
            with self.assertRaisesRegex(ValueError, 'not full'):
                analyze(source, windows, log, capacity=1024)

            source, windows, log = write_capture(root, range(1, 3), 2, 0, 1)
            with self.assertRaisesRegex(ValueError, 'counts'):
                analyze(source, windows, log, capacity=1024)

    def test_capacity_must_be_positive_integer(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, windows, log = write_capture(root, range(1, 2), 1, 0, 1)
            for capacity in (0, -1, 1.5, True):
                with self.assertRaisesRegex(ValueError, 'positive integer'):
                    analyze(source, windows, log, capacity=capacity)


if __name__ == '__main__':
    unittest.main()
