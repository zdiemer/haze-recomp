"""Separate SPURS queue lock, sleep/native wait and remaining wall time.

Only scopes wholly contained in the requested frame interval contribute costs;
boundary-crossing calls are counted separately. Durations include scheduling
delays and are not CPU utilization. Unavailable means empty for pop/full for
push. Wait time cannot establish when the producer actually became ready.
"""
import argparse
import collections
import csv
import json
from pathlib import Path


def summarize(directory, renderer, first, last):
    with Path(renderer).open(newline='') as file:
        frames = {int(r['frame']): int(r['qpc']) for r in csv.DictReader(file)}
    begin, end = frames[first], frames[last]
    groups = collections.defaultdict(collections.Counter)
    crossing = 0
    for path in sorted(Path(directory).glob('queue-*.csv')):
        with path.open(newline='') as file:
            for row in csv.DictReader(file):
                try:
                    start, finish = int(row['begin_qpc']), int(row['end_qpc'])
                    if finish < begin or start > end:
                        continue
                    if start < begin or finish > end:
                        crossing += 1
                        continue
                    frequency = int(row['frequency'])
                    factor = 1000 / frequency
                    wall = (finish - start) * factor
                    lock = int(row['lock_ticks']) * factor
                    wait = int(row.get('wait_ticks', row.get('sleep_ticks'))) * factor
                    if wall < 0 or lock < 0 or wait < 0 or lock + wait > wall + 1e-6:
                        raise ValueError(f'Invalid duration partition in {path}')
                    key = (row['host_tid'], row['queue'], row['taskset'], row['push'], row.get('native_wait', '0'))
                    group = groups[key]
                    group['calls'] += 1
                    group['wall_ms'] += wall
                    group['lock_ms'] += lock
                    group['wait_ms'] += wait
                    group['remaining_ms'] += wall - lock - wait
                    group['max_call_ms'] = max(group['max_call_ms'], wall)
                    for name in ('attempts', 'busy', 'unavailable'):
                        group[name] += int(row[name])
                    group['errors'] += int(row['result'], 16) != 0
                except (KeyError, TypeError):
                    # A live buffered stream can end with a partial final row.
                    continue
    output = []
    for key, costs in sorted(groups.items(), key=lambda item: item[1]['wall_ms'], reverse=True):
        observations = costs['busy'] + costs['unavailable']
        # Nonblocking calls do not sleep; their counts must not enter this mean.
        # The severe-wait analysis selects blocking calls, but report this as a
        # duration per unavailable/claimed observation rather than per sleep.
        output.append(dict(host_tid=int(key[0]), queue=key[1], taskset=key[2],
            operation='push' if int(key[3]) else 'pop', native_wait=int(key[4]), **costs,
            wait_ms_per_observation=costs['wait_ms'] / observations if observations else None))
    return dict(first_frame=first, last_frame=last, boundary_crossing_calls=crossing,
                groups=output, note=__doc__)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('renderer', type=Path)
    parser.add_argument('--first', type=int, required=True)
    parser.add_argument('--last', type=int, required=True)
    args = parser.parse_args()
    if args.last <= args.first:
        parser.error('last must exceed first')
    print(json.dumps(summarize(args.directory, args.renderer, args.first, args.last), indent=2))
