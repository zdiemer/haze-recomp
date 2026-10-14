from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from frame_stall_trigger import FrameStallTrigger


class StallTriggerTests(unittest.TestCase):
    def test_requires_progress_then_caps_and_rearms(self):
        gate=FrameStallTrigger(1000,50,2)
        self.assertIsNone(gate.observe(8,0))
        self.assertIsNone(gate.observe(8,1000))
        self.assertIsNone(gate.observe(9,1010))
        self.assertIsNone(gate.observe(9,1059))
        self.assertEqual(gate.observe(9,1060)['sample_in_episode'],1)
        self.assertEqual(gate.observe(9,1070)['sample_in_episode'],2)
        self.assertIsNone(gate.observe(9,5000))
        self.assertIsNone(gate.observe(10,5010))
        self.assertEqual(gate.observe(10,5060)['episode'],2)

    def test_normal_frames_and_wrap_never_trigger(self):
        gate=FrameStallTrigger(1000,50,3)
        for index in range(100):
            self.assertIsNone(gate.observe((0xfffffffe+index)&0xffffffff,index*16))
        self.assertEqual(gate.summary()['episodes'],0)
        self.assertEqual(gate.summary()['observed_progress'],99)

    def test_rejects_bad_clock_and_records_poll_gaps(self):
        for threshold in (0,float('nan'),float('inf')):
            with self.assertRaises(ValueError): FrameStallTrigger(1000,threshold,3)
        gate=FrameStallTrigger(1000,50,3)
        gate.observe(0,10);gate.observe(1,25)
        self.assertEqual(gate.summary()['max_poll_gap_ticks'],15)
        with self.assertRaises(ValueError):gate.observe(1,24)


if __name__=='__main__':unittest.main()
