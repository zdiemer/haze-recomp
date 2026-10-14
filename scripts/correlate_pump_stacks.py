"""Associate diagnostic stack locations with pump wall intervals, not CPU costs."""
import argparse
import collections
import csv
import json
import math
from pathlib import Path


def correlate(timeline, pump_rows, limit=12):
    samples = timeline['samples']
    frequency = timeline['qpc_frequency']
    if frequency <= 0 or timeline['end_qpc'] <= timeline['begin_qpc']:
        raise ValueError('Invalid capture clock or interval')
    if timeline['total_samples'] != timeline['overwritten'] + len(samples):
        raise ValueError('Sample counts disagree')
    if timeline['overwritten'] < 0 or not samples and timeline['overwritten']:
        raise ValueError('Invalid retained sample suffix')
    triggered = timeline.get('stall_trigger') is not None
    if len({(s['tid'], s['created_filetime']) for s in samples}) != 1 and not (triggered and not samples):
        raise ValueError('Expected exactly one sampled thread lifetime')
    if triggered and (not timeline.get('selected_tids') or len(timeline['selected_tids']) != 1
                      or any(s['tid'] != timeline['selected_tids'][0] for s in samples)):
        raise ValueError('Triggered capture must identify one selected thread')
    previous = timeline['begin_qpc']
    sequence = timeline['overwritten']
    for sample in samples:
        if not previous <= sample['begin_qpc'] <= sample['context_qpc'] <= sample['resumed_qpc'] <= timeline['end_qpc']:
            raise ValueError('Invalid sample interval or ordering')
        if sample['sequence'] != sequence + 1:
            raise ValueError('Missing or reordered sample sequence')
        previous, sequence = sample['resumed_qpc'], sample['sequence']
    # Triggered captures intentionally have unsampled normal frames. Include
    # full captured pump intervals, including the frame that first triggered
    # sampling. If the bounded sample suffix overwrote evidence, discard that
    # prefix conservatively just as in continuous mode.
    lo = timeline['begin_qpc'] if triggered and not timeline['overwritten'] else samples[0]['begin_qpc']
    hi = timeline['end_qpc'] if triggered else samples[-1]['resumed_qpc']
    eligible = []
    previous_end = None
    for row in pump_rows:
        if int(row['qpc_frequency']) != frequency:
            raise ValueError('Pump/sample clock mismatch')
        begin, end = int(row['begin_qpc']), int(row['end_qpc'])
        if end < begin:
            raise ValueError('Reversed pump interval')
        if previous_end is not None and begin < previous_end:
            raise ValueError('Overlapping or reordered pump intervals')
        previous_end = end
        if lo <= begin < end <= hi:
            elapsed = (end-begin)*1000/frequency
            if not math.isfinite(float(row['dt_ms'])) or abs(elapsed-float(row['dt_ms'])) > 0.000002:
                raise ValueError('Pump QPC/wall mismatch')
            eligible.append(row)
    frames = []
    for row in sorted(eligible, key=lambda r: float(r['dt_ms']), reverse=True)[:limit]:
        begin, end = int(row['begin_qpc']), int(row['end_qpc'])
        selected = [s for s in samples if begin <= s['context_qpc'] < end and s['context_valid']]
        # For one selected thread these suspension intervals cannot overlap.
        suspension = sum(max(0, min(end,s['resumed_qpc'])-max(begin,s['begin_qpc'])) for s in samples)
        stacks = collections.Counter(tuple(s['frames']) for s in selected if s['stack_attempted'] and s['frames'])
        frames.append(dict(pump=row, context_samples=len(selected),
            observed_suspend_brackets_ms=suspension*1000/frequency,
            sampled_locations=[dict(frames=list(chain), samples=count) for chain,count in stacks.most_common()],
            rip_locations=collections.Counter(s['rip_symbol'] for s in selected).most_common()))
    return dict(note='Diagnostic only: sampling perturbs these intervals. Locations include waits, '
                     'not CPU percentages or exact causal attribution. Unsampled gaps remain. '
                     'Suspension brackets include context/unwind and surrounding sampler work.',
                pid=timeline['pid'], exe=timeline['exe'],
                tid=timeline['selected_tids'][0] if triggered else samples[0]['tid'],
                stall_trigger=timeline.get('stall_trigger'),
                stack_evidence_status='samples present' if samples else 'no triggered samples',
                qpc_frequency=frequency, eligible_pump_intervals=len(eligible),
                retained_samples=len(samples), overwritten=timeline['overwritten'],
                max_intersample_gap_ms=max((b['context_qpc']-a['context_qpc'] for a,b in zip(samples,samples[1:])),default=0)*1000/frequency,
                longest_pump_intervals=frames)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('timeline', type=Path)
    parser.add_argument('pump', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--limit', type=int, default=12)
    args = parser.parse_args()
    if args.limit < 1:
        parser.error('limit must be positive')
    timeline = json.loads(args.timeline.read_text(encoding='utf-8'))
    with args.pump.open(newline='') as stream:
        result = correlate(timeline, list(csv.DictReader(stream)), args.limit)
    args.output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
