"""Export/verify the source-only private snapshot used by hosted release builds."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

REQUIRED = ('ppu/ppu_recomp.h', 'ppu/ppu_recomp_003.cpp', 'spu/spu_workloads.c',
            'spu/firmware_register.c', 'spu/audio_plugins_register.c')


def inventory(root):
    result = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
            raise ValueError('Build inputs cannot contain links')
        if path.is_file():
            name = path.relative_to(root).as_posix()
            if name.split('/')[0] not in ('ppu', 'spu') or path.suffix not in ('.c', '.cpp', '.h'):
                raise ValueError(f'Unexpected build input: {name}')
            with path.open('rb') as stream:
                result[name] = hashlib.file_digest(stream, 'sha256').hexdigest()
    for name in REQUIRED:
        if name not in result:
            raise ValueError(f'Missing build input: {name}')
    return result


def export(source, destination):
    if destination.exists():
        raise ValueError('Use a new snapshot directory')
    for directory in ('ppu', 'spu'):
        for path in sorted((source / directory).rglob('*')):
            if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
                raise ValueError('Build inputs cannot contain links')
            if path.is_file() and path.suffix in ('.c', '.cpp', '.h'):
                target = destination / 'generated' / path.relative_to(source)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
    manifest = {'schema': 1, 'title': 'BLUS30094', 'files': inventory(destination / 'generated')}
    (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (destination / '.gitattributes').write_text('* -text\n', encoding='ascii')
    (destination / 'README.md').write_text(
        '# Private Haze build inputs\n\nGenerated C/C++ and headers only. Never make this repository public.\n'
        'No ISO, ELF, extracted assets, keys, or firmware binary files are included.\n'
        'Update with scripts/build_inputs.py in haze-recomp, then pin the new commit there.\n', encoding='utf-8')


def verify(root):
    manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or manifest.get('files') != inventory(root / 'generated'):
        raise ValueError('Private snapshot does not match its manifest')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--verify', type=Path)
    args = parser.parse_args()
    if args.verify:
        verify(args.verify)
    elif args.export and args.output:
        export(args.export, args.output)
    else:
        parser.error('Use --verify SNAPSHOT or --export GENERATED --output NEW_DIRECTORY')
