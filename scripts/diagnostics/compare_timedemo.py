"""Compare frame-locked benchmark runs by gameplay frame.

Each run is a prefix PREFIX with PREFIX-timeline.csv (HAZE_BENCHMARK_TIMELINE)
and PREFIX-fingerprint.csv (RSX_FRAME_FINGERPRINT_CSV). Frame time is the
producer's boundary-to-boundary interval over the sweep. Workload agreement
compares each later run's per-frame draw counts with the first run's; runs are
not bit-identical, so treat a large draw difference as a different scene, not
as a faster or slower build. See docs/benchmark-clock.md.
"""
import argparse
import csv
import json
from pathlib import Path
import statistics


def load_run(prefix):
    prefix = Path(prefix)
    lines = Path(f'{prefix}-timeline.csv').read_text().splitlines()
    header = dict(item.split('=', 1) for item in lines[0].lstrip('# ').split())
    timeline = {}
    for row in csv.DictReader(lines[1:]):
        if int(row['gameplay']) >= 0:
            timeline[int(row['gameplay'])] = (int(row['qpc']), int(row['escaped_us']), int(row['flips']))
    prints = {}
    with open(f'{prefix}-fingerprint.csv', newline='') as stream:
        for row in csv.DictReader(stream):
            prints[int(row['flips'])] = (row['hash'], int(row['draws']), int(row['constants']))
    return dict(name=prefix.name, frequency=int(header['qpc_frequency']), settle=int(header['settle']),
                sweep=int(header['sweep']), timeline=timeline, prints=prints)


def frame_print(run, gameplay_frame, offset=0):
    # The boundary follows simulation and precedes render submission, so the
    # frame drawn after boundary g carries flip count flips(g)+1.
    entry = run['timeline'].get(gameplay_frame)
    return run['prints'].get(entry[2]+1+offset) if entry else None


def frame_times(run):
    timeline, frequency = run['timeline'], run['frequency']
    begin, end = run['settle'], run['settle']+run['sweep']
    return [(timeline[g+1][0]-timeline[g][0])*1000/frequency for g in range(begin, end) if g+1 in timeline]


def summarize(run):
    times = sorted(frame_times(run))
    if not times:
        raise ValueError(f"{run['name']}: no sweep frames in the timeline")
    pick = lambda q: times[min(len(times)-1, int(q*len(times)))]
    end = run['settle']+run['sweep']
    escaped = sorted(g for g, value in run['timeline'].items() if value[1] and g <= end)
    mean = statistics.fmean(times)
    return dict(run=run['name'], frames=len(times), mean_ms=round(mean, 3), p50_ms=round(pick(.5), 3),
                p95_ms=round(pick(.95), 3), p99_ms=round(pick(.99), 3), max_ms=round(times[-1], 3),
                fps=round(1000/mean, 2), escaped_frames=len(escaped), first_escape=escaped[0] if escaped else None)


def agreement(reference, run, window):
    end = min(reference['settle']+reference['sweep'], run['settle']+run['sweep'])
    pairs = lambda offset: [(g, frame_print(reference, g), frame_print(run, g, offset)) for g in range(end)]
    # Renderer lag can shift the flip mapping by one frame; pick the best fit.
    offset = max((-1, 0, 1), key=lambda o: sum(1 for _, a, b in pairs(o)[:200] if a and b and a[1:] == b[1:]))
    rows = [(g, a, b) for g, a, b in pairs(offset) if a and b]
    identical = next((g for g, a, b in rows if a[0] != b[0]), None)
    windows = []
    for begin in range(0, end, window):
        part = [(a, b) for g, a, b in rows if begin <= g < begin+window]
        if part:
            draws_a = statistics.fmean(a[1] for a, _ in part)
            draws_b = statistics.fmean(b[1] for _, b in part)
            windows.append(dict(begin=begin, frames=len(part), equal_draws=sum(a[1] == b[1] for a, b in part),
                                draws=round(draws_b, 1), reference_draws=round(draws_a, 1),
                                draw_difference_percent=round(100*(draws_b-draws_a)/draws_a, 2) if draws_a else None))
    return dict(run=run['name'], reference=reference['name'], flip_offset=offset,
                identical_until=identical, windows=windows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('runs', nargs='+', help='run prefixes; the first is the reference')
    parser.add_argument('--window', type=int, default=600, help='gameplay frames per agreement window')
    args = parser.parse_args()
    runs = [load_run(prefix) for prefix in args.runs]
    print(json.dumps(dict(summaries=[summarize(run) for run in runs],
                          agreement=[agreement(runs[0], run, args.window) for run in runs[1:]]), indent=2))


if __name__ == '__main__':
    main()
