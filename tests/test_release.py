"""Check package integrity, deterministic archives and MSI upgrade ownership."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import release


class Release(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.stage = self.root / 'package'
        (self.stage / 'config').mkdir(parents=True)
        (self.stage / 'config/input.ini').write_text('[input]\n')
        (self.stage / 'HazeLauncher.exe').write_bytes(b'fixture, not a real binary')
        self.files = release.inventory(self.stage)
        (self.stage / 'manifest.json').write_text(json.dumps({'files': self.files}))

    def test_manifest_detects_changed_extra_and_missing_files(self):
        release.verify(self.stage)
        path = self.stage / 'HazeLauncher.exe'
        path.write_bytes(b'changed')
        with self.assertRaises(ValueError): release.verify(self.stage)
        path.write_bytes(b'fixture, not a real binary')
        extra = self.stage / 'EBOOT.ELF'
        extra.write_bytes(b'forbidden extra')
        with self.assertRaises(ValueError): release.verify(self.stage)
        extra.unlink()
        path.unlink()
        with self.assertRaises(ValueError): release.verify(self.stage)

    def test_zip_reproducibility_and_contents(self):
        a, b = self.root / 'a.zip', self.root / 'b.zip'
        release.archive(self.stage, a)
        release.archive(self.stage, b)
        self.assertEqual(a.read_bytes(), b.read_bytes())
        with zipfile.ZipFile(a) as archive:
            self.assertEqual(set(archive.namelist()), {'package/' + p for p in release.inventory(self.stage)})

    def test_msi_is_per_user_and_preserves_settings(self):
        wxs = self.root / 'installer.wxs'
        release.wix_source(self.stage, wxs, '0.1.0')
        ns = {'w': release.WIX}
        root = ET.parse(wxs)
        package = root.find('w:Package', ns)
        self.assertEqual(package.get('Scope'), 'perUser')
        self.assertEqual(package.get('UpgradeCode'), release.UPGRADE_CODE)
        self.assertIsNotNone(package.find('w:MajorUpgrade', ns))
        components = package.findall('.//w:Component', ns)
        settings = next(c for c in components if (f := c.find('w:File', ns)) is not None and f.get('Name') == 'input.ini')
        self.assertEqual(settings.get('Permanent'), 'yes')
        self.assertEqual(settings.get('NeverOverwrite'), 'yes')
        self.assertNotIn('RemoveFile', wxs.read_text())

    def test_runtime_allowlist_rejects_local_data(self):
        runtime = self.root / 'runtime'
        runtime.mkdir()
        for name in ('HazeRecomp.exe', 'HazeLauncher.exe', 'build-info.json', 'EBOOT.ELF'):
            (runtime / name).write_bytes(b'fixture')
        with self.assertRaisesRegex(ValueError, 'unexpected'):
            release.stage_package(runtime, self.root, self.root, self.root, self.root / 'out')


if __name__ == '__main__':
    unittest.main()
