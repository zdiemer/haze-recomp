"""Prepare an explicit public artifact set and validate it before publication."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

from release import digest, inventory


def prepare(project, work, inputs_commit):
    version = (project / 'VERSION').read_text().strip()
    commit = subprocess.check_output(['git', '-C', str(project), 'rev-parse', 'HEAD'], text=True).strip()
    output = work / 'artifacts'
    output.mkdir(exist_ok=False)
    for extension in ('zip',):
        name = f'HazeRecomp-{version}-windows-x64.{extension}'
        shutil.copyfile(work / 'package' / name, output / name)
    name = f'HazeRecomp-{version}-Setup.exe'
    shutil.copyfile(work / 'package' / name, output / name)
    shutil.copyfile(work / 'redistribution/HazeRecomp-dependency-sources.zip', output / 'HazeRecomp-dependency-sources.zip')
    files = inventory(output)
    manifest = {'schema': 1, 'version': version, 'commit': commit, 'inputs_commit': inputs_commit,
                'files': files}
    (output / 'release.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    hashes = inventory(output)
    (output / 'SHA256SUMS.txt').write_text(''.join(f'{h}  {n}\n' for n, h in hashes.items()), encoding='ascii')
    verify(output)


def verify(root, tag=None, commit=None):
    manifest = json.loads((root / 'release.json').read_text(encoding='utf-8'))
    version = manifest['version']
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('Invalid release version')
    if tag is not None and tag != 'v' + version:
        raise ValueError('Tag does not match the package version')
    if commit is not None and commit != manifest['commit']:
        raise ValueError('Tag does not match the package commit')
    expected = {f'HazeRecomp-{version}-windows-x64.zip',
                f'HazeRecomp-{version}-Setup.exe', 'HazeRecomp-dependency-sources.zip'}
    if set(manifest['files']) != expected:
        raise ValueError('Unexpected release asset names')
    actual = inventory(root)
    if set(actual) != expected | {'release.json', 'SHA256SUMS.txt'}:
        raise ValueError('Release directory contains unexpected files')
    if any(actual[name] != value for name, value in manifest['files'].items()):
        raise ValueError('Release artifact hash mismatch')
    checksums = ''.join(f'{actual[n]}  {n}\n' for n in sorted(actual) if n != 'SHA256SUMS.txt')
    if (root / 'SHA256SUMS.txt').read_text() != checksums:
        raise ValueError('Checksums do not match the release')
    with zipfile.ZipFile(root / f'HazeRecomp-{version}-windows-x64.zip') as archive:
        prefix = f'HazeRecomp-{version}-windows-x64/'
        contents = json.loads(archive.read(prefix + 'manifest.json'))
        if contents['commit'] != manifest['commit'] or contents['version'] != version:
            raise ValueError('Portable package provenance mismatch')
        names = set(archive.namelist())
        if names != {prefix + name for name in contents['files']} | {prefix + 'manifest.json'}:
            raise ValueError('Portable archive contains unexpected files')
        import hashlib
        for name, sha in contents['files'].items():
            if hashlib.sha256(archive.read(prefix + name)).hexdigest() != sha:
                raise ValueError('Portable package content mismatch')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'verify'))
    parser.add_argument('--project', type=Path)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--inputs-commit')
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--tag')
    parser.add_argument('--commit')
    args = parser.parse_args()
    if args.command == 'prepare':
        prepare(args.project, args.work, args.inputs_commit)
    else:
        verify(args.directory, args.tag, args.commit)
