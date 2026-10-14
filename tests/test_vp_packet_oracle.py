"""Keep the packing oracle tied to the audited game operation."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from patches.extract_vp_packet_oracle import extract

class PacketOracle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = ROOT / 'generated/ppu/ppu_recomp_003.cpp'
        if not path.exists():
            raise unittest.SkipTest('Requires the private BLUS30094 lift')
        cls.source = path.read_text()

    def test_loop_is_extracted_without_rewriting_operations(self):
        output = extract(self.source)
        loop = output[output.index('loc_0072C4F8:'):-2]
        self.assertIn(loop, self.source)
        self.assertEqual(loop.count('vm_read32('), 35)
        self.assertEqual(loop.count('vm_write32('), 34)

    def test_changed_function_is_rejected(self):
        entry = 'void func_0072C368(ppu_context* ctx) {'
        with self.assertRaises(ValueError):
            extract(self.source.replace(entry, entry+'\n/* changed */', 1))

if __name__ == '__main__':
    unittest.main()
