"""Correlation tests for per-present pump and PPU wait timelines."""
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from diagnostics.correlate_pump_waits import correlate, summarize_windows


PUMP_FIELDS = [
    "qpc_frequency", "frame", "flips", "begin_qpc", "end_qpc", "dt_ms",
    "wait_ms", "fifo_ms", "present_ms", "tick_ms", "other_ms",
    "thread_cpu_ms", "cpu_valid", "waits", "queued_waits", "drains",
]
WAIT_FIELDS = [
    "begin_qpc", "end_qpc", "frequency", "host_tid", "guest_tid", "kind",
    "id", "lr", "arg0", "arg1", "arg2", "result", "stack_lr0",
    "stack_lr1", "stack_lr2",
]


def pump_row(frame, begin, end):
    return dict(qpc_frequency=1000, frame=frame, flips=frame,
                begin_qpc=begin, end_qpc=end, dt_ms=end-begin,
                wait_ms=4.0, fifo_ms=2.0, present_ms=1.0, tick_ms=1.0,
                other_ms=2.0, thread_cpu_ms=3.0, cpu_valid=1,
                waits=2, queued_waits=1, drains=3)


def wait_row(begin, end, host, kind, ident, lr, arg0="0"):
    return dict(begin_qpc=begin, end_qpc=end, frequency=1000,
                host_tid=host, guest_tid=1, kind=kind, id=ident, lr=lr,
                arg0=arg0, arg1="0", arg2="0", result="0",
                stack_lr0="0", stack_lr1="0", stack_lr2="0")


class PumpWaitCorrelationTests(unittest.TestCase):
    def setUp(self):
        self.scratch = ROOT / "local"
        self.scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="pump-waits-", dir=self.scratch)
        self.root = Path(self.temp.name)
        self.pump = self.root / "pump.csv"
        self.wait_dir = self.root / "waits"
        self.wait_dir.mkdir()
        self.pumps = [pump_row(1, 100, 110), pump_row(2, 110, 120)]
        self.waits = [
            # Clip across both present intervals; nested with the syscall below.
            wait_row(98, 113, 10, "hle", "00000001", "AAAA0001"),
            wait_row(105, 115, 10, "syscall", "0000006B", "BBBB0002", "7"),
            # A separate host thread's overlapping service is unioned separately.
            wait_row(102, 107, 20, "syscall", "0000008D", "CCCC0003", "14A"),
        ]

    def tearDown(self):
        self.temp.cleanup()

    def write_csv(self, path, fields, rows):
        with path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def run_correlation(self, waits=None, pumps=None, **kwargs):
        self.write_csv(self.pump, PUMP_FIELDS, self.pumps if pumps is None else pumps)
        path = self.wait_dir / "wait-10.csv"
        self.write_csv(path, WAIT_FIELDS, self.waits if waits is None else waits)
        return correlate(self.pump, self.wait_dir, guest_tid=1, **kwargs)

    def test_clips_waits_and_unions_nested_scopes_per_host_thread(self):
        result = self.run_correlation()
        frames = result["slowest"]
        self.assertEqual([row["frame"] for row in frames], [1, 2])
        first, second = frames
        self.assertEqual(first["wait_union_ms"], 15.0)
        self.assertEqual(first["wait_union_by_host_tid_ms"], {"10": 10.0, "20": 5.0})
        self.assertEqual(second["wait_union_ms"], 5.0)
        self.assertEqual(second["wait_union_by_host_tid_ms"], {"10": 5.0})
        self.assertEqual(first["pump"], {
            "wait_ms": 4.0, "fifo_ms": 2.0, "present_ms": 1.0,
            "tick_ms": 1.0, "other_ms": 2.0})
        self.assertEqual(result["all"]["means_ms"]["wait_union_ms"], 10.0)

    def test_caller_groups_keep_syscall_and_hle_separate(self):
        result = self.run_correlation()
        groups = {(row["kind"], row["id"]): row for row in result["all"]["callers"]}
        self.assertIn(("hle", "00000001"), groups)
        self.assertIn(("syscall", "0000006B"), groups)
        self.assertIn(("syscall", "0000008D"), groups)
        self.assertEqual(groups[("hle", "00000001")]["wall_ms"], 13.0)
        self.assertEqual(groups[("syscall", "0000006B")]["wall_ms"], 10.0)
        self.assertEqual(groups[("syscall", "0000008D")]["wall_ms"], 5.0)

    def test_multiple_host_threads_are_not_union_collapsed(self):
        result = self.run_correlation(waits=[
            wait_row(101, 109, 10, "syscall", "0000006B", "AAAA", "7"),
            wait_row(101, 109, 20, "syscall", "0000006B", "AAAA", "7"),
        ])
        first = result["slowest"][0]
        self.assertEqual(first["wait_union_by_host_tid_ms"], {"10": 8.0, "20": 8.0})
        self.assertEqual(first["wait_union_ms"], 16.0)

    def test_rejects_wait_frequency_mismatch(self):
        rows = [dict(row) for row in self.waits]
        rows[0]["frequency"] = 2000
        with self.assertRaisesRegex(ValueError, "frequency mismatch"):
            self.run_correlation(waits=rows)

    def test_rejects_invalid_wait_interval(self):
        rows = [dict(row) for row in self.waits]
        rows[0]["end_qpc"] = rows[0]["begin_qpc"]
        with self.assertRaisesRegex(ValueError, "Invalid PPU wait interval"):
            self.run_correlation(waits=rows)

    def test_rejects_present_manifest_frequency_mismatch(self):
        self.write_csv(self.pump, PUMP_FIELDS, self.pumps)
        self.write_csv(self.wait_dir / "wait-10.csv", WAIT_FIELDS, self.waits)
        windows = self.root / "windows.json"
        windows.write_text(json.dumps({"qpc_frequency": 2000, "blocks": []}))
        with self.assertRaisesRegex(ValueError, "Present window QPC frequency mismatch"):
            summarize_windows(self.pump, self.wait_dir, windows)


if __name__ == "__main__":
    unittest.main()
