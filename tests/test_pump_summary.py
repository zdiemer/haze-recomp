"""Regression tests for per-present render-pump CSV accounting."""
import csv
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from summarize_pump import summarize


PUMP_FIELDS = [
    "qpc_frequency", "frame", "flips", "begin_qpc", "end_qpc", "dt_ms",
    "wait_ms", "fifo_ms", "present_ms", "tick_ms", "other_ms",
    "thread_cpu_ms", "cpu_valid", "waits", "queued_waits", "drains",
]
RENDER_FIELDS = [
    "frame", "qpc", "qpc_frequency", "draw_cpu_ms", "vertex_pack_ms",
    "fence_wait_ms", "gpu_ms", "vp_prepare_ms", "fp_prepare_ms",
    "texture_hash_ms", "texture_decode_ms", "pso_lookup_ms",
    "frame_limit_ms", "blit_cpu_ms",
]


def pump_row(frame, begin, end, *, wait, fifo, present, tick, other,
             frequency=1000):
    return dict(qpc_frequency=frequency, frame=frame, flips=frame,
                begin_qpc=begin, end_qpc=end, dt_ms=(end-begin)*1000/frequency,
                wait_ms=wait, fifo_ms=fifo, present_ms=present, tick_ms=tick,
                other_ms=other, thread_cpu_ms=4.0, cpu_valid=1,
                waits=2, queued_waits=1, drains=3)


def renderer_row(frame, qpc, frequency=1000):
    return dict(frame=frame, qpc=qpc, qpc_frequency=frequency,
                draw_cpu_ms=1.0, vertex_pack_ms=0.5, fence_wait_ms=0.2,
                gpu_ms=0.3, vp_prepare_ms=0.1, fp_prepare_ms=0.1,
                texture_hash_ms=0.2, texture_decode_ms=0.1,
                pso_lookup_ms=0.05, frame_limit_ms=0.0, blit_cpu_ms=0.1)


