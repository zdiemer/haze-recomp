import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "analyze_producer_profile", ROOT / "scripts/analyze_producer_profile.py")
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


FIELDS = ["kind", "id", "parent_id", "generation", "guest_ea", "callsite_lr",
          "callee_ea", "host_tid", "qpc_begin", "qpc_end", "qpc_valid",
          "cycles_delta", "cycles_valid", "completed"]


def root_row(identifier=1, begin=100, end=300, *, complete=1, qpc_valid=1,
             cycles=100, cycles_valid=1, parent=0):
    return dict(kind=0, id=identifier, parent_id=parent, generation=0,
                guest_ea=0x00680FA0, callsite_lr=0, callee_ea=0, host_tid=22,
                qpc_begin=begin, qpc_end=end if complete else 0,
                qpc_valid=qpc_valid if complete else 0,
                cycles_delta=cycles if complete else 0,
                cycles_valid=cycles_valid if complete else 0, completed=complete)


def call_row(identifier, parent, begin, end, *, lr=0x00681004, callee=0x0068ECD8,
             cycles=40, cycles_valid=1, host_tid=22):
    return dict(kind=1, id=identifier, parent_id=parent, generation=0, guest_ea=0,
                callsite_lr=f"0x{lr:08X}", callee_ea=f"0x{callee:08X}",
                host_tid=host_tid, qpc_begin=begin, qpc_end=end, qpc_valid=1,
                cycles_delta=cycles, cycles_valid=cycles_valid, completed=1)


def make_capture(path, rows, *, capacity=64, begun=None, completed=None, inflight=0,
                 overwritten=None, overwritten_inflight=0, late_discarded=0,
                 discarded_invalid_qpc=0,
                 max_discarded_end=0, earliest_overwritten_begin=0, dump_qpc=400,
                 dump_qpc_valid=1):
    begun = len(rows) if begun is None else begun
    completed = begun - inflight - late_discarded if completed is None else completed
    overwritten = max(0, begun-capacity) if overwritten is None else overwritten
    retained_completed = sum(row["completed"] for row in rows)
    retained_inflight = len(rows)-retained_completed
    first_id = rows[0]["id"] if rows else 0
    last_id = rows[-1]["id"] if rows else 0
    first_begin = rows[0]["qpc_begin"] if rows else 0
    with path.open("w", newline="", encoding="utf-8") as stream:
        stream.write(
            "#qpc_frequency,1000000,begun,{begun},completed,{completed},inflight_at_dump,{inflight},"
            "overwritten,{overwritten},overwritten_inflight,{overwritten_inflight},late_discarded,{late_discarded},"
            "discarded_invalid_qpc_observations,{discarded_invalid_qpc},"
            "max_discarded_end_qpc,{max_discarded_end},earliest_overwritten_inflight_begin_qpc,{earliest_overwritten_begin},"
            "capacity,{capacity},retained_rows,{retained},retained_completed_rows,{retained_completed},"
            "retained_inflight_rows,{retained_inflight},retained_first_id,{first_id},retained_last_id,{last_id},"
            "retained_first_begin_qpc,{first_begin},dump_qpc,{dump_qpc},dump_qpc_valid,{dump_qpc_valid}\n".format(
                begun=begun, completed=completed, inflight=inflight, overwritten=overwritten,
                overwritten_inflight=overwritten_inflight, late_discarded=late_discarded,
                discarded_invalid_qpc=discarded_invalid_qpc,
                max_discarded_end=max_discarded_end, earliest_overwritten_begin=earliest_overwritten_begin,
                capacity=capacity, retained=len(rows), retained_completed=retained_completed,
                retained_inflight=retained_inflight, first_id=first_id, last_id=last_id,
                first_begin=first_begin, dump_qpc=dump_qpc, dump_qpc_valid=dump_qpc_valid))
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


class ProducerProfileAnalyzerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_nested_spans_conserve_root_union_without_double_counting(self):
        path = self.root / "profile.csv"
        rows = [root_row(), call_row(2, 1, 120, 240),
                call_row(3, 2, 150, 180, lr=0x00681014, callee=0x00719EF8)]
        make_capture(path, rows)
        result = analyzer.analyze(path)
        self.assertAlmostEqual(result["roots"]["inclusive_sum_ms"], .2)
        self.assertAlmostEqual(result["roots"]["interval_union_ms_no_nested_double_count"], .2)
        self.assertAlmostEqual(result["roots"]["direct_child_union_ms_no_nested_double_count"], .12)
        self.assertAlmostEqual(result["roots"]["uncovered_by_profiled_direct_calls_union_ms"], .08)
        self.assertEqual(result["roots"]["cycles_delta_raw_sum"], 100)
        self.assertEqual(len(result["direct_calls_grouped_by_lr_callee"]), 1)
        group = result["direct_calls_grouped_by_lr_callee"][0]
        self.assertEqual(group["callsite_lr"], 0x00681004)
        self.assertEqual(group["cycles_delta_raw_sum"], 40)
        self.assertEqual(result["capture"]["requested_window_coverage_proven"], False)

    def test_window_clips_scopes_and_joins_pump_by_qpc(self):
        path, pump = self.root / "profile.csv", self.root / "pump.csv"
        rows = [root_row(), call_row(2, 1, 120, 260)]
        make_capture(path, rows)
        pump.write_text("qpc_frequency,frame,begin_qpc,end_qpc\n"
                        "1000000,99,90,180\n1000000,100,180,310\n", encoding="utf-8")
        result = analyzer.analyze(path, 150, 250, pump)
        self.assertAlmostEqual(result["roots"]["inclusive_sum_ms"], .1)
        self.assertAlmostEqual(result["roots"]["direct_child_union_ms_no_nested_double_count"], .1)
        self.assertAlmostEqual(result["roots"]["uncovered_by_profiled_direct_calls_union_ms"], 0)
        group = result["direct_calls_grouped_by_lr_callee"][0]
        self.assertAlmostEqual(group["window_inclusive_wall_ms"], .1)
        self.assertAlmostEqual(group["whole_scope_inclusive_wall_ms"], .14)
        self.assertEqual(result["pump_qpc_overlap"]["matched_pump_intervals"], 2)
        self.assertAlmostEqual(result["pump_qpc_overlap"]["root_qpc_union_ms"], .1)

    def test_nested_targets_are_partitioned_by_parent_without_changing_root_cost(self):
        path = self.root / "targets.csv"
        rows = [root_row(1, 100, 400),
                call_row(2, 1, 120, 220, callee=0x006869F0),
                call_row(3, 2, 140, 200, lr=0x00686AC8, callee=0x00123450, cycles=25),
                call_row(4, 1, 240, 360, callee=0x00697C70),
                call_row(5, 4, 250, 350, lr=0x00686AC8, callee=0x00123450, cycles=55)]
        make_capture(path, rows, dump_qpc=500)
        result = analyzer.analyze(path, 160, 300)
        groups = [g for g in result["all_calls_grouped_by_parent_lr_callee"]
                  if g["callee_ea"] == 0x00123450]
        self.assertEqual(len(groups), 2)
        by_parent = {g["parent_function_ea"]: g for g in groups}
        first = by_parent[0x006869F0]
        self.assertAlmostEqual(first["window_inclusive_wall_ms"], .04)
        self.assertAlmostEqual(first["whole_scope_distribution_ms"]["mean"], .06)
        self.assertEqual(first["cycles_delta_raw_whole_scope_sum"], 25)
        self.assertAlmostEqual(by_parent[0x00697C70]["window_inclusive_wall_ms"], .05)
        self.assertAlmostEqual(result["roots"]["inclusive_sum_ms"], .14)
        self.assertAlmostEqual(result["roots"]["direct_child_union_ms_no_nested_double_count"], .12)

    def test_nested_frame_is_not_summed_as_an_independent_root(self):
        path = self.root / "nested-frames.csv"
        rows = [root_row(1, 100, 300), root_row(2, 150, 250, parent=1),
                call_row(3, 2, 175, 225)]
        make_capture(path, rows)
        result = analyzer.analyze(path)
        self.assertEqual(result["roots"]["completed_valid_qpc_frames"], 1)
        self.assertEqual(result["roots"]["nested_frame_scopes_excluded_from_root_totals"], 1)
        self.assertAlmostEqual(result["roots"]["interval_union_ms_no_nested_double_count"], .2)
        self.assertAlmostEqual(result["roots"]["direct_child_union_ms_no_nested_double_count"], .1)

    def test_inflight_overwritten_and_late_discarded_make_window_partial(self):
        path = self.root / "partial.csv"
        rows = [root_row(2), call_row(3, 1, 120, 160), root_row(4, 350, 0, complete=0)]
        make_capture(path, rows, capacity=3, begun=4, completed=2, inflight=1,
                     overwritten=1, overwritten_inflight=1, late_discarded=1,
                     max_discarded_end=280, earliest_overwritten_begin=90)
        result = analyzer.analyze(path, 200, 390)
        self.assertTrue(result["window_coverage"]["potentially_incomplete"])
        self.assertIn("a discarded completed scope may overlap the requested window",
                      result["window_coverage"]["reasons"])
        self.assertIn("an overwritten in-flight scope may overlap the requested window",
                      result["window_coverage"]["reasons"])
        self.assertIn("an unfinished retained scope may overlap the requested window",
                      result["window_coverage"]["reasons"])

    def test_orphan_parent_and_invalid_clock_cycle_flags_are_visible(self):
        path = self.root / "orphan.csv"
        # The root row (id 1) was overwritten; retained call id 3 is an orphan.
        rows = [call_row(2, 1, 120, 150), root_row(3, 200, 250, cycles_valid=0)]
        make_capture(path, rows, capacity=2, begun=3, completed=3, overwritten=1,
                     overwritten_inflight=0, late_discarded=0, max_discarded_end=180)
        result = analyzer.analyze(path, 100, 300)
        self.assertEqual(result["orphan_calls"]["count"], 1)
        self.assertFalse(result["window_coverage"]["known_completed_valid_qpc_only"] is False)
        self.assertTrue(result["window_coverage"]["potentially_incomplete"])

    def test_rejects_wrong_frequency_parent_containment_and_bad_clock_flags(self):
        path = self.root / "bad.csv"
        rows = [root_row(), call_row(2, 1, 90, 180)]
        make_capture(path, rows)
        with self.assertRaisesRegex(ValueError, "begins before its parent"):
            analyzer.analyze(path)
        rows = [root_row(), call_row(2, 1, 120, 310)]
        make_capture(path, rows)
        with self.assertRaisesRegex(ValueError, "outside its parent's"):
            analyzer.analyze(path)
        rows = [root_row(cycles_valid=0, cycles=0)]
        make_capture(path, rows)
        with path.open(encoding="utf-8") as stream:
            data = stream.read().replace("qpc_frequency,1000000", "qpc_frequency,0")
        path.write_text(data, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "frequency"):
            analyzer.analyze(path)


if __name__ == "__main__":
    unittest.main()
