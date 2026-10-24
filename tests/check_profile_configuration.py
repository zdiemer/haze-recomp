"""Check profiling flags on the actual Ninja compile rules before compilation.

Source properties in the SDK directory do not apply to PPU files compiled by
the game target. Inspect both targets so silent instrumentation loss is
reported immediately, and ordinary builds retain no profiling definitions.
"""
import argparse
from pathlib import Path
import re


def check_rules(ninja, renderer_enabled, jobs_enabled=False, inline_reads=None, pgo_generate=False):
    if inline_reads is not None:
        generated = 0
        for block in ninja.split('\n\n'):
            if not block.startswith('build ') or '__HazeRecomp_' not in block.splitlines()[0]:
                continue
            first = block.splitlines()[0]
            is_generated = bool(re.search(r'[\\/]ppu_(?:recomp_\d+|hle_nids)\.cpp\.obj:', first))
            if is_generated:
                generated += 1
            # HAZE_INLINE_HOT_READS inlines the two measured hot modules in
            # ordinary builds; the experiment extends it to every module.
            is_hot = bool(re.search(r'[\\/]ppu_recomp_00[35]\.cpp\.obj:', first))
            if ('haze_ppu_inline_reads.h' in block) != (is_generated and (inline_reads or is_hot)):
                raise ValueError(f'Generated-only inline-read compile gate mismatch: {first}')
            # The committed gameplay profile applies to every lifted module and
            # never to runtime code; only a -PgoGenerate build is instrumented.
            if pgo_generate:
                if is_generated and '-fprofile-instr-generate' not in block:
                    raise ValueError(f'Lifted module is not PGO-instrumented: {first}')
            elif '-fprofile-instr-generate' in block:
                raise ValueError(f'PGO instrumentation in an ordinary build: {first}')
            elif is_generated and '-fprofile-instr-use=' not in block:
                raise ValueError(f'Lifted module built without the PGO profile: {first}')
            if re.search(r'[\\/]main\.cpp\.obj:', first) and '-fprofile-instr-use=' in block:
                raise ValueError(f'PGO profile applied to runtime code: {first}')
        if generated != 8:
            raise ValueError(f'Expected 8 lifted Haze compile rules, got {generated}')
        print(f'Actual compile rules: inline reads={int(inline_reads)} on generated sources only (hot modules always)')
        print(f"Actual compile rules: PGO {'instrumentation' if pgo_generate else 'profile'} on every lifted module")

    sdk_sources = {
        'ppu_loader.cpp': 'HazeRecomp',
        'ppu_hle.cpp': 'HazeRecomp',
        'spu_workload.c': 'ps3recomp_runtime',
        'cellSpursQueue.c': 'ps3recomp_runtime',
        ('haze_video_backend.c' if 'haze_video_backend.c.obj:' in ninja else 'rsx_live_draw.c'): 'ps3recomp_runtime',
        'cellGcmSys.c': 'ps3recomp_runtime',
    }
    for source, target in sdk_sources.items():
        blocks = [block for block in ninja.split('\n\n')
                  if block.startswith('build ')
                  and re.search(r'[\\/]' + re.escape(source) + r'\.obj:', block.splitlines()[0])
                  and f'__{target}_' in block.splitlines()[0]]
        if len(blocks) != 1:
            raise ValueError(f'Expected one {target} compile rule for {source}, found {len(blocks)}')
        defines = next((line for line in blocks[0].splitlines()
                        if line.startswith('  DEFINES = ')), '')
        profiled = bool(re.search(r'(?:^|\s)(?:-D|/D)YZ_PERF_PROFILE(?:=\S+)?(?:\s|$)', defines))
        expected = (renderer_enabled or jobs_enabled) if source == 'spu_workload.c' else renderer_enabled
        if profiled != expected:
            raise ValueError(f'{target}/{source}: profiling flag is {profiled}, expected {expected}')
    print(f'Actual compile rules: renderer={int(renderer_enabled)} jobs={int(jobs_enabled)} verified on SDK sources')

    for source in ('main.cpp', 'haze_job_profile.cpp', 'ppu_recomp_003.cpp'):
        blocks = [block for block in ninja.split('\n\n') if block.startswith('build ')
                  and re.search(r'[\\/]' + re.escape(source) + r'\.obj:', block.splitlines()[0])
                  and '__HazeRecomp_' in block.splitlines()[0]]
        if len(blocks) != 1:
            raise ValueError(f'Expected one completion-profile rule for {source}')
        defines = next((line for line in blocks[0].splitlines() if line.startswith('  DEFINES = ')), '')
        if bool(re.search(r'(?:^|\s)(?:-D|/D)HAZE_JOB_PROFILE(?:\s|$)', defines)) != (renderer_enabled or jobs_enabled):
            raise ValueError(f'{source}: completion profiler compile gate mismatch')
    for source in ('main.cpp', 'haze_job_trace.cpp', 'haze_job0_capture.cpp',
                   r'haze_job0_capture\spu_recomp.c', 'ppu_recomp_003.cpp'):
        blocks = [block for block in ninja.split('\n\n') if block.startswith('build ')
                  and re.search(r'[\\/]' + re.escape(source) + r'\.obj:', block.splitlines()[0])
                  and '__HazeRecomp_' in block.splitlines()[0]]
        if len(blocks) != 1:
            raise ValueError(f'Expected one job-trace rule for {source}')
        defines = next((line for line in blocks[0].splitlines() if line.startswith('  DEFINES = ')), '')
        if bool(re.search(r'(?:^|\s)(?:-D|/D)HAZE_JOB_TRACE(?:\s|$)', defines)) != (renderer_enabled or jobs_enabled):
            raise ValueError(f'{source}: job trace compile gate mismatch')


def check(build, enabled, inline_reads=None, jobs_enabled=False, pgo_generate=False):
    check_rules((Path(build) / 'build.ninja').read_text(), enabled, jobs_enabled, inline_reads, pgo_generate)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--enabled', type=int, choices=(0, 1), required=True,
                        help='ProfileRenderer compile gate')
    parser.add_argument('--jobs-enabled', type=int, choices=(0, 1), default=0,
                        help='ProfileJobs compile gate')
    parser.add_argument('--inline-reads', type=int, choices=(0, 1))
    parser.add_argument('--pgo-generate', type=int, choices=(0, 1), default=0,
                        help='HAZE_PGO_GENERATE instrumented build')
    args = parser.parse_args()
    check(args.build, bool(args.enabled), None if args.inline_reads is None else bool(args.inline_reads),
          bool(args.jobs_enabled), bool(args.pgo_generate))