class PumpSummaryTests(unittest.TestCase):
    def setUp(self):
        self.scratch = ROOT / "local"
        self.scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="pump-summary-", dir=self.scratch)
        self.root = Path(self.temp.name)
        self.pump_path = self.root / "pump.csv"
        self.renderer_path = self.root / "renderer.csv"
        self.pump_rows = [
            pump_row(1, 100, 110, wait=2, fifo=3, present=1, tick=1, other=3),
            pump_row(2, 110, 130, wait=5, fifo=5, present=2, tick=1, other=7),
        ]
        self.renderer_rows = [renderer_row(1, 105), renderer_row(2, 120)]

    def tearDown(self):
        self.temp.cleanup()

    def write_csv(self, path, fields, rows):
        with path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def run_summary(self, *, pumps=None, renderer=None, start=None, end=None):
        self.write_csv(self.pump_path, PUMP_FIELDS,
                       self.pump_rows if pumps is None else pumps)
        if renderer is not None:
            self.write_csv(self.renderer_path, RENDER_FIELDS, renderer)
        return summarize(self.pump_path, start, end,
                         self.renderer_path if renderer is not None else None)

    def test_valid_disjoint_intervals_and_one_renderer_stamp_each(self):
        result = self.run_summary(renderer=self.renderer_rows, start=100, end=130)
        self.assertEqual(result["all"]["frames"], 2)
        self.assertEqual(result["all"]["means_ms"]["dt_ms"], 15.0)
        self.assertEqual(result["all"]["means_ms"]["wait_ms"], 3.5)
        self.assertEqual(result["all"]["nested_renderer_means_ms"]["draw_cpu_ms"], 1.0)
        self.assertEqual([r["frame"] for r in result["slowest"]], [2, 1])

    def test_retains_additional_renderer_scopes(self):
        self.write_csv(self.pump_path, PUMP_FIELDS, self.pump_rows)
        fields=RENDER_FIELDS+['pso_prepare_ms','shader_disk_load_ms']
        rows=[dict(r,pso_prepare_ms=140,shader_disk_load_ms=130) for r in self.renderer_rows]
        self.write_csv(self.renderer_path,fields,rows)
        result=summarize(self.pump_path,renderer=self.renderer_path)
        self.assertEqual(result['all']['nested_renderer_means_ms']['pso_prepare_ms'],140)
        self.assertEqual(result['slowest'][0]['renderer']['shader_disk_load_ms'],130)
        rows[0]['shader_disk_load_ms']=float('nan')
        self.write_csv(self.renderer_path,fields,rows)
        with self.assertRaisesRegex(ValueError,'Nonfinite renderer'):
            summarize(self.pump_path,renderer=self.renderer_path)

    def test_rejects_overlapping_scopes(self):
        rows = [dict(r) for r in self.pump_rows]
        rows[0]["fifo_ms"] = 4  # Pump scopes now sum to 11ms inside a 10ms row.
        with self.assertRaisesRegex(ValueError, "Scope totals"):
            self.run_summary(pumps=rows)

    def test_rejects_negative_residual(self):
        rows = [dict(r) for r in self.pump_rows]
        rows[0]["other_ms"] = -0.1
        with self.assertRaisesRegex(ValueError, "Invalid interval accounting"):
            self.run_summary(pumps=rows)

    def test_rejects_nonfinite_accounting(self):
        for value in (float('nan'), float('inf')):
            rows = [dict(r) for r in self.pump_rows]
            rows[0]['fifo_ms'] = value
            with self.assertRaisesRegex(ValueError, 'Nonfinite'):
                self.run_summary(pumps=rows)

    def test_rejects_invalid_window(self):
        with self.assertRaisesRegex(ValueError, 'Invalid requested'):
            self.run_summary(start=110, end=100)

    def test_rejects_nonconsecutive_rows(self):
        rows = [dict(r) for r in self.pump_rows]
        rows[1]["begin_qpc"] = 111
        rows[1]["dt_ms"] = 19.0
        rows[1]["other_ms"] = 6.0
        with self.assertRaisesRegex(ValueError, "Nonconsecutive"):
            self.run_summary(pumps=rows)

    def test_rejects_missing_window_coverage(self):
        with self.assertRaisesRegex(ValueError, "does not cover"):
            self.run_summary(start=99, end=130)
        with self.assertRaisesRegex(ValueError, "does not cover"):
            self.run_summary(start=100, end=131)

    def test_rejects_duplicate_renderer_timestamps(self):
        rows = [dict(r) for r in self.renderer_rows]
        rows[1]["qpc"] = rows[0]["qpc"]
        with self.assertRaisesRegex(ValueError, "Nonmonotonic renderer"):
            self.run_summary(renderer=rows)

    def test_rejects_missing_renderer_timestamp_for_a_present(self):
        with self.assertRaisesRegex(ValueError, "got 0"):
            self.run_summary(renderer=[self.renderer_rows[0]])

    def test_rejects_multiple_renderer_timestamps_for_a_present(self):
        rows = [self.renderer_rows[0], renderer_row(10, 106), self.renderer_rows[1]]
        with self.assertRaisesRegex(ValueError, "got 2"):
            self.run_summary(renderer=rows)

    def test_rejects_renderer_frequency_mismatch(self):
        rows = [renderer_row(1, 105, 2000), self.renderer_rows[1]]
        with self.assertRaisesRegex(ValueError, "frequency mismatch"):
            self.run_summary(renderer=rows)

    def outcome_rows(self):
        return [dict(r, signaled_waits=1, timeout_waits=1, sleep_waits=0,
                     failed_waits=0, signaled_wait_ms=r['wait_ms']*.25,
                     timeout_wait_ms=r['wait_ms']*.75, sleep_wait_ms=0,
                     failed_wait_ms=0, max_wait_ms=r['wait_ms']*.75,
                     queued_after_waits=1, empty_signals=0) for r in self.pump_rows]

    def outcome_summary(self, rows):
        self.write_csv(self.pump_path,list(rows[0]),rows)
        return summarize(self.pump_path)

    def test_wait_outcomes_partition_and_summary(self):
        result=self.outcome_summary(self.outcome_rows())['all']['wait_outcomes']
        self.assertEqual(result['total_counts']['signaled_waits'],2)
        self.assertEqual(result['total_counts']['timeout_waits'],2)
        self.assertAlmostEqual(result['means_ms_per_frame']['signaled_wait_ms'],.875)
        self.assertAlmostEqual(result['means_ms_per_frame']['timeout_wait_ms'],2.625)
        self.assertAlmostEqual(result['maximum_individual_wait_ms'],3.75)

    def test_rejects_partial_outcome_schema(self):
        rows=self.outcome_rows()
        for row in rows: del row['max_wait_ms']
        with self.assertRaisesRegex(ValueError,'Incomplete wait outcome'):
            self.outcome_summary(rows)

    def test_rejects_invalid_outcome_partitions(self):
        cases=[('signaled_waits',2,'counts do not partition'),
               ('timeout_wait_ms',.9,'times do not partition'),
               ('queued_after_waits',3,'post-wait queue'),
               ('empty_signals',2,'post-wait queue'),
               ('max_wait_ms',3,'exceeds wait total'),
               ('max_wait_ms',float('nan'),'Invalid wait outcome'),
               ('failed_waits',-1,'Negative wait outcome')]
        for key,value,message in cases:
            with self.subTest(key=key):
                rows=self.outcome_rows()
                rows[0][key]=value
                with self.assertRaisesRegex(ValueError,message):
                    self.outcome_summary(rows)

    def test_legacy_csv_remains_supported(self):
        self.assertNotIn('wait_outcomes',self.run_summary()['all'])


if __name__ == "__main__":
    unittest.main()
