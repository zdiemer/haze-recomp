"""Build the Windows installer UI with its MSI and preparation tools embedded."""
from pathlib import Path
import os
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def build_installer(msi, tools, output, version):
    payload = output / 'setup-payload.zip'
    with zipfile.ZipFile(payload, 'w', compression=zipfile.ZIP_DEFLATED) as stream:
        stream.write(msi, 'HazeRecomp.msi')
        for path in sorted((tools / '7zip').rglob('*')):
            if path.is_file():
                stream.write(path, 'tools/7zip/' + path.relative_to(tools / '7zip').as_posix())
        for name in ('release-tools.json', 'firmware.json', 'vc-runtime.json'):
            stream.write(ROOT / 'config' / name, 'tools/' + name)
        for name in ('setup.ps1', 'setup-common.ps1'):
            stream.write(ROOT / 'scripts' / name, 'scripts/' + name)
    compiler = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    result = output / f'HazeRecomp-{version}-Setup.exe'
    subprocess.run([str(compiler), '/nologo', '/target:winexe', '/platform:x64',
        '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll',
        '/reference:System.IO.Compression.dll', '/reference:System.IO.Compression.FileSystem.dll',
        f'/resource:{payload.resolve()},payload.zip', f'/out:{result.resolve()}',
        str(ROOT / 'installer/Program.cs')], check=True)
    subprocess.run([str(result.resolve()), '--verify-payload'], check=True)
    return result
