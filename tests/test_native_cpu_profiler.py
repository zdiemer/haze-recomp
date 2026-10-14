"""Live optimized-call-chain oracle for the optional Windows stack sampler."""
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
fixture = root / 'local' / 'profiler-stack-oracle' / 'HazeRecomp.exe'
timeline_path = fixture.parent / 'oracle-timeline.json'
child = subprocess.Popen([str(fixture), '8'], stdout=subprocess.PIPE, text=True)
try:
    tid = int(child.stdout.readline())
    data = json.loads(subprocess.check_output([
        sys.executable, str(root / 'scripts/diagnose.py'), 'native-cpu',
        str(child.pid), '3', '--tid', str(tid), '--symbol-path', str(fixture.parent),
        '--stack-depth', '12', '--stack-every', '1',
        '--timeline-output', str(timeline_path), '--timeline-limit', '16'], text=True))
    assert len(data['threads']) == 1 and data['threads'][0]['tid'] == tid
    found = 0
    for stack in data['threads'][0]['stacks']:
        names = [frame.split('+0x')[0] for frame in stack['frames']]
        if names[:3] == ['oracle_leaf', 'oracle_middle', 'oracle_entry']:
            found += stack['samples']
    assert found >= 10, ('Too few correctly unwound optimized frames', data)
    assert data['suspension']['count'] > 0
    assert data['threads'][0]['cpu_ms'] > 100
    timeline = json.loads(timeline_path.read_text())
    assert timeline['pid'] == child.pid and timeline['qpc_frequency'] > 0
    assert len(timeline['samples']) == 16 and timeline['overwritten'] > 0
    assert timeline['total_samples'] == data['suspension']['count']
    assert timeline['total_samples'] == timeline['overwritten'] + 16
    previous = timeline['begin_qpc']
    previous_sequence = timeline['overwritten']
    for sample in timeline['samples']:
        assert sample['tid'] == tid and sample['created_filetime'] > 0
        assert previous <= sample['begin_qpc'] <= sample['context_qpc'] <= sample['resumed_qpc'] <= timeline['end_qpc']
        assert sample['sequence'] == previous_sequence + 1
        assert sample['context_valid'] and sample['stack_attempted']
        assert sample['thread_cpu_100ns_before_suspend'] >= 0
        previous = sample['resumed_qpc']
        previous_sequence = sample['sequence']
    assert any([frame.split('+0x')[0] for frame in s['frames']][:3] ==
               ['oracle_leaf', 'oracle_middle', 'oracle_entry'] for s in timeline['samples'])
    assert child.wait(timeout=15) == 0
    print(f'Native profiler oracle passed: {found} leaf/middle/entry stacks; '
          f'max suspension {data["suspension"]["max_ms"]:.3f} ms')
finally:
    if child.poll() is None:
        # This handle is the bounded test fixture launched above, never a game.
        child.terminate()
        child.wait(timeout=5)

# Progress-only fixture must not be suspended. The stalled fixture deliberately
# parks inside oracle_stall; assert that every selected context lies in a known
# stall and sampling stops at the per-episode budget.
for mode, seconds in (('progress', 4), ('stall', 5)):
    child = subprocess.Popen([str(fixture), str(seconds), mode], stdout=subprocess.PIPE, text=True)
    try:
        tid = int(child.stdout.readline())
        triggered_path = fixture.parent / f'oracle-{mode}-timeline.json'
        data = json.loads(subprocess.check_output([
            sys.executable, str(root / 'scripts/diagnose.py'), 'native-cpu',
            str(child.pid), str(seconds-1), '--tid', str(tid),
            '--symbol-path', str(fixture.parent), '--stack-depth', '12',
            '--interval-ms', '5', '--stall-ms', '50', '--stall-samples', '2',
            '--timeline-output', str(triggered_path)], text=True))
        stdout, _ = child.communicate(timeout=10)
        assert child.returncode == 0
        timeline = json.loads(triggered_path.read_text())
        trigger = data['stall_trigger']
        assert trigger['polls'] > 10 and trigger['observed_progress'] > 10
        if mode == 'progress':
            assert trigger['episodes'] == data['suspension']['count'] == 0, data
            assert timeline['samples'] == []
        else:
            known = [tuple(map(int, line.split()[1:])) for line in stdout.splitlines() if line.startswith('STALL ')]
            assert trigger['episodes'] >= 2 and known, data
            assert 2 <= len(timeline['samples']) <= 2*trigger['episodes']
            for sample in timeline['samples']:
                evidence = sample['stall_trigger']
                assert any(lo <= sample['context_qpc'] <= hi for lo,hi in known)
                assert evidence['trigger_qpc'] - evidence['progress_observed_qpc'] >= trigger['threshold_ticks']
                assert evidence['sample_in_episode'] in (1,2)
                assert any(frame.split('+0x')[0]=='oracle_stall' for frame in sample['frames'])
        print(f'Trigger oracle {mode}: {trigger["episodes"]} episodes, '
              f'{data["suspension"]["count"]} suspensions, '
              f'{data["suspension"]["total_ms"]:.3f} ms total')
    finally:
        if child.poll() is None:
            child.terminate();child.wait(timeout=5)
