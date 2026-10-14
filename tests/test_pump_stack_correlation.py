import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from diagnostics.correlate_pump_stacks import correlate


def capture():
    samples = [dict(sequence=i+1, tid=7, created_filetime=11, begin_qpc=t,
                    context_qpc=t+1, resumed_qpc=t+3, context_valid=True,
                    stack_attempted=True, frames=['wait', 'draw'], rip_symbol='wait')
               for i,t in enumerate((10, 20, 30, 40))]
    return dict(pid=1, exe='fixture', qpc_frequency=1000, begin_qpc=0, end_qpc=50,
                total_samples=4, overwritten=0, samples=samples)


def pump(begin=15, end=35):
    return dict(frame=1, qpc_frequency=1000, begin_qpc=begin, end_qpc=end, dt_ms=end-begin)


class PumpStackTests(unittest.TestCase):
    def test_qpc_join_counts_wait_locations_without_cpu_claim(self):
        result=correlate(capture(), [pump()])
        row=result['longest_pump_intervals'][0]
        self.assertEqual(row['context_samples'], 2)
        self.assertEqual(row['observed_suspend_brackets_ms'], 6)
        self.assertEqual(row['sampled_locations'], [dict(frames=['wait','draw'], samples=2)])

    def test_ring_prefix_and_frame_boundaries_are_excluded(self):
        data=capture(); data['samples']=data['samples'][1:]; data['overwritten']=1
        self.assertEqual(correlate(data,[pump()])['eligible_pump_intervals'],0)
        row=correlate(capture(),[pump(22,32)])['longest_pump_intervals'][0]
        self.assertEqual(row['context_samples'],1)
        self.assertEqual(row['observed_suspend_brackets_ms'],3)

    def test_mismatches_rejected(self):
        for change in ({'sequence': 99}, {'tid': 8}, {'resumed_qpc': 99}):
            data=copy.deepcopy(capture()); data['samples'][1].update(change)
            with self.assertRaises(ValueError): correlate(data,[pump()])
        wrong=pump(); wrong['qpc_frequency']=100
        with self.assertRaisesRegex(ValueError,'clock'): correlate(capture(),[wrong])
        wrong=pump(); wrong['dt_ms']=0
        with self.assertRaisesRegex(ValueError,'wall'): correlate(capture(),[wrong])
        wrong=pump(); wrong['dt_ms']=float('nan')
        with self.assertRaisesRegex(ValueError,'wall'): correlate(capture(),[wrong])
        for rows in ([pump(), pump()], [pump(),pump(30,40)], [pump(30,40),pump()]):
            with self.assertRaisesRegex(ValueError,'Overlapping'): correlate(capture(),rows)

    def test_triggered_capture_includes_first_stalled_frame_and_empty_capture(self):
        data=capture();data.update(stall_trigger={'episodes':1},selected_tids=[7])
        row=correlate(data,[pump(0,45)])['longest_pump_intervals'][0]
        self.assertEqual(row['context_samples'],4)
        data.update(samples=[],total_samples=0)
        row=correlate(data,[pump(0,45)])['longest_pump_intervals'][0]
        self.assertEqual(row['context_samples'],0)
        self.assertEqual(row['observed_suspend_brackets_ms'],0)
        data['selected_tids']=[8,7]
        with self.assertRaisesRegex(ValueError,'selected thread'):correlate(data,[pump()])


if __name__=='__main__': unittest.main()
