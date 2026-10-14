from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from patches.patch_vp_upload import patch,DECL,BEGIN,END
from patches.patch_guest_pacer import patch as pacer
from patches.patch_job_wait import patch as job_wait

class UploadPatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p=ROOT/'generated/ppu/ppu_recomp_003.cpp'
        if not p.exists():raise unittest.SkipTest('Requires private BLUS30094 lift')
        cls.source=p.read_text()
    def test_composition_and_reversibility(self):
        for source in (self.source,job_wait(pacer(self.source))):
            result=patch(source)
            self.assertEqual(result.replace(DECL,'',1).replace(BEGIN,'',1).replace(END,'',1),source)
    def test_changed_boundary_or_repeat_rejected(self):
        with self.assertRaises(ValueError):patch(patch(self.source))
        entry='void func_0072C368(ppu_context* ctx) {'
        with self.assertRaises(ValueError):patch(self.source.replace(entry,entry+'\n/* mutation */',1))
if __name__=='__main__':unittest.main()
