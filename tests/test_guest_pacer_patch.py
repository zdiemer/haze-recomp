"""Verify the audited main-loop edit against the local lift, without shipping it."""
from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from patches.patch_guest_pacer import patch, ENTRY, SITE


class GuestPacerPatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=ROOT/'generated/ppu/ppu_recomp_003.cpp'
        if not path.exists(): raise unittest.SkipTest('Requires the locally generated BLUS30094 lift')
        cls.source=path.read_text()

    def test_only_wait_gate_changes(self):
        result=patch(self.source)
        # Reversing the two inserted statements must recover every original
        # update, render, clock, stack and frame-counter operation exactly.
        restored=result.replace(ENTRY+'\n        extern int haze_guest_pacer_bypass();',ENTRY,1)
        restored=restored.replace('loc_00681358:\n        if (haze_guest_pacer_bypass()) goto loc_006812DC;',
                                  'loc_00681358:',1)
        self.assertEqual(restored,self.source)
        self.assertEqual(result.count('if (haze_guest_pacer_bypass())'),1)
        self.assertIn('loc_006812DC:\n        ctx->lr = 0x006812E0; func_00734208(ctx);',result)

    def test_reject_changed_wait_or_update(self):
        for changed in [self.source.replace(SITE,SITE.replace('00680830','00680848'),1),
                        self.source.replace('ctx->lr = 0x00681004; func_0068ECD8',
                                            'ctx->lr = 0x00681004; func_0068EC78',1)]:
            with self.assertRaises(ValueError): patch(changed)

    def test_reject_repeated_patch(self):
        with self.assertRaises(ValueError): patch(patch(self.source))


if __name__=='__main__': unittest.main()
