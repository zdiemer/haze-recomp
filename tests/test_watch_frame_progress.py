from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from diagnostics.watch_frame_progress import FrameRecoveryWatcher


class FrameRecoveryWatcherTests(unittest.TestCase):
    def test_ignores_early_trigger_then_stops_after_recovery_window(self):
        watcher = FrameRecoveryWatcher(1000, 0, stall_ms=50,
                                      minimum_seconds=0.1,
                                      post_progress_seconds=0.04)
        self.assertIsNone(watcher.observe(10, 0))
        self.assertIsNone(watcher.observe(11, 20))  # real pre-minimum progress
        self.assertIsNone(watcher.observe(11, 99))
        self.assertIsNone(watcher.observe(11, 100))
        self.assertEqual(watcher.trigger["observed_qpc"], 100)
        self.assertEqual(watcher.trigger["frame_counter"], 11)
        self.assertIsNone(watcher.observe(11, 120))
        self.assertIsNone(watcher.observe(12, 130))
        self.assertEqual(watcher.recovered_qpc, 130)
        self.assertIsNone(watcher.observe(12, 169))
        self.assertEqual(watcher.observe(12, 170), "triggered-and-recovered")
        result = watcher.result(170, {"pid": 7})
        self.assertEqual(result["completion_reason"], "triggered-and-recovered")
        self.assertEqual(result["recovered_counter"], 12)
        self.assertEqual(result["trigger_stats"]["watch_polls"], 8)
        self.assertEqual(result["trigger_stats"]["max_poll_gap_ticks"], 79)

    def test_requires_real_progress_after_minimum_if_none_was_seen_before(self):
        watcher = FrameRecoveryWatcher(1000, 0, stall_ms=50,
                                      minimum_seconds=0.1,
                                      post_progress_seconds=0)
        for qpc in (0, 50, 99, 100):
            self.assertIsNone(watcher.observe(1, qpc))
        self.assertIsNone(watcher.observe(2, 120))
        self.assertIsNone(watcher.observe(2, 169))
        self.assertIsNone(watcher.observe(2, 170))
        self.assertEqual(watcher.trigger["observed_qpc"], 170)
        self.assertIsNone(watcher.recovered_qpc)
        self.assertIsNone(watcher.observe(3, 180))
        self.assertEqual(watcher.recovered_qpc, 180)
        self.assertEqual(watcher.observe(3, 181), "triggered-and-recovered")

    def test_timeout_and_unrecovered_trigger_are_unfinished(self):
        no_stall = FrameRecoveryWatcher(1000, 10, stall_ms=50,
                                       minimum_seconds=0.02,
                                       post_progress_seconds=5)
        no_stall.observe(1, 10)
        no_stall.observe(2, 20)
        no_stall.observe(3, 30)
        no_stall.finish_timeout(40)
        self.assertEqual(no_stall.result(40, {})["completion_reason"], "timeout/unfinished")

        held = FrameRecoveryWatcher(1000, 0, stall_ms=50,
                                    minimum_seconds=0,
                                    post_progress_seconds=1)
        held.observe(1, 0)
        held.observe(2, 1)
        held.observe(2, 52)
        self.assertIsNotNone(held.trigger)
        held.finish_timeout(100)
        self.assertIsNone(held.recovered_qpc)
        self.assertEqual(held.result(100, {})["completion_reason"], "timeout/unfinished")

    def test_counter_wrap_counts_as_progress_and_poll_gap_is_reported(self):
        watcher = FrameRecoveryWatcher(1000, 0, stall_ms=20,
                                      minimum_seconds=0,
                                      post_progress_seconds=0)
        watcher.observe(0xFFFFFFFF, 0)
        watcher.observe(0, 1)
        watcher.observe(0, 21)
        self.assertIsNotNone(watcher.trigger)
        watcher.observe(1, 25)
        watcher.observe(1, 26)
        self.assertEqual(watcher.completion_reason, "triggered-and-recovered")
        self.assertEqual(watcher.result(26, {})["trigger_stats"]["max_poll_gap_ticks"], 20)

    def test_rejects_bad_observation_order_and_counter_width(self):
        watcher = FrameRecoveryWatcher(1000, 5)
        with self.assertRaises(ValueError):
            watcher.observe(0x100000000, 6)
        watcher.observe(1, 6)
        with self.assertRaises(ValueError):
            watcher.observe(1, 5)


if __name__ == "__main__":
    unittest.main()
