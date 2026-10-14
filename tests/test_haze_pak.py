"""Archive bounds and full multi-stream decoding, using generated fixtures."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from haze_pak import Pak


class PakTests(unittest.TestCase):
    def fixture(self, root, payload, size, compressed=True, shard=0):
        path = Path(root) / "test.pak"
        path.write_bytes(struct.pack(">6I", 0x5042434B, 24, 1, 44, 0, ((shard+1)<<16)|2) +
                         struct.pack(">5I", 0x12345678, size, len(payload) if compressed else 0, 16, shard))
        Path(f"{path}.{shard:02d}").write_bytes(bytes(16) + payload)
        return Pak(path)

    def test_multiple_streams(self):
        blocks = [b"a" * 16384, b"b" * 16384, b"native menu resource tail"]
        with tempfile.TemporaryDirectory() as root:
            pak = self.fixture(root, b"".join(zlib.compress(block) for block in blocks), sum(map(len, blocks)), shard=1)
            self.assertEqual(pak.read(pak.entries[0]), b"".join(blocks))

    def test_uncompressed(self):
        with tempfile.TemporaryDirectory() as root:
            pak = self.fixture(root, b"resource", 8, compressed=False)
            self.assertEqual(pak.read(pak.entries[0]), b"resource")

    def test_bad_streams_and_sizes(self):
        with tempfile.TemporaryDirectory() as root:
            good = zlib.compress(b"resource")
            for payload, size in ((good[:-1], 8), (good, 7), (good, 9), (good + b"junk", 8)):
                with self.subTest(payload=payload, size=size):
                    pak = self.fixture(root, payload, size)
                    with self.assertRaises(ValueError):
                        pak.read(pak.entries[0])

    def test_bounds(self):
        with tempfile.TemporaryDirectory() as root:
            pak = self.fixture(root, b"short", 100, compressed=False)
            with self.assertRaisesRegex(ValueError, "beyond shard"):
                pak.read(pak.entries[0])
            with self.assertRaisesRegex(ValueError, "limit"):
                pak.read(pak.entries[0], limit=50)
            data = bytearray(pak.path.read_bytes())
            struct.pack_into(">I", data, 12, 45)
            pak.path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "table bounds"):
                Pak(pak.path)


if __name__ == "__main__":
    unittest.main()
