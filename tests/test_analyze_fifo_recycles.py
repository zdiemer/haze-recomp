import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "analyze_fifo_recycles", ROOT / "scripts/analyze_fifo_recycles.py")
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


FIELDS = [
    "sequence", "host_tid", "renderer_frame", "qpc_begin", "qpc_end", "total_ms",
    "context_ea", "begin_ea", "current_ea", "begin_io", "current_io", "outcome",
    "ack_current_expected_ea", "ack_current_initial_observed_ea", "ack_current_observed_ea",
    "ack_current_begin", "ack_current_end", "ack_current_ms", "ack_current_wait_calls",
    "ack_current_completed", "ack_begin_expected_ea", "ack_begin_initial_observed_ea",
    "ack_begin_observed_ea", "ack_begin_begin", "ack_begin_end", "ack_begin_ms",
    "ack_begin_wait_calls", "ack_begin_completed",
]


def event(sequence=1, begin=100, end=140, outcome=0):
    row = {field: 0 for field in FIELDS}
    row.update(sequence=sequence, host_tid=10, renderer_frame=sequence,
               qpc_begin=begin, qpc_end=end, total_ms=(end-begin)*.001,
               context_ea="0x1000", begin_ea="0x2000", current_ea="0x3000",
               begin_io="0x00502000", current_io="0x00503000", outcome=outcome)
    if outcome == 0:
        row.update(ack_current_expected_ea="0x3000", ack_current_initial_observed_ea="0x00000000",
                   ack_current_observed_ea="0x3000", ack_current_begin=110,
                   ack_current_end=120, ack_current_ms=.01, ack_current_wait_calls=1,
                   ack_current_completed=1, ack_begin_expected_ea="0x2000",
                   ack_begin_initial_observed_ea="0x3000", ack_begin_observed_ea="0x2000",
                   ack_begin_begin=125, ack_begin_end=135, ack_begin_ms=.01,
                   ack_begin_wait_calls=1, ack_begin_completed=1)
    else:
        for field in FIELDS:
            if field.endswith("_ea") or field.endswith("_io"):
                row[field] = "0x00000000"
    return row


def write_capture(path, rows, *, recorded=None, overwritten=0, started=None,
                  completed=None, inflight=0):
    recorded = len(rows) + overwritten if recorded is None else recorded
    completed = recorded if completed is None else completed
    started = completed + inflight if started is None else started
    with path.open("w", newline="", encoding="utf-8") as stream:
        stream.write(f"#qpc_frequency,1000000,recorded,{recorded},overwritten,{overwritten},started,{started},completed,{completed},inflight_at_dump,{inflight}\n")
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


class FifoRecycleAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_window_clips_callbacks_acks_and_correlates_pump_by_qpc(self):
        trace = self.root / "recycle.csv"
        pump = self.root / "pump.csv"
        write_capture(trace, [event(), event(2, 200, 202, outcome=2)])
        pump.write_text(
            "qpc_frequency,frame,begin_qpc,end_qpc\n"
            "1000000,17,90,130\n1000000,18,130,210\n", encoding="utf-8")
        result = analyzer.analyze(trace, 115, 205, pump)
        self.assertEqual(result["summary"]["selected_callbacks"], 2)
        self.assertAlmostEqual(result["summary"]["callback_ms"], .027)
        self.assertAlmostEqual(result["summary"]["current_ack_ms"], .005)
        self.assertAlmostEqual(result["summary"]["begin_ack_ms"], .010)
        self.assertEqual(result["summary"]["current_wait_calls_whole_intersecting_callbacks"], 1)
        self.assertEqual(result["events"][0]["ack_current_wait_calls"], 1)
        self.assertEqual([x["pump_frame"] for x in result["pump_overlap_by_qpc"]], [17, 18, 18])
        self.assertEqual(result["events"][0]["renderer_frame"], 1)
        self.assertEqual(result["events"][0]["window_overlap_qpc"], 25)

    def test_sequence_is_completion_order_not_start_order(self):
        trace = self.root / "overlap.csv"
        first_completed = event(1, 150, 200)
        first_completed.update(ack_current_begin=155, ack_current_end=160,
                               ack_current_ms=.005, ack_begin_begin=165,
                               ack_begin_end=170, ack_begin_ms=.005)
        second_completed = event(2, 100, 250)
        write_capture(trace, [first_completed, second_completed])
        report = analyzer.analyze(trace)
        self.assertEqual([e["sequence"] for e in report["events"]], [1, 2])
        self.assertEqual([e["qpc_begin"] for e in report["events"]], [150, 100])

    def test_inflight_and_overwritten_are_explicit_partial_coverage(self):
        trace = self.root / "partial.csv"
        write_capture(trace, [event(1, 100, 102, outcome=2)], started=2,
                      completed=1, inflight=1)
        self.assertFalse(analyzer.analyze(trace)["coverage"]["complete_for_capture"])

        # Exercise an actually full retained ring with one overwritten event.
        rows = [event(i, 100+i, 101+i, outcome=2)
                for i in range(2, analyzer.CAPACITY + 2)]
        write_capture(trace, rows, recorded=analyzer.CAPACITY + 1,
                      overwritten=1, started=analyzer.CAPACITY + 1,
                      completed=analyzer.CAPACITY + 1)
        report = analyzer.analyze(trace)
        self.assertEqual(report["coverage"]["retained"], analyzer.CAPACITY)
        self.assertFalse(report["coverage"]["complete_for_capture"])
        self.assertEqual(report["events"][0]["sequence"], 2)

    def test_rejects_sequence_metadata_and_ack_order_errors(self):
        trace = self.root / "bad.csv"
        write_capture(trace, [event(2)])
        with self.assertRaisesRegex(ValueError, "sequence"):
            analyzer.analyze(trace)
        write_capture(trace, [event(1)])
        text = trace.read_text(encoding="utf-8")
        trace.write_text(text.replace("recorded,1", "recorded,2"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "counts disagree"):
            analyzer.analyze(trace)
        bad = event()
        bad["ack_begin_begin"] = 115
        bad["ack_begin_ms"] = .020
        write_capture(trace, [bad])
        with self.assertRaisesRegex(ValueError, "ordered"):
            analyzer.analyze(trace)
        bad = event()
        bad["ack_current_expected_ea"] = "0x00003001"
        bad["ack_current_observed_ea"] = "0x00003001"
        write_capture(trace, [bad])
        with self.assertRaisesRegex(ValueError, "expected EAs disagree"):
            analyzer.analyze(trace)

    def test_rejects_qpc_frequency_mismatch_and_malformed_rows(self):
        trace = self.root / "trace.csv"
        pump = self.root / "pump.csv"
        write_capture(trace, [event()])
        pump.write_text("qpc_frequency,frame,begin_qpc,end_qpc\n100,1,1,2\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "frequencies differ"):
            analyzer.analyze(trace, pump_path=pump)
        trace.write_text(trace.read_text(encoding="utf-8") + "broken,row\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            analyzer.analyze(trace)


if __name__ == "__main__":
    unittest.main()
