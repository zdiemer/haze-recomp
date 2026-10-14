"""Diagnostic completion-loop accounting; no claim of removable wait time.

Only complete, top-level loops inside each window are summed. Boundary loops
are reported separately, and nested rows are never double counted. Phase ticks
are elapsed time, including preemption and nested completion processing.
"""
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path

PHASES = ('predicate', 'delay', 'pending', 'service')


def read_rows(path):
    with Path(path).open(newline='') as f:
        rows = [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]
    if not rows:
        raise ValueError('No completed job loops')
    frequency = rows[0]['qpc_frequency']
    last_end = 0
    for r in rows:
        elapsed = r['end_qpc'] - r['begin_qpc']
        if frequency <= 0 or r['qpc_frequency'] != frequency or elapsed < 0:
            raise ValueError('Invalid timestamps/frequency')
        if r['end_qpc'] < last_end or any(v < 0 for v in r.values()):
            raise ValueError('Invalid row order/counts')
        if r['job'] >= 512 or sum(r[p+'_ticks'] for p in PHASES) > elapsed:
            raise ValueError('Invalid job/accounting')
        last_end = r['end_qpc']
    top = sorted((r for r in rows if not r['depth']), key=lambda r:r['begin_qpc'])
    if any(a['end_qpc'] > b['begin_qpc'] for a,b in zip(top,top[1:])):
        raise ValueError('Overlapping top-level loops')
    return rows


def summarize(rows, start, end):
    if end <= start:
        raise ValueError('Invalid window')
    if min(r['begin_qpc'] for r in rows)>start or max(r['end_qpc'] for r in rows)<end:
        raise ValueError('Recorded loops do not bracket the window')
    scale=1000/rows[0]['qpc_frequency']
    selected=[r for r in rows if not r['depth'] and start<=r['begin_qpc'] and r['end_qpc']<=end]
    crossing=[r for r in rows if not r['depth'] and r['begin_qpc']<end and r['end_qpc']>start
              and not (start<=r['begin_qpc'] and r['end_qpc']<=end)]
    def stats(group):
        elapsed=sum(r['end_qpc']-r['begin_qpc'] for r in group)*scale
        phases={p+'_ms':sum(r[p+'_ticks'] for r in group)*scale for p in PHASES}
        return dict(loops=len(group),elapsed_ms=elapsed,other_ms=elapsed-sum(phases.values()),
                    **phases,**{p+'_calls':sum(r[p+'_calls'] for r in group) for p in PHASES})
    callers=defaultdict(list)
    for r in selected:
        callers[f"{r['caller']:08X}"].append(r)
    return dict(start_qpc=start,end_qpc=end,complete=stats(selected),
                callers={k:stats(v) for k,v in callers.items()},
                boundary_loops=len(crossing),
                boundary_overlap_ms=sum(min(end,r['end_qpc'])-max(start,r['begin_qpc']) for r in crossing)*scale,
                slowest=[dict(job=r['job'],caller=f"{r['caller']:08X}",begin_qpc=r['begin_qpc'],
                              end_qpc=r['end_qpc'],**stats([r]))
                         for r in sorted(selected,key=lambda r:r['end_qpc']-r['begin_qpc'],reverse=True)[:10]])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv',type=Path)
    parser.add_argument('windows',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    rows=read_rows(args.csv)
    windows=json.loads(args.windows.read_text(encoding='utf-8-sig'))
    if not windows.get('complete') or windows['qpc_frequency']!=rows[0]['qpc_frequency']:
        raise ValueError('Incomplete window manifest or frequency mismatch')
    result=[summarize(rows,b['start_qpc'],b['end_qpc']) for b in windows['blocks']]
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps([r['complete'] for r in result],indent=2))
