"""Exercise command routing and the shared patch I/O and registration guards."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import diagnose

spec = importlib.util.spec_from_file_location('patch_cli', ROOT / 'scripts/patch.py')
patch_cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch_cli)


class ScriptCommands(unittest.TestCase):
    def test_command_help_from_another_directory(self):
        # Exercise relative package imports as well as argparse forwarding.
        with tempfile.TemporaryDirectory() as directory:
            for tool, commands in (
                ('diagnose.py', diagnose.COMMANDS),
                ('patch.py', (*patch_cli.PATCHES, *patch_cli.ORACLES)),
            ):
                for command in commands:
                    with self.subTest(tool=tool, command=command):
                        result = subprocess.run(
                            [sys.executable, str(ROOT / 'scripts' / tool), command, '--help'],
                            cwd=directory, capture_output=True, text=True, timeout=15)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertIn(command, result.stdout)

    def test_unknown_command_is_rejected(self):
        for tool in ('diagnose.py', 'patch.py'):
            result = subprocess.run([sys.executable, str(ROOT / 'scripts' / tool), '../setup'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 2)
            self.assertIn('invalid choice', result.stderr)

    def test_latin1_patch_preserves_bytes_and_supports_in_place_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source.cpp'
            source.write_bytes(b'\x80\xff\r\noriginal\r\n')
            def transform(text):
                self.assertEqual(text, '\x80\xff\noriginal\n')
                return text.replace('original', 'patched')
            with patch.object(sys, 'argv', ['patch.py', 'audio-tick', str(source), str(source)]), \
                 patch.object(patch_cli.importlib, 'import_module', return_value=SimpleNamespace(patch=transform)):
                patch_cli.main()
            self.assertEqual(source.read_bytes(), b'\x80\xff\npatched\n')

    def test_changed_registration_never_invokes_transform_or_overwrites_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source.c'
            source.write_text('source')
            registration = Path(directory) / 'registration.c'
            registration.write_text('spu_begin_image(999);')
            output = Path(directory) / 'output.c'
            output.write_text('keep')
            for name in patch_cli.REGISTRATIONS:
                with self.subTest(patch=name), \
                     patch.object(sys, 'argv', ['patch.py', name, str(source), str(output),
                                               '--registration', str(registration)]), \
                     patch.object(patch_cli.importlib, 'import_module') as module:
                    with self.assertRaisesRegex(ValueError, 'registration changed'):
                        patch_cli.main()
                    module.return_value.patch.assert_not_called()
                    self.assertEqual(output.read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()
