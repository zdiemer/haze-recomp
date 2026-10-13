"""The native EOF bridge must reject a different guest function."""
import argparse
import importlib.util
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('movie_patch', root / 'scripts/patch_movie_poll.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
args = parser.parse_args()
source = args.source.read_bytes().replace(b'\r\n', b'\n').decode('latin1')
patched = module.patch(source)
assert patched.count('haze_movie_guest_poll(ctx);') == 1
assert source[:source.index(module.ENTRY)] == patched[:patched.index(module.ENTRY)]
start = source.index(module.ENTRY)
for invalid in (source.replace(module.ENTRY, '// missing'), patched,
                source[:start] + source[start:].replace('-0x7FB0', '-0x7FB4', 1)):
    try:
        module.patch(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('Changed movie poll was accepted')
print('Movie guest EOF bridge and source mutation guards passed.')
