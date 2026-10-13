"""Private input exclusion and immutable release artifact verification."""
from pathlib import Path
import json
import sys
import subprocess
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

    def publisher(self, bad_digest=False, existing=False):
        script = self.root / 'fake-publish.ps1'
        # Place mocks outside the strictly allowlisted artifact directory.
        artifacts = self.root / 'artifacts'
        artifacts.mkdir()
        for path in list(self.root.iterdir()):
            if path.is_file():
                path.rename(artifacts / path.name)
        notes = self.root / 'notes.txt'
        notes.write_text('Synthetic publication regression.\n')
        calls = self.root / 'calls.txt'
        payload = {'assets': [{'name': path.name, 'size': path.stat().st_size,
                    'digest': 'sha256:' + ('0' * 64 if bad_digest else release.digest(path))}
                   for path in artifacts.iterdir()]}
        publisher = Path(__file__).resolve().parents[1] / 'scripts/publish-release.ps1'
        def quoted(path):
            return "'" + str(path).replace("'", "''") + "'"
        script.write_text("$ErrorActionPreference='Stop'\nfunction global:gh {\n"
            "$global:LASTEXITCODE=0\n$command=$args -join ' '\n"
            f"Add-Content -LiteralPath {quoted(calls)} -Value $command\n"
            "if ($command -like 'api */git/ref/tags/*') {\n"
            f"  return '{{\"object\":{{\"type\":\"commit\",\"sha\":\"{self.commit}\"}}}}'\n}}\n"
            "if ($command -match '/releases\\?per_page=') { " + ("return 'v0.1.0'" if existing else 'return') + " }\n"
            "if ($command -like 'api */releases/tags/*') {\n"
            f"  return '{json.dumps(payload)}'\n}}\n}}\n"
            f"& {quoted(publisher)} -Directory {quoted(artifacts)} -Tag v0.1.0 -Commit {self.commit} "
            f"-Notes {quoted(notes)} -Python {quoted(sys.executable)}\n", encoding='utf-8')
        result = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                                capture_output=True, text=True, timeout=30)
        return result, calls.read_text() if calls.exists() else ''

    def test_publisher_reveals_only_verified_complete_draft(self):
        result, calls = self.publisher()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('release create', calls)
        self.assertIn('--draft', calls)
        self.assertIn('release edit v0.1.0', calls)

    def test_bad_uploaded_digest_keeps_draft_private(self):
        result, calls = self.publisher(bad_digest=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Uploaded checksum differs', result.stderr)
        self.assertIn('release create', calls)
        self.assertNotIn('release edit', calls)

    def test_existing_release_cannot_be_overwritten(self):
        result, calls = self.publisher(existing=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('release create', calls)
        self.assertNotIn('release edit', calls)


if __name__ == '__main__':
    unittest.main()
