import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "analyze_texture_misses", ROOT / "scripts" / "analyze_texture_misses.py"
)
ANALYZER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(ANALYZER)


def trace_row(event, qpc, serial, *, frame=4, location=1, offset=20, remap=0,
              content_hash=55, decoded=1, evicted=0):
    return {
        "event": event, "frame": frame, "qpc": qpc, "use_serial": serial,
        "slot": 2, "evicted": evicted, "previous_use_serial": 0,
        "location": location, "offset": offset, "format": 1,
        "width": 64, "height": 32, "pitch": 256, "remap": remap,
        "cubemap": 0, "mipmaps": 1, "dimension": 2,
        "content_hash": content_hash, "decoded": decoded,
    }


class TextureMissAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="texture-misses-")
        self.root = Path(self.temp.name)
        self.rows = [
            trace_row((1 << 64) - 2, 100, (1 << 64) - 2, frame=4),
            trace_row((1 << 64) - 1, 110, (1 << 64) - 1, frame=4),
            trace_row(0, 120, 0, remap=1, evicted=1, frame=4),
            trace_row(1, 130, 1, location=2, offset=40, content_hash=66, decoded=0, frame=5),
        ]

    def tearDown(self):
        self.temp.cleanup()

    def write_csv(self, path, fields, rows):
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def test_wrap_repeats_identity_sensitivity_and_windows_reconcile(self):
        windows = [
            {"index": 0, "start_qpc": 100, "end_qpc": 120},
            {"index": 1, "start_qpc": 121, "end_qpc": 140},
        ]
        renderer = [
            {"qpc": 115, "frame": 5, "texture_decode_miss_calls": 3},
            {"qpc": 130, "frame": 6, "texture_decode_miss_calls": 1},
        ]
        result = ANALYZER.analyze(self.rows, windows, renderer)
        self.assertTrue(result["validation"]["ok"])
        self.assertTrue(result["validation"]["event_continuity"]["ok"])
        self.assertEqual(result["unique_full_descriptors"], 3)
        self.assertEqual(result["repeated_descriptor_keys"], 1)
        repeated = result["repeated_miss_keys"][0]
        self.assertEqual(repeated["misses"], 2)
        self.assertEqual(result["unique_counts_ignoring_one_field"]["ignoring_remap"], 2)
        self.assertEqual(result["content_hashes"]["hashes_spanning_multiple_descriptors"], 1)
        groups = result["layout_content_groups"]
        self.assertIn("content_hash", groups["grouping_fields"])
        self.assertNotIn("location", groups["grouping_fields"])
        self.assertNotIn("offset", groups["grouping_fields"])
        self.assertEqual(groups["multi_source_group_count"], 0)
        self.assertEqual(result["windows"]["trace_rows_covered"], 4)
        self.assertEqual([b["unique_descriptors"] for b in result["windows"]["blocks"]], [2, 1])
        self.assertEqual(result["windows"]["blocks"][0]["layout_content_groups"]["group_count"], 2)
        self.assertTrue(result["windows"]["renderer_reconciliation_ok"])

    def test_overlap_coverage_and_renderer_mismatch_are_reported(self):
        windows = [
            {"index": 0, "start_qpc": 100, "end_qpc": 110},
            {"index": 1, "start_qpc": 105, "end_qpc": 115},
            {"index": 2, "start_qpc": 500, "end_qpc": 600},
        ]
        renderer = [{"qpc": 110, "frame": 5, "texture_decode_miss_calls": 4}]
        result = ANALYZER.analyze(self.rows, windows, renderer)
        self.assertEqual(result["windows"]["trace_rows_in_overlapping_windows"], 1)
        self.assertEqual(result["windows"]["trace_rows_outside_windows"], 2)
        self.assertEqual(result["windows"]["empty_trace_blocks"], 1)
        self.assertFalse(result["windows"]["blocks"][2]["renderer_reconciles"])
        self.assertFalse(result["windows"]["renderer_reconciliation_ok"])
        self.assertFalse(result["validation"]["ok"])

    def test_boundary_event_time_is_separate_from_present_frame_reconciliation(self):
        rows = [
            trace_row(0, 99, 20, frame=9),
            trace_row(1, 102, 21, frame=9),
            trace_row(2, 111, 22, frame=10),
        ]
        windows = [{"index": 0, "start_qpc": 100, "end_qpc": 110}]
        renderer = [{"qpc": 110, "frame": 10, "texture_decode_miss_calls": 2}]
        result = ANALYZER.analyze(rows, windows, renderer)
        block = result["windows"]["blocks"][0]
        self.assertEqual(block["event_time_miss_rows"], 1)
        self.assertEqual(block["frame_matched_trace_miss_rows"], 2)
        self.assertEqual(block["renderer_frames"], {"count": 1, "first": 10, "last": 10})
        self.assertEqual(block["frame_matched_rows"], 1)
        self.assertEqual(block["frame_mismatch_count"], 0)
        self.assertEqual(block["frame_mismatch_examples"], [])
        self.assertTrue(block["renderer_reconciles"])

    def test_layout_content_groups_count_candidate_relocations(self):
        rows = [
            trace_row(0, 100, 10, frame=2, location=1, offset=20, content_hash=55),
            trace_row(1, 110, 11, frame=2, location=2, offset=40, content_hash=55),
        ]
        result = ANALYZER.analyze(
            rows, [{"index": 0, "start_qpc": 100, "end_qpc": 110}]
        )
        groups = result["layout_content_groups"]
        self.assertEqual(groups["group_count"], 1)
        self.assertEqual(groups["multi_source_group_count"], 1)
        self.assertEqual(groups["top_repeated_groups"][0]["distinct_sources"], 2)
        self.assertIn("not an exact-byte comparison", groups["caveat"])
        self.assertEqual(
            result["windows"]["blocks"][0]["layout_content_groups"]["multi_source_group_count"], 1
        )

    def test_event_gap_qpc_decrease_and_serial_decrease_are_detected(self):
        rows = [dict(row) for row in self.rows]
        rows[2]["event"] = 8
        rows[2]["qpc"] = 90
        rows[2]["use_serial"] = 7
        result = ANALYZER.analyze(rows)
        self.assertFalse(result["validation"]["ok"])
        self.assertFalse(result["validation"]["event_continuity"]["ok"])
        self.assertFalse(result["validation"]["qpc_monotonic"]["ok"])
        self.assertFalse(result["validation"]["use_serial_monotonic"]["ok"])

    def test_trace_parser_rejects_missing_columns_and_truncated_rows(self):
        path = self.root / "bad.csv"
        self.write_csv(path, ["event", "qpc"], [{"event": 1, "qpc": 2}])
        with self.assertRaises(ANALYZER.TraceError):
            ANALYZER.read_trace(path)

        path.write_text(",".join(ANALYZER.TRACE_FIELDS) + "\n1,2\n", encoding="utf-8")
        with self.assertRaises(ANALYZER.TraceError):
            ANALYZER.read_trace(path)

        bad = trace_row(1, 2, 3, evicted=2)
        self.write_csv(path, ANALYZER.TRACE_FIELDS, [bad])
        with self.assertRaises(ANALYZER.TraceError):
            ANALYZER.read_trace(path)

    def test_cli_writes_json_and_uses_119_column_renderer(self):
        trace = self.root / "misses.csv"
        self.write_csv(trace, ANALYZER.TRACE_FIELDS, self.rows)
        windows = self.root / "windows.json"
        windows.write_text(json.dumps({"blocks": [
            {"start_qpc": 100, "end_qpc": 120},
            {"start_qpc": 121, "end_qpc": 140},
        ]}), encoding="utf-8")
        renderer = self.root / "renderer.csv"
        fields = [f"column_{i}" for i in range(119)]
        fields[0], fields[1], fields[2] = "qpc", "texture_decode_miss_calls", "frame"
        renderer_rows = []
        for qpc, frame, misses in [(115, 5, 3), (130, 6, 1)]:
            row = {field: "0" for field in fields}
            row.update(qpc=str(qpc), frame=str(frame), texture_decode_miss_calls=str(misses))
            renderer_rows.append(row)
        self.write_csv(renderer, fields, [
            *renderer_rows,
        ])
        output = self.root / "analysis.json"
        status = ANALYZER.main([
            str(trace), "--windows", str(windows), "--renderer119colCSV", str(renderer),
            "--output", str(output),
        ])
        self.assertEqual(status, 0)
        saved = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(saved["rows"], 4)
        self.assertTrue(saved["validation"]["ok"])

    def test_renderer_parser_rejects_any_truncated_cell_and_negative_value(self):
        path = self.root / "renderer.csv"
        fields = [f"column_{i}" for i in range(119)]
        fields[0], fields[1], fields[2] = "qpc", "frame", "texture_decode_miss_calls"
        row = {field: "0" for field in fields}
        row.update(qpc="100", frame="5", texture_decode_miss_calls="1")
        self.write_csv(path, fields, [row])
        self.assertEqual(len(ANALYZER.read_renderer(path)), 1)
        row[fields[-1]] = ""
        self.write_csv(path, fields, [row])
        with self.assertRaises(ANALYZER.TraceError):
            ANALYZER.read_renderer(path)
        row[fields[-1]] = "-2"
        self.write_csv(path, fields, [row])
        with self.assertRaises(ANALYZER.TraceError):
            ANALYZER.read_renderer(path)


if __name__ == "__main__":
    unittest.main()
