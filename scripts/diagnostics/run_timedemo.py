"""Run one frame-locked benchmark session on a committed benchmark scene.

Launches the game on a private copy of the scene's checkpoint save, loads it
from the main menu, runs the frame-scheduled camera sweep on the virtual
benchmark clock (docs/benchmark-clock.md), closes the game and keeps the
reports under local/timedemo/out/STEM-*. Compare runs with `diagnose.py
timedemo` or `diagnose.py timedemo-ab`.

Each scene archive in config/benchmarks/scenes holds only the checkpoint's
save state. The save's icons and sound come from the local disc, and the
first run of a scene records a shader cache under local/timedemo/cache so
measured runs do not compile shaders. Needs a prepared checkout and an
interactive desktop; the game window must keep focus.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[2]
SCENES = ROOT/'config'/'benchmarks'/'scenes'
PROFILE = 'BLUS30094PROFILEE0080007000'
STATE_FILES = ('PARAM.SFO', '_PROFILE.DAT', 'CHCKPNTS.SAV')
DISC_FILES = ('ICON0.PNG', 'ICON1.PAM', 'PIC1.PNG', 'SND0.AT3')
SCENE_NOTES = {
    'later': 'later combat, the default',
    'deck': 'the campaign\'s opening carrier deck',
    'mine': 'desert mine, vehicle start',
}


class NavigationError(RuntimeError):
    """The wall-clock menu presses did not reach gameplay (occasional)."""


def scenes():
    return sorted(path.name[:-len('.tar.xz')] for path in SCENES.glob('*.tar.xz'))


def unpack_scene(scene, disc, target):
    """Write the scene's save into target/PROFILE with the disc's icons."""
    with tarfile.open(SCENES/f'{scene}.tar.xz') as archive:
        members = archive.getmembers()
        names = sorted(member.name for member in members)
        if names != sorted(f'{PROFILE}/{name}' for name in STATE_FILES) or not all(m.isfile() for m in members):
            raise ValueError(f'{scene}: unexpected scene archive contents {names}')
        profile = Path(target)/PROFILE
        profile.mkdir(parents=True)
        for member in members:
            (profile/Path(member.name).name).write_bytes(archive.extractfile(member).read())
    for name in DISC_FILES:
        shutil.copyfile(Path(disc)/'PS3_GAME'/name, profile/name)


def benchmark_input(source, autoskip):
    """Controller input with the sticks unbound; startup screens skip."""
    text = Path(source).read_text()
    if autoskip:
        text = text.replace('AutoSkipLogos=0', 'AutoSkipLogos=1').replace('AutoSkipAttract=0', 'AutoSkipAttract=1')
    for axis, stick in (('MoveX', 'leftx'), ('MoveY', 'lefty'), ('LookX', 'rightx'), ('LookY', 'righty')):
        text = text.replace(f'{axis}={stick}', f'{axis}=None')
    return text


def close_window(pid):
    """Post WM_CLOSE to the game's window so it writes its reports."""
    from ctypes import wintypes
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        owner = wintypes.DWORD(); user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        title = ctypes.create_unicode_buffer(256); user32.GetWindowTextW(hwnd, title, 256)
        if owner.value == pid and title.value.startswith('Haze Recomp'):
            user32.PostMessageW(hwnd, 0x10, 0, 0)
        return True
    user32.EnumWindows(visit, 0)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('stem', help='output name; local/timedemo/out/STEM-* must not exist yet')
    parser.add_argument('--scene', default='later', help='scene archive name: ' + ', '.join(
        f'{name} ({SCENE_NOTES.get(name, "")})' for name in scenes()))
    parser.add_argument('--exe', type=Path, default=ROOT/'build'/'haze'/'HazeRecomp.exe')
    parser.add_argument('--shared-root', type=Path, default=ROOT,
                        help='prepared checkout with game/disc and local/tools/rpcs3/dev_flash')
    parser.add_argument('--clock-us', type=int, default=6944, help='virtual microseconds per frame')
    parser.add_argument('--settle', type=int, default=600, help='gameplay frames before the sweep')
    parser.add_argument('--sweep', type=int, default=1440, help='camera sweep frames')
    parser.add_argument('--cache', type=Path, help='shader cache seed (default: the scene\'s recorded cache)')
    parser.add_argument('--timeout', type=float, default=900)
    parser.add_argument('--job-trace', action='store_true', help='requires a -ProfileJobs build')
    parser.add_argument('--producer-profile', action='store_true')
    parser.add_argument('--sample', type=float, help='native-sample the producer for this many seconds from sweep start')
    parser.add_argument('--sample-all', action='store_true', help='sample every thread')
    parser.add_argument('--keep-intro', action='store_true', help='do not auto-skip logos and attract')
    parser.add_argument('--attempts', type=int, default=2, help='sessions to try when menu navigation misses')
    parser.add_argument('--env', action='append', default=[], metavar='KEY=VALUE',
                        help='extra environment for a diagnostic build (repeatable)')
    args = parser.parse_args(argv)
    if args.scene not in scenes():
        parser.error(f'unknown scene {args.scene}; choose from {", ".join(scenes())}')
    return args


