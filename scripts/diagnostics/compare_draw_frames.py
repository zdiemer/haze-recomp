"""Compare RSX_DRAW_CSV submissions in two captured guest frames.

Groups describe a pipeline, render target, topology and vertex count. They
are not model IDs: one model can use several groups and several models can
share a group. Executed draws do not establish visible rasterized pixels.
Capture with RSX_DRAW_CSV_GATE and avoid its partial first/last frames.
In the normal live 3D path, requested_N.ppm contains draw-frame N. Readback
precedes presentation; its successful-present record uses guest_frame N+1.
"""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path


GROUP_FIELDS = (
    'surf', 'prim', 'source_verts', 'live_pso_key',
    'fp_location', 'fp_offset', 'fp_size',
)


def compare(path, frame_a, frame_b):
    frames = {frame: dict(outcomes=Counter(), depth=Counter(), groups=Counter())
              for frame in (frame_a, frame_b)}
    with path.open(newline='', encoding='utf-8-sig') as source:
        # Blits and surface events share the file as # comments, with their
        # own schemas. They must not be counted as draw outcomes.
        reader = csv.DictReader(line for line in source if not line.startswith('#'))
        required = {'frame', 'outcome', 'dfunc', *GROUP_FIELDS}
        missing = required - set(reader.fieldnames or ())
        if missing:
            raise ValueError('Missing draw columns: ' + ', '.join(sorted(missing)))
        for row in reader:
            frame = int(row['frame'])
            if frame not in frames:
                continue
            sample = frames[frame]
            sample['outcomes'][row['outcome']] += 1
            if row['outcome'] == 'execute':
                sample['depth'][row['dfunc']] += 1
                sample['groups'][tuple(row[field] for field in GROUP_FIELDS)] += 1
    absent = [frame for frame, sample in frames.items() if not sample['outcomes']]
    if absent:
        raise ValueError('No recorded draws for frame(s): ' + ', '.join(map(str, absent)))
    a, b = frames[frame_a]['groups'], frames[frame_b]['groups']
    changes = [dict(group=dict(zip(GROUP_FIELDS, key)),
                    count_a=a[key], count_b=b[key])
               for key in sorted(a.keys() | b.keys()) if a[key] != b[key]]
    changes.sort(key=lambda change: -abs(change['count_b'] - change['count_a']))
    return dict(
        source=str(path.resolve()), frame_a=frame_a, frame_b=frame_b,
        frames={str(frame): dict(outcomes=dict(sample['outcomes']),
                                depth_functions=dict(sample['depth']),
                                executed_groups=len(sample['groups']))
                for frame, sample in frames.items()},
        changed_groups=changes,
        note='Frame arguments are draw CSV counters. Normal live 3D capture N '
             'contains draw-frame N and precedes present record N+1. '
             'Submission groups are not model IDs or '
             'proof of visible pixels; gate boundary frames may be partial.',
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--frames', nargs=2, required=True, type=int, metavar=('A', 'B'),
                        help='draw CSV counters; normal live 3D capture N uses N')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if min(args.frames) < 0 or args.frames[0] == args.frames[1]:
        parser.error('choose two distinct nonnegative guest frame numbers')
    try:
        result = compare(args.csv, *args.frames)
    except (OSError, ValueError, csv.Error) as error:
        parser.error(str(error))
    encoded = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(encoded, encoding='utf-8')
        print(json.dumps(dict(output=str(args.output),
                              frames=result['frames'],
                              changed_groups=len(result['changed_groups']))))
    else:
        print(encoded, end='')


if __name__ == '__main__':
    main()
