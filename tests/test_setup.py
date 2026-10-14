"""Exercise the shipped PowerShell validators and helper exit/timeout handling."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def sfo(values):
    keys = bytearray()
    data = bytearray()
    entries = bytearray()
    for key, value in values.items():
        raw = value.encode() + b'\0'
        entries += struct.pack('<HHIII', len(keys), 0x0204, len(raw), len(raw), len(data))
        keys += key.encode() + b'\0'
        data += raw
    start = 20 + len(entries)
    return struct.pack('<5I', 0x46535000, 0x101, start, start + len(keys), len(values)) + entries + keys + data


class Setup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='haze setup ')
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)

    def run_ps(self, source):
        script = self.path / 'test.ps1'
        common = str(ROOT / 'scripts/setup-common.ps1').replace("'", "''")
        script.write_text(f"$ErrorActionPreference='Stop'\n. '{common}'\n" + source)
        return subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                              cwd=self.path, capture_output=True, text=True, timeout=30)

    def test_valid_metadata(self):
        (self.path / 'PARAM.SFO').write_bytes(sfo({'TITLE_ID': 'BLUS30094', 'VERSION': '01.02', 'APP_VER': '01.00'}))
        result = self.run_ps("$s=Read-HazeSfo 'PARAM.SFO'; if ($s.TITLE_ID -ne 'BLUS30094') { throw 'wrong metadata' }")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_malformed_and_wrong_region(self):
        for content in (b'encrypted', sfo({'TITLE_ID': 'BLES00000', 'VERSION': '01.02', 'APP_VER': '01.00'}),
                        struct.pack('<5I', 0x46535000, 0, 0xffffffff, 0xffffffff, 0xffffffff)):
            target = self.path / 'PS3_GAME/PARAM.SFO'
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(content)
            result = self.run_ps("Assert-HazeDisc '.'")
            self.assertNotEqual(result.returncode, 0)
            self.assertRegex(result.stderr, 'Unreadable|Unsupported disc|Truncated')

    def test_bad_elf(self):
        (self.path / 'bad.elf').write_bytes(b'\x7fELF' + bytes(128))
        result = self.run_ps("Assert-HazeElf 'bad.elf'")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unsupported ELF', result.stderr)

    def test_helper_exit_and_timeout(self):
        for body, timeout, expected in [('exit 0', 10, 0), ('exit 7', 10, 1), ('Start-Sleep 20', 1, 1)]:
            (self.path / 'child.ps1').write_text(body)
            result = self.run_ps(f"Invoke-HazeHelper (Get-Process -Id $PID).Path @('-NoProfile','-ExecutionPolicy','Bypass','-File',\"$PWD/child.ps1\") $PWD.Path \"$PWD/helper\" {timeout}")
            self.assertEqual(result.returncode, expected, result.stderr)


if __name__ == '__main__':
    unittest.main()
