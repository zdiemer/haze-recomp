import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from diagnostics import analyze_slow_drains as analyzer


FREQ = 1000
FIFO_HEADER = [
    'sequence', 'host_tid', 'renderer_frame', 'context_valid', 'qpc_begin',
    'qpc_lock_acquired', 'qpc_body_end', 'total_ms', 'lock_wait_ms', 'body_ms',
    'thread_cycles_valid', 'thread_cycles_delta', 'get_before', 'put_before',
    'get_after', 'put_after',
]
FIFO_PHASE_HEADER = FIFO_HEADER + [
    'phase_mask', 'prewalk_setup_ticks', 'walker_ticks', 'postwalk_ticks',
    'recycle_ticks', 'post_recycle_ticks', 'drained_ea_publish_ticks',
    'get_write_ticks',
    'reference_lock_wait_ticks', 'reference_publish_ticks',
    'phase_unaccounted_ticks', 'stopped_reason',
]
FLUSH_HEADER = [
    'sequence', 'frame', 'host_tid', 'reason', 'qpc_begin', 'qpc_end',
    'total_ticks', 'thread_cycles_valid', 'thread_cycles_delta', 'fence_value',
    'completed_value', 'retired_count', 'waited', 'wait_result', 'wait_error',
    'stage_mask', 'close_hr', 'signal_hr', 'set_event_hr', 'alloc_reset_hr',
    'list_reset_hr', 'failed_stage', 'unaccounted_ticks', 'close_ticks',
    'execute_ticks', 'signal_ticks', 'completed_value_ticks', 'set_event_ticks',
    'wait_ticks', 'info_queue_ticks', 'retired_release_ticks', 'alloc_reset_ticks',
    'list_reset_ticks',
]


class SlowDrainAnalyzerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.fifo = self.root / 'fifo.csv'
        self.flush = self.root / 'flush.csv'
        self.pump = self.root / 'pump.csv'
        self.windows = self.root / 'windows.json'
        self.fifo_rows = [self.fifo_row(1, 7, 100, 110, 130),
                          self.fifo_row(2, 8, 130, 132, 150)]
        self.write_fifo()
        self.write_flush([self.flush_row(1, 7, 115, 125)])
        self.write_pump()
        self.windows.write_text(json.dumps({
            'complete': True, 'qpc_frequency': FREQ,
            'blocks': [{'flags': 'on', 'start_qpc': 105, 'end_qpc': 140}],
        }))

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def fifo_row(seq, tid, begin, locked, end):
        return {
            'sequence': seq, 'host_tid': tid, 'renderer_frame': 3, 'context_valid': 1,
            'qpc_begin': begin, 'qpc_lock_acquired': locked, 'qpc_body_end': end,
            'total_ms': end-begin, 'lock_wait_ms': locked-begin, 'body_ms': end-locked,
            'thread_cycles_valid': 1, 'thread_cycles_delta': (end-begin)*5,
            'get_before': '0x00000010', 'put_before': '0x00000020',
            'get_after': '0x00000010', 'put_after': '0x00000020',
        }

    @staticmethod
    def flush_row(seq, tid, begin, end):
        row = {key: 0 for key in FLUSH_HEADER}
        row.update(sequence=seq, frame=3, host_tid=tid, reason=2, qpc_begin=begin,
                   qpc_end=end, total_ticks=end-begin, thread_cycles_valid=1,
                   thread_cycles_delta=(end-begin)*7, waited=1,
                   stage_mask=(1 << 0) | (1 << 1) | (1 << 5),
                   close_ticks=2, execute_ticks=1, wait_ticks=end-begin-3)
        return row

    def write_fifo(self, rows=None, *, recorded=None, overwritten=0):
        rows = self.fifo_rows if rows is None else rows
        recorded = len(rows)+overwritten if recorded is None else recorded
        with self.fifo.open('w', newline='') as f:
            f.write(f'#qpc_frequency,{FREQ},recorded,{recorded},overwritten,{overwritten}\n')
            writer = csv.DictWriter(f, fieldnames=FIFO_HEADER)
            writer.writeheader()
            writer.writerows(rows)

    def write_fifo_with_phases(self, rows=None):
        rows = self.fifo_rows[:1] if rows is None else rows
        with self.fifo.open('w', newline='') as f:
            f.write(f'#qpc_frequency,{FREQ},recorded,{len(rows)},overwritten,0\n')
            writer = csv.DictWriter(f, fieldnames=FIFO_PHASE_HEADER)
            writer.writeheader()
            writer.writerows(rows)

    def write_flush(self, rows, *, capacity=4, overwritten=0):
        with self.flush.open('w', newline='') as f:
            retained = len(rows)
            f.write(f'#qpc_frequency,{FREQ},threshold_us,2000,capacity,{capacity},retained,{retained},overwritten,{overwritten}\n')
            writer = csv.DictWriter(f, fieldnames=FLUSH_HEADER)
            writer.writeheader()
            writer.writerows(rows)

    def write_pump(self, *, frequency=FREQ):
        fields = ['qpc_frequency', 'frame', 'flips', 'begin_qpc', 'end_qpc',
                  'cpu_valid', 'waits', 'queued_waits', 'drains', 'dt_ms', 'wait_ms',
                  'fifo_ms', 'present_ms', 'tick_ms', 'other_ms', 'thread_cpu_ms']
        with self.pump.open('w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=fields)
            w.writeheader()
            for frame, (begin, end) in enumerate(((100, 120), (120, 160), (160, 220)), 1):
                dt = (end-begin)*1000/frequency
                w.writerow(dict(qpc_frequency=frequency, frame=frame, flips=1,
                                begin_qpc=begin, end_qpc=end, cpu_valid=0, waits=0,
                                queued_waits=0, drains=1, dt_ms=dt, wait_ms=0,
                                fifo_ms=0, present_ms=0, tick_ms=0, other_ms=dt,
                                thread_cpu_ms=0))

    def test_valid_qpc_join_clips_to_pump_and_window(self):
        result = analyzer.analyze(self.fifo, self.windows, self.pump, self.flush)
        self.assertEqual(result['fifo_capture']['tail_counts']['total']['over_25_ms'], 1)
        self.assertEqual(result['fifo_capture']['worst']['lock_wait'][0]['sequence'], 1)
        self.assertEqual(result['flush_capture']['stage_ticks']['close_ticks']['sum_raw_ticks'], 2)
        self.assertEqual(result['fifo_flush_overlap']['overlapping_pairs'], 1)
        pair = result['fifo_flush_overlap']['pairs'][0]
        self.assertTrue(pair['same_thread_fifo_contains_flush'])
        self.assertEqual(pair['overlap_ticks'], 10)
        p1, p2, _ = result['pump_frames']
        first_frame = {e['sequence']: e for e in p1['fifo']['events']}
        second_frame = {e['sequence']: e for e in p2['fifo']['events']}
        self.assertEqual(first_frame[1]['end_qpc'], 120)
        self.assertTrue(first_frame[1]['clipped'])
        self.assertEqual(p1['fifo']['phase_union_overlap_ms'],
                         {'lock_wait': 10, 'body': 10})
        self.assertEqual(second_frame[1]['begin_qpc'], 120)
        self.assertTrue(result['windows'][0]['fifo']['events'][0]['clipped'])
        self.assertTrue(result['coverage'][0]['retention_complete'])
        self.assertEqual(result['worst_pump_frames_entire_capture'][0]['pump_frame'], 3)
        self.assertEqual(result['worst_pump_frames_in_windows'][0]['pump_frame'], 2)
        self.assertIn('thresholded event rings', result['coverage'][0]['interpretation'])

    def test_different_threads_are_overlap_not_nested(self):
        self.write_flush([self.flush_row(1, 99, 105, 135)])
        result = analyzer.analyze(self.fifo, flush_path=self.flush)
        pair = result['fifo_flush_overlap']['pairs'][0]
        self.assertFalse(pair['same_tid'])
        self.assertFalse(pair['same_thread_fifo_contains_flush'])
        self.assertFalse(pair['same_thread_flush_contains_fifo'])
        self.assertEqual(result['fifo_flush_overlap']['same_thread_containment_pairs'], 0)

    def test_rejects_bad_ring_counts(self):
        self.write_fifo(recorded=5)
        with self.assertRaisesRegex(ValueError, 'counts disagree'):
            analyzer.analyze(self.fifo)

    def test_rejects_invalid_phase_partition(self):
        rows = list(self.fifo_rows)
        rows[0] = dict(rows[0], body_ms=21)
        self.write_fifo(rows)
        with self.assertRaisesRegex(ValueError, 'body duration|partition total'):
            analyzer.analyze(self.fifo)

    def test_rejects_nonmonotonic_lock_order(self):
        rows = [self.fifo_row(1, 7, 100, 140, 145),
                self.fifo_row(2, 8, 110, 120, 125)]
        self.write_fifo(rows)
        with self.assertRaisesRegex(ValueError, 'sequence does not follow lock acquisition'):
            analyzer.analyze(self.fifo)

    def test_rejects_frequency_mismatch(self):
        self.write_pump(frequency=FREQ*2)
        with self.assertRaisesRegex(ValueError, 'frequencies differ'):
            analyzer.analyze(self.fifo, pump_path=self.pump)

    def test_nested_fifo_phases_parse_and_reconcile(self):
        row = dict(self.fifo_rows[0])
        row.update(phase_mask=0x1ff, prewalk_setup_ticks=2, walker_ticks=3,
                   postwalk_ticks=1, recycle_ticks=4, post_recycle_ticks=1,
                   drained_ea_publish_ticks=1, get_write_ticks=1,
                   reference_lock_wait_ticks=3, reference_publish_ticks=2,
                   phase_unaccounted_ticks=2,
                   stopped_reason='caught-up')
        self.write_fifo_with_phases([row])
        result = analyzer.analyze(self.fifo)
        phases = result['fifo_capture']['nested_phase_ticks']
        self.assertEqual(phases['walker_ticks']['sum_raw_ticks'], 3)
        self.assertEqual(phases['recycle_ticks']['sum_raw_ticks'], 4)

    def test_rejects_incomplete_nested_fifo_schema_and_bad_partition(self):
        row = dict(self.fifo_rows[0])
        row.update(phase_mask=0x1ff, prewalk_setup_ticks=2, walker_ticks=3,
                   postwalk_ticks=1, recycle_ticks=4, post_recycle_ticks=1,
                   drained_ea_publish_ticks=1, get_write_ticks=1,
                   reference_lock_wait_ticks=3, reference_publish_ticks=2,
                   phase_unaccounted_ticks=3,
                   stopped_reason='caught-up')
        self.write_fifo_with_phases([row])
        with self.assertRaisesRegex(ValueError, 'nested phases do not partition'):
            analyzer.analyze(self.fifo)
        with self.fifo.open('w', newline='') as f:
            f.write(f'#qpc_frequency,{FREQ},recorded,1,overwritten,0\n')
            writer = csv.DictWriter(f, fieldnames=FIFO_HEADER + ['phase_mask'])
            writer.writeheader()
            writer.writerow({**self.fifo_rows[0], 'phase_mask': 0})
        with self.assertRaisesRegex(ValueError, 'incomplete nested-phase schema'):
            analyzer.analyze(self.fifo)

    def test_rejects_flush_metadata_or_interval_mismatch(self):
        row = self.flush_row(1, 7, 115, 125)
        row['total_ticks'] = 9
        self.write_flush([row])
        with self.assertRaisesRegex(ValueError, 'total ticks disagree'):
            analyzer.analyze(self.fifo, flush_path=self.flush)

    def test_rejects_flush_stage_partition_and_unknown_mask(self):
        row = self.flush_row(1, 7, 115, 125)
        row['unaccounted_ticks'] = 1
        self.write_flush([row])
        with self.assertRaisesRegex(ValueError, 'do not partition total'):
            analyzer.analyze(self.fifo, flush_path=self.flush)
        row['unaccounted_ticks'] = 0
        row['stage_mask'] |= 1 << 10
        self.write_flush([row])
        with self.assertRaisesRegex(ValueError, 'unknown stage-mask bits'):
            analyzer.analyze(self.fifo, flush_path=self.flush)

    def test_empty_diagnostic_is_valid_and_coverage_is_unknown(self):
        self.write_fifo([], recorded=0)
        result = analyzer.analyze(self.fifo, self.windows)
        self.assertEqual(result['fifo_capture']['events'], 0)
        self.assertFalse(result['coverage'][0]['inside_retained_event_extent'])
        self.assertIn('do not provide continuous', result['coverage'][0]['interpretation'])


if __name__ == '__main__':
    unittest.main()
