"""Bridge Haze's frame-pointer based movie EOF test to native playback."""
import argparse
import hashlib
from pathlib import Path

ENTRY = 'void func_00718880(ppu_context* ctx) {'
DIGEST = 'e2829d9898ca4ce38228e330590588873d771ab34c255087d1286a4beacfd3c4'

def patch(source):
    if source.count(ENTRY) != 1:
        raise ValueError('Expected one Haze movie frame poll')
    start = source.index(ENTRY)
    end = source.index('\nvoid func_', start + len(ENTRY))
    if hashlib.sha256(source[start:end].encode('latin1')).hexdigest() != DIGEST:
        raise ValueError('Haze movie frame poll changed; audit native EOF bridge')
    replacement = ENTRY + '''
        extern void haze_movie_guest_poll(ppu_context*);
        haze_movie_guest_poll(ctx);
}

'''
    return source[:start] + replacement + source[end:]

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = patch(args.source.read_bytes().replace(b'\r\n', b'\n').decode('latin1'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result.encode('latin1'))
