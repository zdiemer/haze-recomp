"""Fetch pinned dependency sources, preserve notices and package matching source."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import urllib.request
import zipfile

from release import digest, inventory

ROOT = Path(__file__).resolve().parents[1]


def fetch(output):
    output.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / 'config/release-sources.json').read_text())
    for name, item in lock.items():
        archive = output / f'{name}.zip'
        if not archive.exists():
            print(f'Downloading {name} source...', flush=True)
            urllib.request.urlretrieve(item['url'], archive)
        if digest(archive) != item['sha256']:
            raise ValueError(f'Source checksum mismatch: {name}')
        directory = output / name
        if directory.exists():
            raise ValueError(f'Use a fresh source directory: {directory}')
        with zipfile.ZipFile(archive) as stream:
            members = stream.infolist()
            prefixes = {m.filename.split('/')[0] for m in members}
            if len(prefixes) != 1:
                raise ValueError('Source archive must have exactly one root')
            prefix = prefixes.pop() + '/'
            for member in members:
                relative = member.filename.removeprefix(prefix)
                if not relative or member.is_dir():
                    continue
                path = Path(relative)
                if path.is_absolute() or '..' in path.parts or ':' in relative or '\\' in relative:
                    raise ValueError('Unsafe source archive path')
                target = directory / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(stream.read(member))
    (output / 'lock.json').write_text(json.dumps(lock, indent=2) + '\n')


def bundle(sources, project, ffmpeg, output):
    output.mkdir(parents=True, exist_ok=False)
    licenses = output / 'licenses'
    licenses.mkdir()
    shutil.copy2(project / 'LICENSE', licenses / 'Haze-MIT.txt')
    shutil.copy2(project / 'external/ps3recomp/LICENSE', licenses / 'ps3recomp-MIT.txt')
    shutil.copy2(project / 'assets/input-prompts/License.txt', licenses / 'Kenney-CC0.txt')
    shutil.copy2(sources / 'sdl/LICENSE.txt', licenses / 'SDL-zlib.txt')
    shutil.copytree(sources / '7zip/DOC', licenses / '7zip')
    for name in ('COPYING.LGPLv2.1', 'COPYING.LGPLv3', 'LICENSE.md'):
        shutil.copy2(sources / 'ffmpeg' / name, licenses / ('FFmpeg-' + name))
    (licenses / 'THIRD-PARTY-NOTICES.txt').write_text(
        'Haze Recomp and ps3recomp: MIT. SDL 3.4.18: zlib. Kenney Input Prompts: CC0.\n'
        'FFmpeg: LGPL 2.1 or later, dynamically linked; built from the accompanying source\n'
        'with external libraries, GPL and nonfree options disabled. You may replace these\n'
        'DLLs with compatible modified builds. Reverse engineering for debugging such\n'
        'modifications is permitted. See the FFmpeg license texts.\n'
        '7-Zip 26.04: LGPL and BSD portions with the unRAR restriction; see 7zip/License.txt.\n'
        'Microsoft Visual C++ runtime: Copyright Microsoft Corporation. Redistributed\n'
        'under the Visual Studio distributable-code terms; see Microsoft-Redist.txt.\n'
        'RPCS3 and Sony firmware are downloaded from their respective providers during\n'
        'setup and are not included in this installer or portable archive.\n', encoding='utf-8')
    (licenses / 'SOURCE-OFFER.txt').write_text(
        'The matching open-source dependency sources and FFmpeg build recipe are provided\n'
        'alongside this release as HazeRecomp-dependency-sources.zip. This is direct source\n'
        'distribution, not a promise to supply sources later. The archive contains the\n'
        'unmodified pinned FFmpeg, SDL and 7-Zip source archives, project/SDK sources and\n'
        'the FFmpeg configure log. Private game-derived translations are not included.\n', encoding='utf-8')
    vswhere = Path('C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')
    vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    shutil.copy2(Path(vs) / 'Licenses/1033/Redist.txt', licenses / 'Microsoft-Redist.txt')
    archive = output / 'HazeRecomp-dependency-sources.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as stream:
        lock = json.loads((sources / 'lock.json').read_text())
        for name, item in lock.items():
            source = sources / f'{name}.zip'
            if digest(source) != item['sha256']:
                raise ValueError('Source archive changed after preparation')
            stream.write(source, f'{name}.zip')
        stream.write(sources / 'lock.json', 'sources.json')
        stream.write(ffmpeg / 'build-config.log', 'ffmpeg-build-config.log')
        for name in ('build-ffmpeg.ps1', 'build-ffmpeg.sh'):
            stream.write(ROOT / 'scripts' / name, name)
        for name, directory in (('haze', project), ('ps3recomp', project / 'external/ps3recomp'), ('release-tooling', ROOT)):
            source = output / f'{name}-source.zip'
            subprocess.run(['git', '-C', str(directory), 'archive', '--format=zip',
                            '-o', str(source.resolve()), 'HEAD'], check=True)
            stream.write(source, source.name)
            source.unlink()
    (output / 'redistribution.json').write_text(json.dumps({
        'sources': digest(archive), 'licenses': inventory(licenses),
        'ffmpeg': inventory(ffmpeg), 'source_lock': lock,
    }, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', type=Path)
    parser.add_argument('--sources', type=Path)
    parser.add_argument('--project', type=Path)
    parser.add_argument('--ffmpeg', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.fetch:
        fetch(args.fetch)
    elif all((args.sources, args.project, args.ffmpeg, args.output)):
        bundle(args.sources, args.project, args.ffmpeg, args.output)
    else:
        parser.error('Provide --fetch or all bundle arguments')
