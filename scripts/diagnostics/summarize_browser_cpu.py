"""Rank self samples and caller stacks in Chromium CPU profiles.

Accepts playtest-wasm.py cpu-*.json (Profiler.stop responses), raw .cpuprofile
files, or directories containing either. Uses only the Python standard library.
Counts are sampled observations, not elapsed time or inclusive CPU usage.
"""
import argparse
from collections import Counter
import json
from pathlib import Path


def summarize(data, top=12, stack_depth=12):
    profile = data.get('profile', data)
    nodes = {}
    for node in profile['nodes']:
        if node['id'] in nodes:
            raise ValueError(f"Duplicate node ID: {node['id']}")
        nodes[node['id']] = node
    parents = {}
    for node in nodes.values():
        for child in node.get('children', []):
            if child not in nodes:
                raise ValueError(f'Unknown child node: {child}')
            if child in parents:
                raise ValueError(f'Multiple parents for node: {child}')
            parents[child] = node['id']
    # Check the whole graph, including nodes outside the displayed top results.
    visited = set()
    for node_id in nodes:
        chain = set()
        current = node_id
        while current is not None and current not in visited:
            if current in chain:
                raise ValueError(f'Cycle at node: {current}')
            chain.add(current)
            current = parents.get(current)
        visited.update(chain)
    if 'samples' in profile:
        counts = Counter(profile['samples'])
        if counts.keys() - nodes.keys():
            raise ValueError('Samples reference unknown nodes')
        count_source = 'samples'
    else:
        counts = Counter({key: node.get('hitCount', 0) for key, node in nodes.items()})
        count_source = 'hitCount'
    if any(type(count) is not int or count < 0 for count in counts.values()):
        raise ValueError('Sample counts must be nonnegative integers')
    total = sum(counts.values())
    rows = []
    for node_id, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
        if not count or len(rows) >= top:
            break
        stack = []
        current = node_id
        while current is not None and len(stack) < stack_depth:
            node = nodes[current]
            stack.append(dict(node_id=current, **node['callFrame']))
            current = parents.get(current)
        rows.append(dict(node_id=node_id, self_samples=count,
                         percent=100 * count / total, stack=stack))
    return dict(total_samples=total, count_source=count_source, hottest=rows)


def positive(value):
    value = int(value)
    if value < 1:
        raise argparse.ArgumentTypeError('must be positive')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+', type=Path)
    parser.add_argument('--top', type=positive, default=12)
    parser.add_argument('--stack-depth', type=positive, default=12)
    parser.add_argument('--json', action='store_true', help='emit machine-readable results')
    args = parser.parse_args()
    paths = []
    for source in args.inputs:
        found = sorted({*source.glob('cpu-*.json'), *source.glob('*.cpuprofile')}) if source.is_dir() else [source]
        if not found:
            parser.error(f'No CPU profiles in {source}')
        paths.extend(path for path in found if path not in paths)
    results = []
    for path in paths:
        try:
            data = json.loads(path.read_text(encoding='utf-8-sig'))
            result = summarize(data, args.top, args.stack_depth)
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
            parser.error(f'{path}: {error}')
        results.append(dict(source=str(path), **result))
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for result in results:
            print(f"{result['source']}: {result['total_samples']} self samples ({result['count_source']})")
            for row in result['hottest']:
                names = ' <- '.join(frame.get('functionName') or '(anonymous)' for frame in row['stack'])
                print(f"  {row['self_samples']:8} {row['percent']:6.2f}% node {row['node_id']}: {names}")


if __name__ == '__main__':
    main()
