from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from patches.patch_spu_triplet_copy import patch, ENTRY, INSERT


class TripletPatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ROOT/'generated/spu/spu_0041_at_00F2E380/spu_recomp.c'
        if not source.exists():
            raise unittest.SkipTest('Requires the locally generated BLUS30094 lift')
        cls.source = source.read_text()

    def test_only_prefix_changes(self):
        patched = patch(self.source)
        self.assertEqual(patched.count('unsigned native_items = haze_spu_triplet_copy(ctx);'), 1)
        restored = patched.replace(ENTRY + INSERT, ENTRY, 1).replace(
            '#include "spu_helpers.h"\n#include "haze_spu_triplet_copy.h"',
            '#include "spu_helpers.h"', 1)
        self.assertEqual(restored, self.source)

    def test_changed_image_and_repeated_patch_rejected(self):
        for source in (self.source + '\n', patch(self.source),
            self.source.replace('loc_000035F8:', 'loc_000035FC:', 1)):
            with self.assertRaises(ValueError): patch(source)


if __name__ == '__main__': unittest.main()
