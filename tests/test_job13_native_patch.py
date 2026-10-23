import argparse
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from patches.patch_job13_native import (
    DIVIDE_ANCHOR, DIVIDE_INSERT, HEADER, INCLUDE, SQRT_ENTRY, SQRT_REFERENCE, WRAPPER, patch)

SOURCE = ROOT/'generated/spu/spu_0031_at_00ECEC00/spu_recomp.c'


class Job13NativePatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SOURCE.exists():
            raise unittest.SkipTest('Requires the locally generated BLUS30094 lift')
        cls.source = SOURCE.read_text()

    def test_only_audited_boundaries_change(self):
        patched = patch(self.source)
        self.assertEqual(patched.count('haze_job13_native_divide_prefix(ctx)'), 1)
        self.assertEqual(patched.count(SQRT_REFERENCE), 1)
        self.assertEqual(patched.count(SQRT_ENTRY), 1)
        self.assertTrue(patched.endswith(WRAPPER))
        restored = patched[:-len(WRAPPER)].replace(SQRT_REFERENCE, SQRT_ENTRY, 1).replace(
            DIVIDE_INSERT + DIVIDE_ANCHOR, DIVIDE_ANCHOR, 1).replace(HEADER, INCLUDE, 1)
        self.assertEqual(restored, self.source)

    def test_changed_image_and_repeated_patch_rejected(self):
        for source in (self.source + '\n', patch(self.source),
                       self.source.replace(DIVIDE_ANCHOR, DIVIDE_ANCHOR.replace('0xEB38', '0xEB3C'), 1)):
            with self.assertRaises(ValueError):
                patch(source)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    args, remainder = parser.parse_known_args()
    if args.source:
        SOURCE = args.source
    unittest.main(argv=[sys.argv[0], *remainder])
