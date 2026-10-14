"""Reject changed lifts and verify capture preserves every original operation."""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patch_job0_capture import patch
from prepare_job0_oracle import prepare
from patch_job0_kernel import patch as kernel_patch


class Job0Patch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = Path(__file__).resolve().parents[1] / 'generated/spu/spu_0029_at_00EC9A80/spu_recomp.c'
        if not source.exists():
            raise unittest.SkipTest('Private image-30 lift is unavailable')
        cls.source = source.read_text()

    def test_capture_operation_preservation(self):
        changed = patch(self.source)
        self.assertEqual(changed.count('haze_job0_capture_input('), 3)
        restored = re.sub(r'#if defined\(HAZE_JOB_TRACE\)\n.*?\n#endif\n', '', changed)
        restored = restored.replace('\n#include "haze_job0_capture.h"', '', 1)
        self.assertEqual(restored, self.source)

    def test_reject_changed_and_repeated_inputs(self):
        for source in (self.source + '\n', patch(self.source)):
            with self.assertRaises(ValueError):
                patch(source)
            with self.assertRaises(ValueError):
                prepare(source)
            with self.assertRaises(ValueError):
                kernel_patch(source)

    def test_native_sites_are_restricted(self):
        changed = kernel_patch(self.source)
        self.assertEqual(changed.count('if (haze_job0_kernel_accept(ctx,'), 2)
        self.assertNotIn('haze_job0_kernel_accept(ctx, 0x00003F48)', changed)
        with self.assertRaises(ValueError):
            kernel_patch(changed)


if __name__ == '__main__':
    unittest.main()
