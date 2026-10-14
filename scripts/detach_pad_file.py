"""End PAD_FILE navigation and require native acknowledgement before timing.

Use only after releasing navigation buttons and visually confirming gameplay.
The SDK's detach command permanently disables this diagnostic input source for
the running process. Normal controller and keyboard/mouse input remain active.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import time


def detach(pad_file: Path, boot_log: Path, timeout: float) -> None:
    if not pad_file.is_file() or not boot_log.is_file():
        raise ValueError('Existing navigation file and owned run boot log are required')
    # Ignore old acknowledgements; this call must observe its own transition.
    offset = boot_log.stat().st_size
    deadline = time.monotonic() + timeout
    while True:
        try:
            pad_file.write_text('detach\n', encoding='ascii')
            break
        except PermissionError:
            if time.monotonic() >= deadline:
                raise TimeoutError('Could not send PAD_FILE detach command')
            time.sleep(.02)
    tail = b''
    with boot_log.open('rb') as stream:
        stream.seek(offset)
        while time.monotonic() < deadline:
            tail = (tail + stream.read())[-8192:]
            if b'[cellPad] PAD_FILE detached' in tail:
                print('Native PAD_FILE detach acknowledged; diagnostic input polling is off.')
                return
            time.sleep(.05)
    raise TimeoutError('No native PAD_FILE detach acknowledgement; do not start timing')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pad_file', type=Path)
    parser.add_argument('boot_log', type=Path)
    parser.add_argument('--timeout', type=float, default=30)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 120:
        parser.error('timeout must be 1..120 seconds')
    detach(args.pad_file, args.boot_log, args.timeout)


if __name__ == '__main__':
    main()