def run(args):
    out = ROOT/'local'/'timedemo'/'out'; out.mkdir(parents=True, exist_ok=True)
    prefix = out/args.stem
    private = ROOT/'local'/'timedemo'/'private'/args.stem
    workdir = ROOT/'local'/'timedemo'/'runs'/args.stem
    if private.exists() or workdir.exists() or Path(f'{prefix}-meta.json').exists():
        raise SystemExit(f'stem {args.stem} already used')
    assets = args.shared_root.resolve()
    disc = assets/'game'/'disc'
    firmware = os.environ.get('PS3_DEV_FLASH') or str(assets/'local'/'tools'/'rpcs3'/'dev_flash')
    exe = args.exe.resolve()
    if not exe.exists() or not (disc/'PS3_GAME'/'USRDIR'/'EBOOT.ELF').exists():
        raise SystemExit('Needs a built game and a prepared checkout (see README.md).')
    cache_seed = args.cache or ROOT/'local'/'timedemo'/'cache'/args.scene
    warming = args.cache is None and not cache_seed.exists()
    if warming:
        print(f'no recorded shader cache for {args.scene}; this run records one and is not a measurement', flush=True)

    private.mkdir(parents=True); workdir.mkdir(parents=True)
    unpack_scene(args.scene, disc, private/'savedata')
    if warming:
        (private/'shader-cache').mkdir()
    else:
        shutil.copytree(cache_seed, private/'shader-cache')
    # The game writes its installed data; each run starts from the disc copy.
    shutil.copytree(disc/'game', private/'hdd0')
    Path(f'{prefix}-input.ini').write_text(benchmark_input(ROOT/'config'/'input.ini', not args.keep_intro))
    timeline = Path(f'{prefix}-timeline.csv')
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('HAZE_', 'RSX_', 'SPU_', 'GCM_', 'PS3_', 'PAD_', 'LD_FRAME_', 'CELLMARK_'))}
    env.update(PS3_VFS_ROOT=str(disc), PS3_DEV_FLASH=firmware, PS3_NET_ONLINE='1', YDKJ_NO_CRI_CTX='1',
               PS3_SAVEDATA_ROOT=str(private/'savedata'), PS3_HDD0_ROOT=str(private/'hdd0'),
               PS3_HLE_TRACE='0', PS3_HLE_UNRESOLVED='fail', PS3_VERBOSE='0', AUDIO_MUTE='1',
               HAZE_INPUT='controller', RSX_LIVE_DRAW='1', RSX_SHADER_CACHE_DIR=str(private/'shader-cache'),
               HAZE_INPUT_CONFIG=f'{prefix}-input.ini',
               HAZE_GRAPHICS_CONFIG=str(ROOT/'config'/'benchmarks'/'timedemo.ini'),
               PAD_FILE=f'{prefix}-pad.txt', PAD_NO_KEYBOARD='1', RSX_PRESENT_CSV=f'{prefix}-presents.csv',
               HAZE_BENCHMARK_CAMERA_FILE=f'{prefix}-camera.txt', RSX_FRAME_FINGERPRINT_CSV=f'{prefix}-fingerprint.csv',
               HAZE_BENCHMARK_CLOCK_US=str(args.clock_us),
               HAZE_BENCHMARK_TIMELINE=f'{args.settle},{args.sweep},{timeline}')
    for item in args.env:
        key, separator, value = item.partition('=')
        if not separator or not key:
            raise SystemExit(f'--env expects KEY=VALUE, got {item!r}')
        env[key] = value
    if args.producer_profile:
        env['HAZE_PRODUCER_PROFILE_CSV'] = f'{prefix}-producer.csv'
    if args.job_trace:
        env.update(HAZE_JOB_TRACE_CSV=f'{prefix}-jobs.csv', HAZE_JOB_PROFILE_CSV=f'{prefix}-job-profile.csv')
    Path(env['PAD_FILE']).write_text('0 0\n'); Path(env['HAZE_BENCHMARK_CAMERA_FILE']).write_text('0\n')
    Path(f'{prefix}-meta.json').write_text(json.dumps(dict(
        stem=args.stem, scene=args.scene, exe=str(exe), clock_us=args.clock_us, settle=args.settle,
        sweep=args.sweep, cache='recording' if warming else str(cache_seed), started=time.time()), indent=2))

    presses = []

    def qpc():
        value = ctypes.c_longlong(); ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(value))
        return value.value

    def pad(value, polls=100):
        presses.append((qpc(), value, polls))
        Path(env['PAD_FILE']).write_text(f'{value} {polls}\n'); time.sleep(.25)

    log, stdout = workdir/'boot.stderr.log', workdir/'boot.stdout.log'
    # The producer is the process's initial thread; keep its id for sampling.
    import _winapi
    create_process = _winapi.CreateProcess; initial = {}

    def remember_initial_thread(*values, **keywords):
        created = create_process(*values, **keywords); initial['tid'] = created[3]; return created
    _winapi.CreateProcess = remember_initial_thread
    try:
        child = subprocess.Popen([str(exe), str(disc/'PS3_GAME'/'USRDIR'/'EBOOT.ELF')], cwd=workdir, env=env,
                                 stdout=stdout.open('wb'), stderr=log.open('wb'),
                                 creationflags=subprocess.CREATE_NO_WINDOW)
    finally:
        _winapi.CreateProcess = create_process
    print('pid', child.pid, flush=True)
    sampler = None
    status = 'incomplete'
    try:
        def text():
            return log.read_text(errors='replace')

        def wait(seconds):
            end = time.monotonic()+seconds
            while time.monotonic() < end:
                if child.poll() is not None:
                    raise RuntimeError(f'game exited {child.returncode}')
                time.sleep(.1)

        # Menu navigation is wall-clock driven. The idle attract demo starts
        # after the logos and is skipped; a press during that transition is
        # lost, so press START once the title returns after the first attract.
        # Then: Single Player, Continue Campaign, confirm the load.
        deadline = time.monotonic()+150
        while 'attract stop at' not in text():
            if time.monotonic() > deadline:
                raise NavigationError('attract demo never ran')
            wait(.2)
        wait(5); pad('0x8', 1000); wait(30)
        for _ in range(3):
            pad('0x4000', 1000); wait(4)
        wait(2)
        pad('0x10'); wait(1); pad('0x4000'); wait(.5)
        Path(f'{prefix}-presses.json').write_text(json.dumps(presses))
        deadline = time.monotonic()+120
        while '[haze-native-cursor-mode] enabled=1 ' not in text():
            if time.monotonic() > deadline:
                raise NavigationError('No initial gameplay camera')
            wait(.2)
        sys.path.insert(0, str(ROOT/'scripts'/'diagnostics'))
        from detach_pad_file import detach
        detach(Path(env['PAD_FILE']), stdout, 30)
        print('gameplay reached; waiting for the timeline', flush=True)
        if args.sample:
            # scripts/build_profiler.ps1 builds the stack walker in a checkout's local/.
            stack_library = next((path for path in (ROOT/'local'/'native_stackwalk.dll',
                                                    assets/'local'/'native_stackwalk.dll') if path.exists()),
                                 ROOT/'local'/'native_stackwalk.dll')
            while '[benchmark-camera-mode] token=1 ' not in text():
                if time.monotonic() > deadline+args.timeout:
                    raise RuntimeError('sweep never started')
                wait(.2)
            sampler = subprocess.Popen(
                [sys.executable, str(ROOT/'scripts'/'diagnose.py'), 'native-cpu', str(child.pid), str(args.sample),
                 *(['--skip-idle'] if args.sample_all else ['--tid', str(initial['tid'])]),
                 '--scan-every', '1' if args.sample_all else '1000', '--stack-depth', '16' if args.sample_all else '24',
                 # All-thread rounds suspend dozens of threads; unwinding each
                 # one every round would cut the round rate several-fold.
                 '--stack-every', '10' if args.sample_all else '1',
                 '--interval-ms', '2' if args.sample_all else '7', '--stack-limit', '2000', '--raw-limit', '400',
                 '--symbol-path', str(exe.parent), '--stack-library', str(stack_library)],
                stdout=Path(f'{prefix}-samples.json').open('wb'), stderr=Path(f'{prefix}-sampler.err').open('wb'),
                creationflags=subprocess.CREATE_NO_WINDOW)
        done = Path(f'{timeline}.done'); deadline = time.monotonic()+args.timeout
        while not done.exists():
            if time.monotonic() > deadline:
                raise RuntimeError('timeline did not finish')
            wait(.5)
        status = 'complete'
    finally:
        if sampler:
            try:
                sampler.wait(120)
            except subprocess.TimeoutExpired:
                sampler.kill()
        if child.poll() is None:
            close_window(child.pid)
            try:
                child.wait(30)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait(10)
        for name in ('boot.stderr.log', 'boot.stdout.log'):
            if (workdir/name).exists():
                shutil.copy2(workdir/name, f'{prefix}-{name}')
        if warming and status == 'complete':
            cache_seed.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(private/'shader-cache', cache_seed)
            print(f'recorded shader cache {cache_seed}', flush=True)
        base = (ROOT/'local'/'timedemo').resolve()
        for folder in (private, workdir):
            target = folder.resolve()
            if not target.is_relative_to(base) or folder.is_symlink():
                raise RuntimeError(f'refusing to delete {target}')
            shutil.rmtree(target)
        print('status', status, flush=True)
    return status


def main(argv=None):
    args = parse_args(argv)
    out = ROOT/'local'/'timedemo'/'out'
    for attempt in range(1, args.attempts+1):
        before = set(out.iterdir()) if out.exists() else set()
        try:
            return 0 if run(args) == 'complete' else 1
        except NavigationError as error:
            if attempt == args.attempts:
                raise
            print(f'{error}; retrying ({attempt}/{args.attempts})', flush=True)
            # Only this attempt's reports; other stems may share the prefix.
            for path in set(out.iterdir()) - before:
                if path.name.startswith(f'{args.stem}-'):
                    path.unlink()


if __name__ == '__main__':
    sys.exit(main())
