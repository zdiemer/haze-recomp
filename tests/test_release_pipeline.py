"""Private input exclusion and immutable release artifact verification."""
from pathlib import Path
import json
import struct
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

    def publisher(self, bad_digest=False, existing=False, resume=False, published=False,
                  verify_only=False, missing_asset=False, wrong_id=False, wrong_tag=False):
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
        payload = {'id': 42, 'tag_name': 'v0.1.0', 'draft': not published,
                   'assets': [{'name': path.name, 'size': path.stat().st_size,
                    'digest': 'sha256:' + ('0' * 64 if bad_digest else release.digest(path))}
                   for path in artifacts.iterdir()]}
        if missing_asset:
            payload['assets'].pop()
        listed = json.dumps(payload)
        if wrong_tag:
            payload['tag_name'] = 'v9.9.9'
        published_payload = json.dumps(dict(payload, draft=False, html_url='https://example.invalid/release'))
        publisher = Path(__file__).resolve().parents[1] / 'scripts/publish-release.ps1'
        def quoted(path):
            return "'" + str(path).replace("'", "''") + "'"
        script.write_text("$ErrorActionPreference='Stop'\n$global:Created=$false\nfunction global:gh {\n"
            "$global:LASTEXITCODE=0\n$command=$args -join ' '\n"
            f"Add-Content -LiteralPath {quoted(calls)} -Value $command\n"
            "if ($command -like 'api */git/ref/tags/*') {\n"
            f"  return '{{\"object\":{{\"type\":\"commit\",\"sha\":\"{self.commit}\"}}}}'\n}}\n"
            "if ($command -match '/releases\\?per_page=') {\n"
            "  if ($command -notlike '*--paginate*') { throw 'Release listing must paginate' }\n"
            "  Write-Output '{\"id\":99,\"tag_name\":\"v9.9.9\",\"draft\":false}'\n"
            f"  if ($global:Created -or ${str(existing).lower()}) {{ Write-Output '{listed}' }}\n  return\n}}\n"
            "if ($command -like 'release create *') { $global:Created=$true; return }\n"
            "if ($command -like 'api */releases/tags/*') {\n"
            "  $global:LASTEXITCODE=1; [Console]::Error.WriteLine('Not Found (HTTP 404)'); return\n}\n"
            "if ($command -like 'api */releases/42 --method PATCH --field draft=false') {\n"
            f"  return '{published_payload}'\n}}\n"
            "if ($command -like 'api */releases/42') {\n"
            f"  return '{json.dumps(payload)}'\n}}\n"
            "throw \"Unexpected gh command: $command\"\n}\n"
            f"& {quoted(publisher)} -Directory {quoted(artifacts)} -Tag v0.1.0 -Commit {self.commit} "
            f"-Notes {quoted(notes)} -Python {quoted(sys.executable)} "
            + (f"-ResumeDraftId {43 if wrong_id else 42} " if resume else '')
            + ('-VerifyOnly ' if verify_only else '') + '\n', encoding='utf-8')
        result = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                                capture_output=True, text=True, timeout=30)
        return result, calls.read_text() if calls.exists() else ''

    def test_publisher_reveals_only_verified_complete_draft(self):
        result, calls = self.publisher()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('release create', calls)
        self.assertIn('--draft', calls)
        self.assertIn('/releases/42 --method PATCH --field draft=false', calls)
        self.assertNotIn('/releases/tags/', calls)

    def test_bad_uploaded_digest_keeps_draft_private(self):
        result, calls = self.publisher(bad_digest=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Uploaded checksum differs', result.stderr)
        self.assertIn('release create', calls)
        self.assertNotIn('--method PATCH', calls)

    def test_existing_release_cannot_be_overwritten(self):
        result, calls = self.publisher(existing=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('release create', calls)
        self.assertNotIn('--method PATCH', calls)

    def test_existing_draft_requires_explicit_resume(self):
        result, calls = self.publisher(existing=True, resume=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('release create', calls)
        self.assertIn('/releases/42 --method PATCH --field draft=false', calls)

    def test_resume_never_modifies_published_release(self):
        result, calls = self.publisher(existing=True, resume=True, published=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('--method PATCH', calls)
        self.assertNotIn('release create', calls)

    def test_resume_rejects_wrong_draft_id(self):
        result, calls = self.publisher(existing=True, resume=True, wrong_id=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('--method PATCH', calls)

    def test_resume_rejects_missing_draft(self):
        result, calls = self.publisher(resume=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('release create', calls)

    def test_resume_rejects_incomplete_upload(self):
        result, calls = self.publisher(existing=True, resume=True, missing_asset=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Uploaded asset set differs', result.stderr)
        self.assertNotIn('--method PATCH', calls)

    def test_release_identity_is_checked_again_by_id(self):
        result, calls = self.publisher(existing=True, resume=True, wrong_tag=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Release identity or draft state changed', result.stderr)
        self.assertNotIn('--method PATCH', calls)

    def test_verify_only_checks_real_hashes_without_publishing(self):
        result, calls = self.publisher(existing=True, resume=True, verify_only=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Verified draft 42', result.stdout)
        self.assertNotIn('--method PATCH', calls)
        self.assertNotIn('release create', calls)

    def test_verify_only_rejects_bad_uploaded_hash(self):
        result, calls = self.publisher(existing=True, resume=True, verify_only=True, bad_digest=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Uploaded checksum differs', result.stderr)
        self.assertNotIn('--method PATCH', calls)

    def test_verify_only_requires_explicit_draft(self):
        result, calls = self.publisher(verify_only=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, '')


class ReleaseBuildSteps(unittest.TestCase):
    def test_game_and_launcher_builds_share_the_release_root(self):
        # Both executables embed their program icon from the shared root
        # (windows-icon.cmake reads game/disc/PS3_GAME/ICON0.PNG under it).
        # The launcher step previously omitted -SharedRoot, so release
        # launchers fell back to the default Windows icon (#108).
        script = (Path(__file__).resolve().parents[1] / 'scripts' /
                  'build-release.ps1').read_text(encoding='utf-8')
        for step in ('game', 'launcher'):
            self.assertRegex(
                script,
                rf"Run-ReleaseStep '{step}'[^)]*'-SharedRoot',\$inputsPath",
                f"release {step} step must pass -SharedRoot $inputsPath")

    def test_bundled_logo_is_wired_into_setup_and_icon_fallback(self):
        # Hosted release builds have no game artwork: the private build
        # inputs carry generated sources only. The bundled logo is the icon
        # source for the setup executable (#98) and the fallback artwork
        # for the game/launcher when ICON0.PNG is unavailable (#108).
        root = Path(__file__).resolve().parents[1]
        png = root / 'assets/haze-logo.png'
        ico = root / 'assets/haze-logo.ico'
        self.assertTrue(png.is_file())
        with png.open('rb') as stream:
            self.assertEqual(stream.read(8), b'\x89PNG\r\n\x1a\n')
        with ico.open('rb') as stream:
            data = stream.read()
        reserved, kind, count = struct.unpack_from('<HHH', data)
        self.assertEqual((reserved, kind, count), (0, 1, 7))
        sizes = []
        for index in range(count):
            width, height = struct.unpack_from('<BB', data, 6 + 16 * index)
            sizes.append(width or 256)
            self.assertEqual(width, height)
        self.assertEqual(sizes, [16, 24, 32, 48, 64, 128, 256])
        installer = (root / 'scripts/installer.py').read_text(encoding='utf-8')
        self.assertIn('/win32icon:', installer)
        self.assertIn('haze-logo.ico', installer)
        cmake = (root / 'port/windows-icon.cmake').read_text(encoding='utf-8')
        self.assertIn('-FallbackSource "${icon_root}/assets/haze-logo.png"',
                      cmake)
        converter = (root / 'scripts/build-icon.ps1').read_text(encoding='utf-8')
        self.assertIn('[string]$FallbackSource', converter)


if __name__ == '__main__':
    unittest.main()
