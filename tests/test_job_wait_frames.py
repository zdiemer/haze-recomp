"""Per-frame job wait attribution conserves wall intervals and handles clipping."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from analyze_job_trace import join_async_jobs
from analyze_job_wait_frames import (attribute_wait_intervals, map_to_frames,
                                    validate_retained_ring, validate_trace_window_coverage)


def reused_slot_events():
    rows = []
    for generation, offset in ((1, 0), (2, 100)):
        def row(kind, begin, end, **extra):
            return dict(kind=kind, begin_qpc=begin + offset, end_qpc=end + offset,
                        host_tid=1, guest_thread=1, slot=3, generation=generation,
                        slot_base=0x1000, caller=123, **extra)
        rows.extend([row(1, 0, 2, arg0=10, effective_route=2),
                     row(4, 3, 80), row(6, 55, 56, arg1=2)])
        for kind, begin, end in ((7, 10, 10), (9, 20, 30), (10, 31, 60)):
            args = (generation, (1 << 32) | 41, 0, 0, 0x1000, 0, 0, 0)
            row_args = {f'arg{i}': value for i, value in enumerate(args)}
            rows.append(row(kind, begin, end, **row_args))
    return rows


def pump_frame(frame, begin, end):
    dt = end - begin
    return dict(frame=frame, begin_qpc=begin, end_qpc=end, qpc_frequency=1000,
                dt_ms=float(dt), wait_ms=float(dt) * .4, fifo_ms=float(dt) * .4,
                present_ms=float(dt) * .1, other_ms=float(dt) * .1,
                thread_cpu_ms=3.0, cpu_valid=1)


class JobWaitFrames(unittest.TestCase):
    def test_window_coverage_requires_gap_free_nondroppped_ring_and_matching_clock(self):
        events = [dict(ticket=10, parent_missing_in_dump=0),
                  dict(ticket=11, parent_missing_in_dump=0)]
        summary = dict(tickets_before_retained_window=10, missing_tickets=0,
                       dropped_busy=0, dropped_stale=0, dropped_frozen=0,
                       incomplete_scopes=0, max_discarded_end_qpc=50,
                       qpc_frequency=1000)
        retained = validate_retained_ring(events, summary)
        self.assertEqual((retained['first_ticket'], retained['last_ticket']), (10, 11))
        self.assertTrue(validate_trace_window_coverage(events, summary, 60, 100, 1000)
                        ['discarded_end_qpc_precedes_window'])
        for key, value in (('missing_tickets', 1), ('dropped_busy', 1),
                           ('incomplete_scopes', 1)):
            invalid = summary.copy()
            invalid[key] = value
            with self.assertRaises(ValueError):
                validate_retained_ring(events, invalid)
        with self.assertRaises(ValueError):
            validate_retained_ring([events[0], dict(ticket=12, parent_missing_in_dump=0)], summary)
        with self.assertRaises(ValueError):
            validate_trace_window_coverage(events, summary, 60, 100, 999)
        with self.assertRaises(ValueError):
            validate_trace_window_coverage(events, summary, 40, 100, 1000)

    def test_per_frame_phase_partition_and_reused_slot(self):
        events = reused_slot_events()
        jobs = join_async_jobs(events)
        intervals, skipped = attribute_wait_intervals(events, jobs, 0, 200)
        self.assertEqual(skipped, {})
        self.assertEqual(len(intervals), 2)
        frames, output, coverage = map_to_frames(intervals,
            [pump_frame(50, 0, 100), pump_frame(51, 100, 200)], 0, 200)
        self.assertEqual(len(frames), 2)
        self.assertEqual(coverage['attributed_phase_ticks_outside_full_pump_frames'], 0)
        first, second = output
        self.assertEqual(first['observed_wait_union_ms'], 77)
        self.assertEqual(first['pump_interval_not_covered_by_observed_wait_ms'], 23)
        self.assertEqual(first['exclusive_phase_wall_ms'], dict(
            before_sdk_submit=7, worker_startup=10, worker_setup=10,
            abi_execution=29, after_abi=20, unattributed=1))
        self.assertEqual(second['exclusive_phase_wall_ms'], dict(
            before_sdk_submit=7, worker_startup=10, worker_setup=10,
            abi_execution=29, after_abi=20, unattributed=1))
        self.assertEqual(first['abi_union_wall_ms_by_image'], {'41': 29})

    def test_partial_window_wait_is_clipped_and_reported(self):
        events = reused_slot_events()
        intervals, skipped = attribute_wait_intervals(events, join_async_jobs(events), 40, 70)
        self.assertEqual(skipped, {})
        self.assertEqual(len(intervals), 1)
        self.assertEqual((intervals[0]['wait_begin_qpc'], intervals[0]['wait_end_qpc']), (40, 70))
        frames, output, coverage = map_to_frames(intervals, [pump_frame(1, 40, 70)], 40, 70)
        self.assertEqual(output[0]['observed_wait_union_ms'], 30)
        self.assertEqual(coverage['attributed_phase_ticks_outside_full_pump_frames'], 0)

    def test_concurrent_host_waits_report_phase_overlap(self):
        rows = reused_slot_events()
        duplicate = next(row.copy() for row in rows if row['kind'] == 4)
        duplicate['host_tid'] = 2
        rows.append(duplicate)
        intervals, skipped = attribute_wait_intervals(rows, join_async_jobs(rows), 0, 100)
        self.assertEqual(skipped, {})
        frames, output, _ = map_to_frames(intervals, [pump_frame(1, 0, 100)], 0, 100)
        self.assertEqual(output[0]['observed_wait_union_ms'], 77)
        self.assertEqual(output[0]['overlap_across_phases_ms'], 0)
        self.assertEqual(sum(output[0]['exclusive_phase_wall_ms'].values()), 77)
        self.assertEqual(output[0]['phase_union_wall_ms']['abi_execution'], 29)


if __name__ == '__main__':
    unittest.main()
