"""Fail closed on changed completion, work servicing or generic sleep semantics."""
from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from patches.patch_job_wait import patch,CALL,DIGESTS,PROFILE_ENTRY,PROFILE_CALLS
from patches.patch_guest_pacer import patch as patch_pacer

class JobWaitPatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=ROOT/'generated/ppu/ppu_recomp_003.cpp'
        if not path.exists():raise unittest.SkipTest('Requires private BLUS30094 lift')
        cls.source=path.read_text()

    def test_only_two_boundaries_change_and_composes(self):
        for source in (self.source,patch_pacer(self.source)):
            result=patch(source)
            restored=result.replace('#include "haze_job_profile.h"\n','',1).replace(PROFILE_ENTRY,'',1)
            for phase, call in enumerate(PROFILE_CALLS):
                restored=restored.replace(f'job_profile.start({phase}); {call} job_profile.stop({phase});',call,1)
            restored=restored.replace('        extern void haze_job_wait_call(ppu_context*);\n','',1)
            restored=restored.replace('        extern bool haze_job_wait_usleep(ppu_context*);\n','',1)
            restored=restored.replace(CALL.replace('func_0068EFC0','haze_job_wait_call'),CALL,1)
            restored=restored.replace('if (!haze_job_wait_usleep(ctx)) lv2_syscall(ctx);','lv2_syscall(ctx);',1)
            self.assertEqual(restored,source)
            for target in ('func_00682510(ctx)','func_00722ED8(ctx)','func_00723198(ctx)'):
                self.assertEqual(result.count(target),source.count(target))

    def test_reject_changed_boundary_or_repeat(self):
        for address in DIGESTS:
            entry=f'void func_{address}(ppu_context* ctx) {{'
            with self.subTest(address=address),self.assertRaises(ValueError):
                patch(self.source.replace(entry,entry+'\n        /* changed */',1))
        with self.assertRaises(ValueError):patch(patch(self.source))

if __name__=='__main__':unittest.main()
