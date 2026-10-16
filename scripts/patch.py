"""Apply audited generated-code patches or extract private regression oracles.

CMake invokes this tool automatically. Source hashes and registration guards
remain in force; changed translations require a new audit.
"""
import argparse
import importlib
from pathlib import Path
import runpy
import sys


PATCHES = (
    'audio-tick', 'bulk-fill', 'guest-pacer', 'job0-capture', 'job0-kernel',
    'job-dispatch-trace', 'job-wait', 'local-dispatch', 'menu-input',
    'menu-lifetime', 'menu-material', 'menu-pages', 'movie-poll', 'native-cursor',
    'producer-profile', 'save-status', 'savedata', 'slot-copy', 'spu-idle',
    'spu-triplet-copy', 'startup-splash', 'teammate-name', 'title-prompt', 'vp-upload',
)
ORACLES = {
    'oracle-bulk-fill': 'extract_bulk_fill_oracle',
    'oracle-slot-copy': 'extract_slot_copy_oracle',
    'oracle-vp-packet': 'extract_vp_packet_oracle',
    'oracle-cursor': 'prepare_cursor_oracle',
    'oracle-job0': 'prepare_job0_oracle',
    'oracle-triplet': 'prepare_triplet_oracle',
}
REGISTRATIONS = {
    'job0-capture': 'spu_begin_image(30); spu_0029_at_00EC9A80_spu_recomp_register();',
    'job0-kernel': 'spu_begin_image(30); spu_0029_at_00EC9A80_spu_recomp_register();',
    'native-cursor': 'spu_begin_image(40); spu_0039_at_00F1C100_spu_recomp_register();',
    'spu-idle': 'spu_begin_image(29); spu_0028_at_00EC7980_spu_recomp_register();',
    'spu-triplet-copy': 'spu_begin_image(42); spu_0041_at_00F2E380_spu_recomp_register();',
}
LATIN1_PATCHES = {'audio-tick', 'menu-lifetime', 'menu-material', 'menu-pages',
                 'movie-poll', 'save-status'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=(*PATCHES, *ORACLES))
    parser.add_argument('arguments', nargs=argparse.REMAINDER, help=argparse.SUPPRESS)
    command = parser.parse_args()
    name = command.command
    if name in ORACLES:
        sys.argv = [f'{parser.prog} {name}', *command.arguments]
        runpy.run_module('patches.' + ORACLES[name], run_name='__main__')
        return

    module = importlib.import_module('patches.patch_' + name.replace('-', '_'))
    arguments = argparse.ArgumentParser(prog=f'{parser.prog} {name}', description=module.__doc__)
    arguments.add_argument('source', type=Path)
    if name == 'savedata':
        arguments.add_argument('port', type=Path)
    arguments.add_argument('output', type=Path)
    if name in REGISTRATIONS:
        arguments.add_argument('--registration', type=Path, required=True)
    if name == 'bulk-fill':
        arguments.add_argument('--caller-source', type=Path, required=True)
    if name == 'slot-copy':
        arguments.add_argument('--memmove-source', type=Path, required=True)
    args = arguments.parse_args(command.arguments)
    if name in REGISTRATIONS and REGISTRATIONS[name] not in args.registration.read_text():
        raise ValueError(f'{name}: runtime image registration changed; re-audit before patching')

    # Preserve each transform's original source decoding and output encoding.
    # Latin-1 paths intentionally preserve arbitrary bytes from the private lift.
    encoding = 'cp1252' if name == 'producer-profile' else None
    if name == 'savedata':
        result = module.patch(args.source, args.port)
    else:
        source = (args.source.read_bytes().replace(b'\r\n', b'\n').decode('latin1')
                  if name in LATIN1_PATCHES else args.source.read_text(encoding=encoding))
        extra = []
        if name == 'bulk-fill':
            extra.append(args.caller_source.read_text())
        elif name == 'slot-copy':
            extra.append(args.memmove_source.read_text())
        result = module.patch(source, *extra)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if name in LATIN1_PATCHES:
        args.output.write_bytes(result.encode('latin1'))
    elif name == 'savedata':
        if not args.output.exists() or args.output.read_text(encoding='utf-8') != result:
            args.output.write_text(result, encoding='utf-8', newline='\n')
    elif name != 'menu-input' or not args.output.exists() or args.output.read_text() != result:
        args.output.write_text(result, encoding=encoding)


if __name__ == '__main__':
    main()
