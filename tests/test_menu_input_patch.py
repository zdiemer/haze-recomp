"""The native GUI bridge must reject any changed guest update body."""
import importlib.util
from pathlib import Path
import sys
import unittest

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('patch_menu_input', root / 'scripts/patches/patch_menu_input.py')
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)


class MenuPatchTests(unittest.TestCase):
    def test_audited_translation(self):
        source = Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else ''
        self.assertTrue(source, 'Pass the locally generated ppu_recomp_001.cpp')
        patched = patcher.patch(source)
        for name in patcher.DIGESTS:
            self.assertEqual(patched.count(f'void func_{name}(ppu_context* ctx)'), 1)
            if name not in ('002F69D8', '00301C18'):
                self.assertEqual(patched.count(f'void haze_original_{name}(ppu_context* ctx)'), 1)
            changed = source.replace(f'void func_{name}(ppu_context* ctx) {{',
                                     f'void func_{name}(ppu_context* ctx) {{\n// changed', 1)
            with self.assertRaises(ValueError):
                patcher.patch(changed)
        with self.assertRaises(ValueError):
            patcher.patch(patched)
        self.assertIn('haze_menu_pointer_before(input);', patched)
        self.assertIn('haze_menu_pointer_after(input);', patched)
        self.assertEqual(patched.count('if (!haze_menu_pointer_confirm('), 1)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
