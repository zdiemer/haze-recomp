"""Private input exclusion and immutable release artifact verification."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import build_inputs
import release
import release_artifacts


class BuildInputs(unittest.TestCase):
    def test_export_omits_binaries_and_detects_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'generated'
            for name in build_inputs.REQUIRED:
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('/* synthetic fixture */\n')
            (source / 'ppu/EBOOT.ELF').write_bytes(b'not a real executable')
            (source / 'spu/image.bin').write_bytes(b'not real game data')
            snapshot = root / 'snapshot'
            build_inputs.export(source, snapshot)
            build_inputs.verify(snapshot)
            self.assertFalse(list(snapshot.rglob('*.ELF')))
            self.assertFalse(list(snapshot.rglob('*.bin')))
            (snapshot / 'generated/ppu/ppu_recomp.h').write_text('changed')
            with self.assertRaisesRegex(ValueError, 'manifest'):
                build_inputs.verify(snapshot)

    def test_missing_required_input_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ValueError, 'Missing build input'):
                build_inputs.inventory(Path(temporary))


class Artifacts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.version = '0.1.0'
        self.commit = 'a' * 40
        name = 'HazeRecomp-0.1.0-windows-x64'
        with zipfile.ZipFile(self.root / (name + '.zip'), 'w') as stream:
            stream.writestr(name + '/manifest.json', json.dumps({
                'version': self.version, 'commit': self.commit, 'files': {}}))
        (self.root / 'HazeRecomp-0.1.0-Setup.exe').write_bytes(b'synthetic installer')
        (self.root / 'HazeRecomp-dependency-sources.zip').write_bytes(b'synthetic source')
        (self.root / 'release.json').write_text(json.dumps({
            'schema': 1, 'version': self.version, 'commit': self.commit, 'files': release.inventory(self.root)}))
        (self.root / 'SHA256SUMS.txt').write_text(''.join(
            f'{sha}  {name}\n' for name, sha in release.inventory(self.root).items()))

    def test_verified_release_and_target(self):
        release_artifacts.verify(self.root, 'v0.1.0', self.commit)
        with self.assertRaisesRegex(ValueError, 'version'):
            release_artifacts.verify(self.root, 'v0.1.1', self.commit)
        with self.assertRaisesRegex(ValueError, 'commit'):
            release_artifacts.verify(self.root, 'v0.1.0', 'b' * 40)

    def test_extra_private_file_is_rejected(self):
        (self.root / 'private-generated.cpp').write_text('must not upload')
        with self.assertRaisesRegex(ValueError, 'unexpected files'):
            release_artifacts.verify(self.root)

    def test_tampered_installer_is_rejected(self):
        (self.root / 'HazeRecomp-0.1.0-Setup.exe').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            release_artifacts.verify(self.root)

    def test_tampered_checksums_are_rejected(self):
        (self.root / 'SHA256SUMS.txt').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'Checksums'):
            release_artifacts.verify(self.root)


if __name__ == '__main__':
    unittest.main()
