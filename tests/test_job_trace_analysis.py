"""Nested and parallel spans must not be misreported as additive frame costs."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from analyze_job_trace import summarize_window, union_ticks, join_async_jobs, summarize_async, summarize_wait_phases


class JobTraceAnalysis(unittest.TestCase):
    @staticmethod
    def reused_slot_events():
        rows = []
        for generation, offset in ((1, 0), (2, 100)):
            def row(kind, begin, end, **extra):
                return dict(kind=kind, begin_qpc=begin + offset, end_qpc=end + offset,
                            host_tid=1, guest_thread=1, slot=3, generation=generation,
                            slot_base=0x1000, caller=123, **extra)
            rows.extend([row(1, 0, 2, arg0=10, effective_route=2),
                         row(4, 3, 80), row(6, 55, 56, arg1=2),
                         row(6, 81, 82, arg1=0)])
            for kind, begin, end in ((7, 10, 10), (9, 20, 30), (10, 31, 60)):
                args = (generation, (1 << 32) | 41, 0, 0, 0x1000, 0, 0, 0)
                rows.append(row(kind, begin, end, **{f'arg{i}': v for i, v in enumerate(args)}))
        return rows

    def test_reused_slot_and_publication_before_abi_return(self):
        rows = self.reused_slot_events()
        result = summarize_wait_phases(rows, join_async_jobs(rows), 0, 200, 1, 2)
        self.assertEqual(result['skipped_waits'], {})
        group, = result['groups']
        self.assertEqual(group['waits'], 2)
        self.assertEqual(group['wait_ms_per_frame'], 77)
        self.assertEqual(group['phase_ms_per_frame'], dict(before_sdk_submit=7,
                         worker_startup=10, worker_setup=10, abi_execution=29,
                         after_abi=20, unattributed=1))

    def test_ambiguous_or_incomplete_jobs_are_not_attributed(self):
        rows = self.reused_slot_events()
        duplicate = next(r.copy() for r in rows if r['kind'] == 7)
        duplicate['arg0'] = 999
        rows.append(duplicate)
        result = summarize_wait_phases(rows, join_async_jobs(rows), 0, 200, 1, 2)
        self.assertEqual(result['skipped_waits'], {'missing_or_ambiguous_sdk_job': 1})
        self.assertEqual(result['groups'][0]['waits'], 1)
        rows = [r for r in self.reused_slot_events() if not (r['kind'] == 10 and r['generation'] == 2)]
        result = summarize_wait_phases(rows, join_async_jobs(rows), 0, 200, 1, 2)
        self.assertEqual(result['skipped_waits'], {'missing_worker_phases': 1})

    def test_wrong_completion_address_is_not_attributed(self):
        rows = self.reused_slot_events()
        next(r for r in rows if r['kind'] == 6)['slot_base'] = 0x2000
        result = summarize_wait_phases(rows, join_async_jobs(rows), 0, 200, 1, 2)
        self.assertEqual(result['skipped_waits'], {'inconsistent_observed_lifetime': 1})

    def test_dispatch_provenance_partitions_same_job_presubmit_wait(self):
        rows = self.reused_slot_events()
        for submit in list(rows):
            if submit['kind'] != 7:
                continue
            discovery = submit.copy()
            discovery.update(kind=16, begin_qpc=submit['begin_qpc']-5,
                             arg4=3, arg5=submit['begin_qpc']-5, arg6=0, arg7=0)
            # Put discovery first to exercise immutable R3 validation later.
            rows.insert(0, discovery)
        jobs = join_async_jobs(rows)
        group, = summarize_wait_phases(rows, jobs, 0, 200, 1, 2)['groups']
        self.assertEqual(group['dispatch_provenance'], {'poll': {
            'waits': 2, 'before_claim_wait_ms_per_frame': 2,
            'claim_to_submit_wait_ms_per_frame': 5,
            'observed_ready_waits': 0, 'before_ready_wait_ms_per_frame': 0,
            'ready_to_claim_wait_ms_per_frame': 0,
            'unknown_ready_before_claim_wait_ms_per_frame': 2}})
        self.assertEqual(group['phase_ms_per_frame']['before_sdk_submit'], 7)
        next(r for r in rows if r['kind'] == 9)['arg7'] = 1
        with self.assertRaisesRegex(ValueError, 'snapshot identity'):
            join_async_jobs(rows)

    def test_dispatch_provenance_rejects_mismatched_endpoint_and_cause(self):
        rows = self.reused_slot_events()
        submit = next(r for r in rows if r['kind'] == 7)
        discovery = dict(submit, kind=16, begin_qpc=5, arg4=3, arg5=5, arg6=0, arg7=0)
        rows.append(discovery)
        for field, value in (('end_qpc', 11), ('host_tid', 7), ('arg4', 99), ('arg5', 4), ('arg6', 6)):
            previous = discovery[field]
            discovery[field] = value
            with self.assertRaises(ValueError):
                join_async_jobs(rows)
            discovery[field] = previous

    def test_observed_readiness_splits_claim_wait_without_changing_phase_total(self):
        rows = self.reused_slot_events()
        for submit in list(rows):
            if submit['kind'] != 7:
                continue
            rows.append(dict(submit, kind=16, begin_qpc=submit['begin_qpc']-3,
                             arg4=4, arg5=submit['begin_qpc']-3,
                             arg6=submit['begin_qpc']-5, arg7=0))
        group, = summarize_wait_phases(rows, join_async_jobs(rows), 0, 200, 1, 2)['groups']
        bucket = group['dispatch_provenance']['ready_wake']
        self.assertEqual(bucket['observed_ready_waits'], 2)
        self.assertEqual(bucket['before_ready_wait_ms_per_frame'], 2)
        self.assertEqual(bucket['ready_to_claim_wait_ms_per_frame'], 2)
        self.assertEqual(bucket['before_claim_wait_ms_per_frame'], 4)
        self.assertEqual(bucket['claim_to_submit_wait_ms_per_frame'], 3)
        self.assertEqual(group['phase_ms_per_frame']['before_sdk_submit'], 7)

    def test_async_thread_can_finish_before_create_returns(self):
        def row(kind, lo, hi, tid):
            return dict(kind=kind, begin_qpc=lo, end_qpc=hi, host_tid=tid,
                        **{f'arg{i}': value for i, value in enumerate((1, 40, 123, 456, 0, 0, 0, 0))})
        rows = [row(8, 10, 100, 1), row(9, 20, 30, 2), row(10, 30, 60, 2),
                row(11, 60, 65, 2), row(12, 66, 70, 2)]
        jobs = join_async_jobs(rows)
        result = summarize_async(jobs, 0, 110, 1, 10)[0]
        self.assertEqual(result['image'], 40)
        self.assertEqual(result['startup_gap_mean_ms'], 10)
        self.assertEqual(result['phases']['spu_execute']['clipped_elapsed_ms_per_frame'], 3)
        rows[-1]['arg2'] = 999
        with self.assertRaises(ValueError):
            join_async_jobs(rows)
        rows[-1]['arg2'] = 123
        rows[-1]['begin_qpc'] = 64
        with self.assertRaises(ValueError):
            join_async_jobs(rows)

    def test_reuse_worker_dispatch_may_overlap_setup_but_is_mutually_exclusive(self):
        def row(kind, lo, hi, tid):
            return dict(kind=kind, begin_qpc=lo, end_qpc=hi, host_tid=tid,
                        **{f'arg{i}': value for i, value in enumerate((1, 30, 123, 456, 0, 0, 0, 0))})
        rows = [row(15, 10, 40, 1), row(9, 20, 30, 2), row(10, 30, 60, 2)]
        jobs = join_async_jobs(rows)
        self.assertIn(15, jobs[1])
        self.assertEqual(summarize_async(jobs, 0, 100, 1, 1)[0]['startup_gap_mean_ms'], 10)
        rows.append(row(8, 10, 15, 1))
        with self.assertRaises(ValueError):
            join_async_jobs(rows)
        failed_reuse = [row(15, 10, 40, 1), row(13, 12, 12, 1)]
        with self.assertRaises(ValueError):
            join_async_jobs(failed_reuse)

    def test_union_nested_disjoint_touching(self):
        self.assertEqual(union_ticks([(0, 20), (5, 10), (20, 25), (30, 40)]), 35)
        self.assertEqual(union_ticks([]), 0)

    def test_clipping_nesting_and_parallelism(self):
        def row(kind, lo, hi, tid=1):
            return dict(kind=kind, begin_qpc=lo, end_qpc=hi, host_tid=tid,
                        slot=0, generation=1, dispatch_code=0x682900, dispatch_toc=0xE94EE0)
        events = [row(1, 0, 100), row(2, 10, 90), row(3, 20, 70), row(3, 30, 50), row(3, 40, 60, 2)]
        summary = dict(qpc_frequency=1000, max_discarded_end_qpc=0, incomplete_scopes=0)
        result = summarize_window(events, summary, dict(start_qpc=25, end_qpc=75), 2)
        self.assertTrue(result['completed_span_coverage'])
        self.assertEqual(result['groups']['submit']['clipped_thread_union_ms'], 50)
        self.assertEqual(result['groups']['transform_kernel']['clipped_thread_union_ms_per_frame'], 32.5)
        self.assertEqual(result['dispatch_targets'][0]['code'], '0x00682900')
        summary['max_discarded_end_qpc'] = 26
        self.assertFalse(summarize_window(events, summary, dict(start_qpc=25, end_qpc=75), 2)['completed_span_coverage'])
        summary.update(max_discarded_end_qpc=0, incomplete_scopes=1)
        self.assertFalse(summarize_window(events, summary, dict(start_qpc=25, end_qpc=75), 2)['completed_span_coverage'])


if __name__ == '__main__':
    unittest.main()
