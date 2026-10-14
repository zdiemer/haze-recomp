import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from summarize_spu_job_modes import summarize


def job(begin, execute, end, cycles=100, image=40):
    return (f'[spu-job-time] image={image} taskset=00ABCDEF task=0 '
            f'begin_qpc={begin} execute_qpc={execute} end_qpc={end} '
            f'setup_cycles=10 run_cycles={cycles}')


class JobModes(unittest.TestCase):
    def setUp(self):
        self.windows = dict(complete=True, mode_column='native_cursor', qpc_frequency=1000,
                            blocks=[dict(flags=0, start_qpc=100, end_qpc=200),
                                    dict(flags=1, start_qpc=300, end_qpc=400)])
        self.lines = ['[haze-native-cursor-mode] enabled=0 qpc=90',
                      '[haze-native-cursor-mode] enabled=1 qpc=290',
                      job(110, 120, 130), job(140, 145, 150, 300),
                      job(310, 320, 330, 50), job(340, 345, 350, 150)]

    def test_cycles_and_complete_job_boundaries(self):
        # Neither a job straddling a window nor an unrelated image contributes.
        result = summarize(self.lines + [job(99, 101, 120, 9999),
                           job(180, 190, 201, 9999), job(110, 120, 130, 9999, 42)],
                           self.windows, 40, min_jobs=2)
        a, b = result['blocks']
        self.assertEqual((a['jobs'], a['total_run_cycles'], a['mean_run_cycles']), (2, 400, 200))
        self.assertEqual((b['jobs'], b['total_run_cycles'], b['median_run_cycles']), (2, 200, 100))
        self.assertEqual(a['mean_run_wall_ms'], 7.5)

    def test_reject_missing_late_wrong_or_changing_modes(self):
        for lines in [self.lines[1:],
                      [line.replace('qpc=90', 'qpc=101') for line in self.lines],
                      [line.replace('enabled=1', 'enabled=0') for line in self.lines],
                      self.lines + ['[haze-native-cursor-mode] enabled=0 qpc=350']]:
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                summarize(lines, self.windows, 40, 2)

    def test_reject_incomplete_windows_or_counters(self):
        for key, value in [('complete', False), ('qpc_frequency', 0), ('mode_column', 'cpu_work')]:
            windows = copy.deepcopy(self.windows)
            windows[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                summarize(self.lines, windows, 40, 2)
        for extra in [job(125, 115, 130), job(110, 120, 130, 0),
                      '[spu-job-time] image=40 end_qpc=150']:
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                summarize(self.lines + [extra], self.windows, 40, 2)
        with self.assertRaises(ValueError):
            summarize(self.lines, self.windows, 40, 3)


if __name__ == '__main__':
    unittest.main()
