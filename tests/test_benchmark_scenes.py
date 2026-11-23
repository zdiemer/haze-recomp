"""Benchmark scene archives hold only checkpoint save state, never game assets."""
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from diagnostics import run_timedemo


class BenchmarkScenes(unittest.TestCase):
    def test_archives_contain_only_save_state(self):
        scenes = run_timedemo.scenes()
        self.assertIn('later', scenes)
        for scene in scenes:
            with self.subTest(scene=scene), tarfile.open(run_timedemo.SCENES / f'{scene}.tar.xz') as archive:
                members = archive.getmembers()
                self.assertEqual(sorted(m.name for m in members),
                                 sorted(f'{run_timedemo.PROFILE}/{name}' for name in run_timedemo.STATE_FILES))
                for member in members:
                    self.assertTrue(member.isfile())
                    self.assertLessEqual(member.size, 1 << 20)
                    self.assertEqual(member.mtime, 0)
                self.assertIn(scene, run_timedemo.SCENE_NOTES)

    def test_unpacked_save_takes_icons_from_the_disc(self):
        with tempfile.TemporaryDirectory() as directory:
            disc = Path(directory) / 'disc'
            (disc / 'PS3_GAME').mkdir(parents=True)
            for name in run_timedemo.DISC_FILES:
                (disc / 'PS3_GAME' / name).write_bytes(name.encode())
            run_timedemo.unpack_scene('later', disc, Path(directory) / 'savedata')
            profile = Path(directory) / 'savedata' / run_timedemo.PROFILE
            self.assertEqual(sorted(p.name for p in profile.iterdir()),
                             sorted(run_timedemo.STATE_FILES + run_timedemo.DISC_FILES))
            self.assertEqual((profile / 'SND0.AT3').read_bytes(), b'SND0.AT3')

    def test_input_unbinds_sticks_and_skips_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'input.ini'
            source.write_text('MoveX=leftx\nMoveY=lefty\nLookX=rightx\nLookY=righty\n'
                              'AutoSkipLogos=0\nAutoSkipAttract=0\n')
            text = run_timedemo.benchmark_input(source, True)
            self.assertNotIn('leftx', text); self.assertNotIn('righty', text)
            self.assertIn('AutoSkipLogos=1', text); self.assertIn('AutoSkipAttract=1', text)
            self.assertIn('AutoSkipLogos=0', run_timedemo.benchmark_input(source, False))
        text = run_timedemo.benchmark_input(ROOT / 'config' / 'input.ini', True)
        for axis in ('MoveX', 'MoveY', 'LookX', 'LookY'):
            self.assertIn(f'{axis}=None', text)

    def test_unknown_scene_is_rejected(self):
        with self.assertRaises(SystemExit):
            run_timedemo.parse_args(['stem', '--scene', '../later'])


if __name__ == '__main__':
    unittest.main()
