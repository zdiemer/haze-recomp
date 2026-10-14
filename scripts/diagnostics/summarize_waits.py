"""Summarize opt-in PPU_WAIT_PROFILE timelines within a renderer frame range.

Scopes include scheduling delays. HLE imports can contain syscalls; their totals
overlap. The union per host thread counts nested scopes once. Calls below 200 us
are omitted by the recorder, so this is a lower bound on service-call wall time.
"""
import argparse
import collections
import csv
import json
from pathlib import Path
import re


def union_ticks(intervals):
    total = 0
    begin = end = None
    for a, b in sorted(intervals):
        if end is None or a > end:
            if end is not None:
                total += end - begin
            begin, end = a, b
        else:
            end = max(end, b)
    return total + (end - begin if end is not None else 0)


def load_names(root):
    names = {'hle': {}, 'syscall': {}}
    source = root / 'generated/ppu/ppu_hle_nids.cpp'
    if source.exists():
        names['hle'].update((nid.upper().zfill(8), name) for nid, name in re.findall(
            r'ps3_hle_register\(0x([0-9a-fA-F]+)[uU]?,\s*"([^"]+)"',
            source.read_text()))
    source = root / 'external/ps3recomp/runtime/syscalls/lv2_syscall_table.h'
    if source.exists():
        for name, number in re.findall(r'^#define\s+(SYS_\w+)\s+(0x[0-9a-fA-F]+|\d+)\b',
                                      source.read_text(), re.M):
            names['syscall'][f'{int(number, 0):08X}'] = name.lower()
    return names


def summarize(directory, renderer, first, last, guest_tid=None):
    frames = {}
    with Path(renderer).open(newline="") as f:
        for row in csv.DictReader(f):
            try:
                frames[int(row['frame'])] = int(row['qpc'])
            except (KeyError, ValueError, TypeError):
                continue
    begin = frames[first]
    end = frames[last]
    root = Path(__file__).resolve().parents[2]
    names = load_names(root)
    groups = collections.defaultdict(list)
    intervals = collections.defaultdict(list)
    threads = {}
    frequency = None
    for path in Path(directory).glob('wait-*.csv'):
        with path.open(newline='') as f:
            for row in csv.DictReader(f):
                try:
                    a, b = int(row['begin_qpc']), int(row['end_qpc'])
                    tid, host = int(row['guest_tid']), int(row['host_tid'])
                    freq = int(row['frequency'])
                    stack = tuple(row.get(f'stack_lr{n}', '00000000') for n in range(3))
                    key = (tid, row['kind'], row['id'], row['lr'], row['arg0'], stack)
                except (ValueError, TypeError, KeyError):
                    continue
                if b <= begin or a >= end or (guest_tid is not None and tid != guest_tid):
                    continue
                a, b = max(a, begin), min(b, end)
                frequency = freq
                groups[key].append((b-a)*1000/freq)
                intervals[host].append((a,b))
                threads[host] = tid
    if not frequency:
        raise ValueError('No recorded calls overlap the requested frames')
    rows = [dict(guest_tid=k[0], kind=k[1], id=k[2],
                 name=names.get(k[1], {}).get(k[2].upper(), k[2]), lr=k[3], arg0=k[4],
                 saved_lr=k[5], calls=len(v), wall_ms=sum(v), max_ms=max(v))
            for k,v in groups.items()]
    return dict(first_frame=first, last_frame=last,
                elapsed_ms=(end-begin)*1000/frequency,
                threads=[dict(host_tid=h, guest_tid=threads[h],
                              service_union_ms=union_ticks(v)*1000/frequency)
                         for h,v in intervals.items()],
                groups=sorted(rows,key=lambda r:r['wall_ms'],reverse=True)[:40],
                note=__doc__)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('renderer', type=Path)
    p.add_argument('--first', type=int, required=True)
    p.add_argument('--last', type=int, required=True)
    p.add_argument('--guest-tid', type=int)
    a = p.parse_args()
    if a.last <= a.first:
        p.error('last must exceed first')
    print(json.dumps(summarize(a.directory,a.renderer,a.first,a.last,a.guest_tid),indent=2))
