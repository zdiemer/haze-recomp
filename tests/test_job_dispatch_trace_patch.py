"""Ensure diagnostic insertion preserves guest operations and rejects changed lifts."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import patch_job_dispatch_trace as trace
from patch_guest_pacer import patch as pacer
from patch_job_wait import patch as wait
from patch_vp_upload import patch as vp


class DispatchTracePatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = ROOT / 'generated/ppu/ppu_recomp_003.cpp'
        if not path.exists():
            raise unittest.SkipTest('Requires private BLUS30094 lift')
        cls.source = vp(wait(pacer(path.read_text())))

    def test_preserves_all_guest_operations(self):
        result = trace.patch(self.source)
        restored = result.replace(trace.INCLUDE, '', 1)
        for address in trace.DIGESTS:
            restored = restored.replace(trace.entry(address), '', 1)
        restored = restored.replace(trace.SLOT, '', 1)
        restored = restored.replace(trace.ROUTE, '', 1)
        self.assertEqual(restored.count(trace.BASE), len(trace.BASE_CALLS))
        restored = restored.replace(trace.BASE, '')
        restored = restored.replace(trace.DISPATCH_BEGIN, '', 1)
        restored = restored.replace('\n' + trace.DISPATCH_END.rstrip('\n'), '', 1)
        self.assertEqual(restored, self.source)
        self.assertIn('loc_00682740:\n' + trace.SLOT, result)
        self.assertIn('loc_0068274C:\n' + trace.ROUTE, result)

    def test_rejects_changed_functions_and_repeated_patch(self):
        for address in trace.DIGESTS:
            marker = f'void func_{address}(ppu_context* ctx) {{'
            with self.subTest(address=address), self.assertRaises(ValueError):
                trace.patch(self.source.replace(marker, marker + '\n /* changed */', 1))
        with self.assertRaises(ValueError):
            trace.patch(trace.patch(self.source))


if __name__ == '__main__':
    unittest.main()
