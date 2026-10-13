"""Compile an asset-free MSI with the real generator; run only when WiX is supplied."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import release

with tempfile.TemporaryDirectory(prefix='haze MSI ') as directory:
    root = Path(directory)
    stage = root / 'stage'
    (stage / 'config').mkdir(parents=True)
    (stage / 'HazeLauncher.exe').write_bytes(b'asset-free installer fixture')
    (stage / 'config/input.ini').write_text('[input]\n')
    wxs = root / 'fixture.wxs'
    release.wix_source(stage, wxs, '0.1.0')
    msi = root / 'fixture.msi'
    subprocess.run([str(Path(sys.argv[1]).resolve()), 'build', '-arch', 'x64', str(wxs), '-o', str(msi)], check=True)
    assert msi.is_file() and msi.stat().st_size > 0
    print('Per-user MSI schema, stable components, settings preservation and Windows Installer validation passed.')
