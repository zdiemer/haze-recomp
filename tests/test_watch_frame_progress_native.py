"""Native end-to-end checks for the read-only frame-progress watcher.

Uses the already-built profiler oracle, never the game executable or compiler.
"""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ORACLE = ROOT / "local" / "profiler-stack-oracle" / "HazeRecomp.exe"
WATCHER = ROOT / "scripts" / "watch_frame_progress.py"


@unittest.skipUnless(sys.platform == "win32", "native PDB observer requires Windows")
class NativeFrameProgressWatcherTests(unittest.TestCase):
    def setUp(self):
        if not ORACLE.is_file() or not ORACLE.with_suffix(".pdb").is_file():
            self.skipTest("prebuilt profiler oracle executable/PDB is unavailable")

    def run_observer(self, fixture_args: list[str], duration: float):
        with tempfile.TemporaryDirectory(prefix="haze-frame-watch-") as temp:
            output = Path(temp) / "watch.json"
            fixture = subprocess.Popen(
                [str(ORACLE), *fixture_args],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                watcher = subprocess.run(
                    [
                        sys.executable,
                        str(WATCHER),
                        str(fixture.pid),
                        "--exe", str(ORACLE),
                        "--symbol-path", str(ORACLE.parent),
                        "--seconds", str(duration),
                        "--minimum-seconds", "0",
                        "--stall-ms", "50",
                        "--poll-ms", "6",
                        "--post-progress-seconds", "0.1",
                        "--output", str(output),
                    ],
                    capture_output=True,
                    text=True,
                    timeout=duration + 5,
                )
                stdout, stderr = fixture.communicate(timeout=10)
            except Exception:
                if fixture.poll() is None:
                    fixture.kill()
                fixture.communicate()
                raise
            self.assertEqual(watcher.returncode, 0,
                             f"watcher failed\nstdout:\n{watcher.stdout}\nstderr:\n{watcher.stderr}")
            self.assertEqual(fixture.returncode, 0,
                             f"oracle failed\nstdout:\n{stdout}\nstderr:\n{stderr}")
            self.assertTrue(output.is_file(), "watcher did not write its JSON result")
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(result["process"]["counter_symbol"], "g_frames_presented")
            self.assertEqual(result["process"]["counter_size"], 4)
            self.assertEqual(Path(result["process"]["image_path"]).resolve(), ORACLE.resolve())
            return result, stdout

    def test_progress_only_finishes_at_hard_bound_without_trigger(self):
        result, stdout = self.run_observer(["4"], duration=2)
        # The observer duration is 2 s while the fixture runs 4 s; because the
        # watcher requires actual progress, it should simply time out unfinished.
        self.assertEqual(result["completion_reason"], "timeout/unfinished")
        self.assertIsNone(result["trigger"])
        self.assertNotIn("STALL ", stdout)
        self.assertGreater(result["trigger_stats"]["watch_polls"], 100)

    def test_trigger_qpc_lands_inside_fixture_stall_and_recovery_afterward(self):
        result, stdout = self.run_observer(["4", "stall"], duration=3)
        self.assertEqual(result["completion_reason"], "triggered-and-recovered")
        stall_lines = [line for line in stdout.splitlines() if line.startswith("STALL ")]
        self.assertTrue(stall_lines, f"fixture did not report its injected stall: {stdout!r}")
        _, begin, end = stall_lines[0].split()
        stall_begin, stall_end = int(begin), int(end)
        trigger_qpc = int(result["trigger"]["trigger_qpc"])
        self.assertLessEqual(stall_begin, trigger_qpc)
        self.assertLessEqual(trigger_qpc, stall_end)
        self.assertGreater(int(result["recovered_qpc"]), stall_end)
        self.assertNotEqual(result["trigger"]["frame_counter"], result["recovered_counter"])


if __name__ == "__main__":
    unittest.main()
