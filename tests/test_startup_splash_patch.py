"""Run the actual patched splash update, including its texture readiness gate."""
import argparse
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'scripts'))
from patch_startup_splash import patch, DIGESTS
from patch_movie_poll import patch as movie_patch

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('compiler')
parser.add_argument('sdk', type=Path)
args = parser.parse_args()
source = args.source.read_text()
modified = patch(source)
assert patch(movie_patch(source)) == movie_patch(modified)
assert modified.count('if (!haze_skip &&') == 3
assert modified.count('haze_startup_splash_end(haze_scene);') == 1
for name in DIGESTS:
    entry = f'void func_{name}(ppu_context* ctx) {{'
    for invalid in (modified, source.replace(entry, '// absent'), source + '\n' + entry,
                    source.replace(entry, entry + '\n        ctx->gpr[3]=0;', 1)):
        try:
            patch(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('Changed splash function was accepted')

entry = 'void func_006407C0(ppu_context* ctx) {'
begin = modified.index(entry)
end = modified.index('\nvoid ', begin + len(entry))
output = root / 'local/startup-test'
output.mkdir(parents=True, exist_ok=True)
# Use the lift's own register, floating-point, rotate, and trampoline helpers.
(output / 'haze_splash_lift.inc').write_text(source[:source.index('\nvoid func_')] + modified[begin:end])
subprocess.run([args.compiler, '/nologo', '/std:c++20', '/EHsc', '/W3',
                '/I' + str(args.source.parent), '/I' + str(output),
                '/I' + str(args.sdk / 'include'), '/I' + str(args.sdk / 'libs/input'),
                str(root / 'tests/test_haze_startup_guest.cpp'), '/Fe:test.exe', '/Fo:test.obj'],
               cwd=output, check=True)
subprocess.run([str(output / 'test.exe')], cwd=output, check=True)
print('Startup splash mutation guards, movie patch composition, and guest transition tests passed.')
