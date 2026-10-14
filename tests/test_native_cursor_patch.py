import hashlib
from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from patches.patch_native_cursor import body,patch,IMAGE_DIGEST


class CursorPatch(unittest.TestCase):
    def setUp(self):
        self.source=(ROOT/'generated/spu/spu_0039_at_00F1C100/spu_recomp.c').read_text()

    def test_original_callers_remain_exact_fallback(self):
        changed=patch(self.source)
        self.assertEqual(changed.count('haze_native_cursor_pair(ctx,'),2)
        for address in ('000047E0','00004900'):
            original=body(self.source,address)
            calls=original.split('\n',1)[1].split('        ctx->gpr[41]' if address=='000047E0' else '        ctx->gpr[55]')[0]
            self.assertIn(calls,body(changed,address))
        self.assertEqual(body(changed,'00003450'),body(self.source,'00003450'))
        self.assertEqual(hashlib.sha256(self.source.encode()).hexdigest(),IMAGE_DIGEST)

    def test_fail_closed_on_leaf_caller_or_registration_source_changes(self):
        for address in ('00003450','000047E0','00004900','00004878','00006918'):
            with self.subTest(address=address),self.assertRaises(ValueError):
                patch(self.source.replace('spu_func_'+address,'spu_func_'+address+'_changed'))
        with self.assertRaises(ValueError):patch(patch(self.source))


if __name__=='__main__':unittest.main()
