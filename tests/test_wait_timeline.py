"""Prevent numeric syscall/HLE confusion in wait-profile interpretation."""
import csv
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from diagnostics.summarize_waits import load_names, summarize
from diagnostics.summarize_spurs_queues import summarize as summarize_queues


class WaitTimelineTests(unittest.TestCase):
    def test_queue_cost_partition_and_interval_boundaries(self):
        scratch = Path(__file__).resolve().parents[1] / 'local'
        with tempfile.TemporaryDirectory(prefix='queue-cost-', dir=scratch) as directory:
            root = Path(directory)
            renderer = root / 'renderer.csv'
            renderer.write_text('frame,qpc\n1,100\n2,1100\n')
            fields = ['begin_qpc','end_qpc','frequency','host_tid','queue','taskset',
                      'push','native_wait','result','attempts','busy','unavailable','lock_ticks','wait_ticks']
            rows = [dict(begin_qpc=200,end_qpc=800,frequency=1000,host_tid=1,
                queue='00000100',taskset='00000900',push=0,native_wait=0,result='00000000',
                attempts=3,busy=0,unavailable=2,lock_ticks=40,wait_ticks=500)]
            rows += [dict(rows[0],native_wait=1), dict(rows[0],begin_qpc=50),
                     dict(rows[0],end_qpc=1200)]
            def run():
                with (root/'queue-1.csv').open('w',newline='') as file:
                    writer=csv.DictWriter(file,fieldnames=fields)
                    writer.writeheader(); writer.writerows(rows)
                return summarize_queues(root,renderer,1,2)
            result = run()
            self.assertEqual(result['boundary_crossing_calls'],2)
            self.assertEqual(len(result['groups']),2)
            for group in result['groups']:
                self.assertEqual(group['calls'],1)
                self.assertEqual(group['wall_ms'],600)
                self.assertEqual(group['lock_ms'],40)
                self.assertEqual(group['wait_ms'],500)
                self.assertEqual(group['remaining_ms'],60)
                self.assertEqual(group['wait_ms_per_observation'],250)
            rows[0]['lock_ticks']=700
            with self.assertRaises(ValueError): run()

    def test_syscall_numbers_come_from_runtime_table(self):
        names = load_names(Path(__file__).resolve().parents[1])
        self.assertEqual(names['syscall']['0000006B'], 'sys_cond_wait')
        self.assertEqual(names['syscall']['0000005C'], 'sys_semaphore_wait')
        self.assertEqual(names['syscall']['0000008D'], 'sys_timer_usleep')

    def test_kinds_are_separate_and_nested_wall_time_is_unioned(self):
        scratch = Path(__file__).resolve().parents[1] / 'local'
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='wait-timeline-', dir=scratch) as directory:
            root = Path(directory)
            renderer = root / 'renderer.csv'
            renderer.write_text('frame,qpc\n1,100\n2,1100\n')
            fields = ['begin_qpc', 'end_qpc', 'frequency', 'host_tid',
                      'guest_tid', 'kind', 'id', 'lr', 'arg0']
            with (root / 'wait-1.csv').open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                for kind, begin, end in [('hle', 50, 900), ('syscall', 200, 800)]:
                    writer.writerow(dict(begin_qpc=begin, end_qpc=end, frequency=1000,
                        host_tid=1, guest_tid=1, kind=kind, id='0000006B', lr='00896C10', arg0='7'))
            result = summarize(root, renderer, 1, 2)
            groups = {row['kind']: row for row in result['groups']}
            self.assertEqual(groups['syscall']['name'], 'sys_cond_wait')
            self.assertNotEqual(groups['hle']['name'], 'sys_cond_wait')
            self.assertEqual(groups['hle']['wall_ms'], 800)
            self.assertEqual(groups['syscall']['wall_ms'], 600)
            self.assertEqual(result['threads'][0]['service_union_ms'], 800)


if __name__ == '__main__':
    unittest.main()
