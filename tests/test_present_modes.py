"""Reject invalid benchmark windows while retaining every stall in valid ones."""
import contextlib
import copy
import csv
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from benchmark_present_modes import report, verify_process_observation


class PresentWindows(unittest.TestCase):
    def test_process_isolation_coverage_and_contention(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'local') as folder:
            log = Path(folder) / 'processes.jsonl'
            exe = str(ROOT / 'local/HazeRecomp.exe')
            windows = dict(pid=123, exe=exe, qpc_frequency=1000,
                           blocks=[dict(start_qpc=1000, end_qpc=7000)])
            good = [dict(qpc=qpc, qpc_frequency=1000,
                         processes=[dict(ProcessId=123, ExecutablePath=exe)])
                    for qpc in range(0, 8001, 2000)]
            def run(rows):
                # PowerShell Add-Content UTF8 can emit a BOM.
                log.write_text('\n'.join(json.dumps(row) for row in rows), encoding='utf-8-sig')
                return verify_process_observation(log, windows, 4)
            self.assertEqual(run(good)['blocks'], [dict(samples=5, max_gap_s=2)])
            invalid = [[], good[1:], good[:-1], [good[0], good[-1]],
                       [good[0], good[0], *good[1:]]]
            for change in ('extra', 'missing', 'pid', 'path', 'frequency'):
                rows = copy.deepcopy(good)
                if change == 'extra':
                    rows[2]['processes'].append(dict(ProcessId=456, ExecutablePath=exe))
                elif change == 'missing': rows[2]['processes'] = []
                elif change == 'pid': rows[2]['processes'][0]['ProcessId'] = 456
                elif change == 'path': rows[2]['processes'][0]['ExecutablePath'] = None
                else: rows[2]['qpc_frequency'] = 2000
                invalid.append(rows)
            for rows in invalid:
                with self.subTest(rows=rows), self.assertRaises(ValueError): run(rows)

    def test_native_queue_mode_evidence(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'local') as folder:
            root = Path(folder)
            args = SimpleNamespace(csv=root/'p.csv', windows=root/'w.json',
                output=root/'r.json', min_frames=60, queue_mode_log=root/'native.log',
                queue_wait_tid=[37, 1])
            args.windows.write_text(json.dumps(dict(complete=True, qpc_frequency=1000,
                mode_column='spurs_queue_wait', blocks=[dict(flags=1,start_qpc=100,end_qpc=3000)])))
            with args.csv.open('w', newline='') as file:
                file.write('# qpc_frequency=1000\npresent_id,guest_frame,qpc\n')
                for frame in range(101): file.write(f'{frame+1},{frame},{frame*40}\n')
            good = ('[spurs-queue-wait-mode] tid=37 native=1 qpc=80\n'
                    '[spurs-queue-wait-mode] tid=1 native=1 qpc=90\n')
            def run(log):
                args.queue_mode_log.write_text(log)
                with contextlib.redirect_stdout(io.StringIO()): report(args)
            run(good)
            self.assertGreaterEqual(json.loads(args.output.read_text())['blocks'][0]['frames'],60)
            for invalid in ['', good.replace('tid=37','tid=38'),
                good.replace('qpc=80','qpc=101'), good.replace('native=1','native=0'),
                good+'[spurs-queue-wait-mode] tid=37 native=0 qpc=150\n'
                     '[spurs-queue-wait-mode] tid=37 native=1 qpc=160\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)
            # The independent persistent-SPU wait needs its own mode events;
            # a PPU queue event is insufficient evidence even with matching TIDs.
            windows = json.loads(args.windows.read_text())
            windows['mode_column'] = 'spu_idle_wait'
            args.windows.write_text(json.dumps(windows))
            with self.assertRaises(ValueError): run(good)
            idle_good = good.replace('spurs-queue-wait-mode', 'haze-spu-idle-mode').replace('native=', 'enabled=')
            run(idle_good)
            for invalid in [idle_good.replace('tid=37', 'tid=38'),
                idle_good.replace('qpc=80', 'qpc=101'),
                idle_good+'[haze-spu-idle-mode] tid=37 enabled=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)
            windows['mode_column'] = 'frame_pacer'
            args.windows.write_text(json.dumps(windows))
            pacer_good = idle_good.replace('haze-spu-idle-mode', 'haze-frame-pacer-mode')
            run(pacer_good)
            for invalid in [idle_good, pacer_good.replace('tid=37', 'tid=38'),
                pacer_good.replace('qpc=80', 'qpc=101'),
                pacer_good.replace('enabled=1', 'enabled=0'),
                pacer_good+'[haze-frame-pacer-mode] tid=37 enabled=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)
            windows['mode_column'] = 'fifo_recycle_wait'
            args.windows.write_text(json.dumps(windows))
            fifo_good = good.replace('spurs-queue-wait-mode', 'gcm-fifo-wait-mode')
            run(fifo_good)
            for invalid in [good, fifo_good.replace('tid=37', 'tid=38'),
                fifo_good.replace('qpc=80', 'qpc=101'),
                fifo_good.replace('native=1', 'native=0'),
                fifo_good+'[gcm-fifo-wait-mode] tid=37 native=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)
            windows['mode_column'] = 'usleep'
            sleep_good = good.replace('spurs-queue-wait-mode', 'usleep-mode').replace(
                ' native=', ' guest_tid=8 timer=')
            for flag in (0, 8):
                windows['blocks'][0]['flags'] = flag
                args.windows.write_text(json.dumps(windows))
                settled = sleep_good.replace('timer=1', f'timer={flag // 8}')
                run(settled)
                for invalid in [good, settled.replace('tid=37 ', 'tid=38 '),
                    settled.replace('qpc=80', 'qpc=101'),
                    settled.replace(f'timer={flag // 8}', f'timer={1 - flag // 8}'),
                    settled + f'[usleep-mode] tid=37 guest_tid=8 timer={1 - flag // 8} qpc=150\n']:
                    with self.subTest(flag=flag, log=invalid), self.assertRaises(ValueError): run(invalid)
            windows['mode_column'] = 'spu_triplet_copy'
            windows['blocks'][0]['flags'] = 1
            args.windows.write_text(json.dumps(windows))
            args.queue_wait_tid = []  # This selector is shared across job threads.
            global_good = '[haze-spu-triplet-mode] enabled=1 qpc=80\n'
            run(global_good)
            for invalid in ['', pacer_good, global_good.replace('qpc=80', 'qpc=101'),
                global_good.replace('enabled=1', 'enabled=0'),
                global_good+'[haze-spu-triplet-mode] enabled=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)
            windows['mode_column'] = 'native_cursor'
            args.windows.write_text(json.dumps(windows))
            cursor_good=global_good.replace('haze-spu-triplet-mode','haze-native-cursor-mode')
            run(cursor_good)
            for invalid in ['', global_good, cursor_good.replace('qpc=80','qpc=101'),
                cursor_good.replace('enabled=1','enabled=0'),
                cursor_good+'[haze-native-cursor-mode] enabled=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)
            windows['mode_column'] = 'spu_float_native'
            args.windows.write_text(json.dumps(windows))
            float_good=cursor_good.replace('haze-native-cursor-mode','spu-float-native-mode')
            run(float_good)
            for invalid in ['', cursor_good, float_good.replace('qpc=80','qpc=101'),
                float_good.replace('enabled=1','enabled=0'),
                float_good+'[spu-float-native-mode] enabled=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)

            windows['mode_column'] = 'job0_kernel'
            args.windows.write_text(json.dumps(windows))
            kernel_good = '[haze-job0-kernel-mode] enabled=1 qpc=80\n'
            run(kernel_good)
            for invalid in ['', float_good, kernel_good.replace('qpc=80', 'qpc=101'),
                kernel_good.replace('enabled=1', 'enabled=0'),
                kernel_good+'[haze-job0-kernel-mode] enabled=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)

            windows['mode_column'] = 'spu_worker_reuse'
            args.windows.write_text(json.dumps(windows))
            kernel_good = '[spu-worker-reuse-mode] enabled=1 qpc=80\n'
            run(kernel_good)
            for invalid in ['', float_good, kernel_good.replace('qpc=80', 'qpc=101'),
                kernel_good.replace('enabled=1', 'enabled=0'),
                kernel_good+'[spu-worker-reuse-mode] enabled=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)

            windows['mode_column'] = 'gpu_vertex_pull'
            args.windows.write_text(json.dumps(windows))
            pull_good=float_good.replace('spu-float-native-mode','gpu-vertex-pull-mode')
            run(pull_good)
            for invalid in ['', float_good, pull_good.replace('qpc=80','qpc=101'),
                pull_good.replace('enabled=1','enabled=0'),
                pull_good+'[gpu-vertex-pull-mode] enabled=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)
            windows['blocks'][0]['flags'] = 2
            args.windows.write_text(json.dumps(windows))
            occurrence_good = pull_good.replace('enabled=1','enabled=2')
            run(occurrence_good)
            for invalid in [pull_good, occurrence_good.replace('qpc=80','qpc=101'),
                occurrence_good+'[gpu-vertex-pull-mode] enabled=1 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError): run(invalid)
            windows['mode_column']='job_wait'
            windows['blocks'][0]['flags']=1
            args.windows.write_text(json.dumps(windows))
            job_good='[haze-job-wait-mode] enabled=1 qpc=80\n'
            run(job_good)
            for invalid in [pull_good,job_good.replace('qpc=80','qpc=101'),
                job_good.replace('enabled=1','enabled=0'),
                job_good+'[haze-job-wait-mode] enabled=0 qpc=150\n']:
                with self.subTest(log=invalid), self.assertRaises(ValueError):run(invalid)

            windows['mode_column']='vp_packets'
            args.windows.write_text(json.dumps(windows))
            vp_good='[haze-vp-packet-mode] enabled=1 qpc=80\n'
            run(vp_good)
            for invalid in [job_good,vp_good.replace('qpc=80','qpc=101'),
                vp_good.replace('enabled=1','enabled=0'),
                vp_good+'[haze-vp-packet-mode] enabled=0 qpc=150\n']:
                with self.subTest(log=invalid),self.assertRaises(ValueError):run(invalid)
            windows['mode_column']='vp_consumer'
            args.windows.write_text(json.dumps(windows))
            consumer_good='[rsx-vp-burst-mode] enabled=1 qpc=80\n'
            run(consumer_good)
            for invalid in [vp_good,consumer_good.replace('qpc=80','qpc=101'),
                consumer_good.replace('enabled=1','enabled=0'),
                consumer_good+'[rsx-vp-burst-mode] enabled=0 qpc=150\n']:
                with self.subTest(log=invalid),self.assertRaises(ValueError):run(invalid)

    def test_fifo_idle_wait_mode_covers_each_full_block(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'local') as folder:
            root = Path(folder)
            args = SimpleNamespace(csv=root/'p.csv', windows=root/'w.json',
                output=root/'r.json', min_frames=60, queue_mode_log=root/'native.log')
            windows = dict(complete=True, qpc_frequency=1000,
                mode_column='fifo_idle_wait', blocks=[
                    dict(flags=1, start_qpc=400, end_qpc=3000),
                    dict(flags=0, start_qpc=3200, end_qpc=5800)])
            args.windows.write_text(json.dumps(windows))
            with args.csv.open('w', newline='') as stream:
                stream.write('# qpc_frequency=1000\npresent_id,guest_frame,qpc\n')
                for frame in range(151):
                    stream.write(f'{frame+1},{frame},{frame*40}\n')

            good = ('[fifo-idle-wait-mode] timeout_ms=1 qpc=100\n'
                    '[fifo-idle-wait-mode] timeout_ms=0 qpc=3100\n')
            def run(log):
                args.queue_mode_log.write_text(log)
                with contextlib.redirect_stdout(io.StringIO()):
                    report(args)

            run(good)
            self.assertEqual([b['flags'] for b in json.loads(args.output.read_text())['blocks']], [1, 0])

            invalid = [
                '',  # No startup event covering the first block.
                good.replace('timeout_ms=1 qpc=100', 'timeout_ms=0 qpc=100'),
                good.replace('timeout_ms=0 qpc=3100', ''),  # Second block is uncovered.
                good.replace('qpc=3100', 'qpc=3000'),  # Change at the first block's end.
                good.replace('qpc=3100', 'qpc=4000'),  # Change inside the second block.
                good + '[fifo-idle-wait-mode] timeout_ms=1 qpc=4000\n',
            ]
            for log in invalid:
                with self.subTest(log=log), self.assertRaises(ValueError):
                    run(log)

    def test_modes_and_invalid_windows(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'local') as folder:
            root = Path(folder)
            args = SimpleNamespace(csv=root/'p.csv', windows=root/'w.json',
                                   output=root/'r.json', min_frames=60)
            for column, modes in [('cpu_work', (259,1283)), ('cpu_work',(1283,3331)), ('cpu_work',(1283,5379)), ('vertex_cache',(0,1)), ('geometry_cache',(0,1)), ('vertex_workers',(0,1)), ('vertex_workers',(1,3)), ('shadow_compare',(0,1)), ('depth_export',(0,1)), ('window_position',(0,2)), ('depth_clear',(0,1))]:
                with self.subTest(column=column):
                    rows, ticks = [], 0
                    for i in range(202):
                        ticks += 120 if i == 50 else 40
                        row = dict(present_id=i+1, guest_frame=i, qpc=ticks,
                                   cpu_work=259, shadow_compare=1, depth_export=1, window_position=1, depth_clear=1)
                        row[column] = modes[i>100]
                        rows.append(row)
                    windows = dict(complete=True, qpc_frequency=1000, mode_column=column,
                        blocks=[dict(flags=modes[0], start_qpc=rows[0]['qpc'], end_qpc=rows[100]['qpc']),
                                dict(flags=modes[1], start_qpc=rows[101]['qpc'], end_qpc=rows[201]['qpc'])])

                    def run():
                        args.windows.write_text(json.dumps(windows))
                        with args.csv.open('w',newline='') as stream:
                            stream.write('# qpc_frequency=1000\n')
                            writer=csv.DictWriter(stream,fieldnames=rows[0].keys())
                            writer.writeheader();writer.writerows(rows)
                        with contextlib.redirect_stdout(io.StringIO()):
                            report(args)

                    run()
                    blocks=json.loads(args.output.read_text())['blocks']
                    self.assertEqual([b['frames'] for b in blocks],[100,100])
                    self.assertEqual(blocks[0]['frame_ms']['max'],120)
                    self.assertEqual(blocks[0]['frames_over_50_ms'],1)
                    complete_rows = rows
                    # Enough frames must not hide a truncated ring or early close.
                    for truncated in (complete_rows[1:], complete_rows[:-1]):
                        rows = truncated
                        with self.assertRaisesRegex(ValueError, 'entire measured block'): run()
                    rows = complete_rows
                    rows[50][column]=modes[1]
                    with self.assertRaises(ValueError):run()
                    rows[50][column]=modes[0];rows[50]['present_id']=999
                    with self.assertRaises(ValueError):run()
                    rows[50]['present_id']=51;windows['complete']=False
                    with self.assertRaises(ValueError):run()


if __name__=='__main__':unittest.main()
