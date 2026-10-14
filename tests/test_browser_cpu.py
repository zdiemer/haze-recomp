"""Check sample attribution, profile formats, and corrupt caller graphs."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from diagnostics.summarize_browser_cpu import summarize


def profile():
    return {'nodes': [
        {'id': 1, 'callFrame': {'functionName': '(root)'}, 'children': [2, 3]},
        {'id': 2, 'callFrame': {'functionName': 'render'}, 'hitCount': 99},
        {'id': 3, 'callFrame': {'functionName': 'render'}, 'hitCount': 1},
    ], 'samples': [2, 3, 2]}


class BrowserCPU(unittest.TestCase):
    def test_samples_override_hit_counts_without_merging_names(self):
        result = summarize({'profile': profile()})
        self.assertEqual(result['total_samples'], 3)
        self.assertEqual([r['self_samples'] for r in result['hottest']], [2, 1])
        self.assertEqual([f['node_id'] for f in result['hottest'][0]['stack']], [2, 1])
        self.assertAlmostEqual(result['hottest'][0]['percent'], 200 / 3)

    def test_raw_hit_counts_and_limits(self):
        data = profile()
        del data['samples']
        result = summarize(data, top=1, stack_depth=1)
        self.assertEqual(result['count_source'], 'hitCount')
        self.assertEqual(result['total_samples'], 100)
        self.assertEqual(len(result['hottest']), 1)
        self.assertEqual(len(result['hottest'][0]['stack']), 1)

    def test_empty_samples_do_not_fall_back_to_hit_counts(self):
        data = profile()
        data['samples'] = []
        self.assertEqual(summarize(data)['hottest'], [])

    def test_invalid_graphs_and_samples(self):
        for kind in ('cycle', 'missing-child', 'missing-sample', 'duplicate', 'two-parents'):
            with self.subTest(kind=kind):
                data = profile()
                if kind == 'cycle':
                    data['nodes'][1]['children'] = [1]
                elif kind == 'missing-child':
                    data['nodes'][0]['children'].append(99)
                elif kind == 'missing-sample':
                    data['samples'].append(99)
                elif kind == 'duplicate':
                    data['nodes'].append(data['nodes'][1])
                else:
                    data['nodes'][1]['children'] = [3]
                with self.assertRaises(ValueError):
                    summarize(data)

    def test_directory_cli_and_bad_input(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            (folder / 'cpu-01.json').write_text(json.dumps({'profile': profile()}))
            (folder / 'worker.cpuprofile').write_text(json.dumps(profile()))
            (folder / 'unrelated.json').write_text('{}')
            command = [sys.executable, str(ROOT / 'scripts/diagnose.py'), 'browser-cpu', temp, '--json']
            run = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(len(json.loads(run.stdout)), 2)
            (folder / 'cpu-01.json').write_text('{')
            run = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('cpu-01.json', run.stderr)


if __name__ == '__main__':
    unittest.main()
