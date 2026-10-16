import unittest

from check_profile_configuration import check_rules


def rule(target, source, defines=''):
    define_flags = ' '.join(f'-D{define}' for define in defines.split())
    return f'build C:/out/{source}.obj: CXX_COMPILER__{target}_ {source}\n  DEFINES = {define_flags}'


def synthetic_ninja(renderer, jobs):
    sdk_defs = 'YZ_PERF_PROFILE' if renderer else ''
    workload_defs = 'YZ_PERF_PROFILE' if (renderer or jobs) else ''
    title_job_defs = 'HAZE_JOB_PROFILE HAZE_JOB_TRACE' if (renderer or jobs) else ''
    blocks = []
    for source in ('ppu_loader.cpp', 'ppu_hle.cpp'):
        blocks.append(rule('HazeRecomp', source, sdk_defs))
    for source in ('spu_workload.c', 'cellSpursQueue.c', 'rsx_live_draw.c', 'cellGcmSys.c'):
        defines = workload_defs if source == 'spu_workload.c' else sdk_defs
        blocks.append(rule('ps3recomp_runtime', source, defines))
    title_sources = ('main.cpp', 'haze_job_profile.cpp', 'ppu_recomp_003.cpp',
                     'haze_job_trace.cpp', 'haze_job0_capture.cpp',
                     r'haze_job0_capture\spu_recomp.c')
    for source in title_sources:
        blocks.append(rule('HazeRecomp', source, title_job_defs))
    return '\n\n'.join(blocks)


class ProfileConfigurationTests(unittest.TestCase):
    def test_independent_renderer_and_job_gates(self):
        for renderer in (False, True):
            for jobs in (False, True):
                with self.subTest(renderer=renderer, jobs=jobs):
                    check_rules(synthetic_ninja(renderer, jobs), renderer, jobs)

    def test_jobs_only_rejects_broad_renderer_instrumentation(self):
        ninja = synthetic_ninja(False, True).replace(
            'cellSpursQueue.c.obj: CXX_COMPILER__ps3recomp_runtime_ cellSpursQueue.c\n  DEFINES = ',
            'cellSpursQueue.c.obj: CXX_COMPILER__ps3recomp_runtime_ cellSpursQueue.c\n  DEFINES = -DYZ_PERF_PROFILE')
        with self.assertRaisesRegex(ValueError, 'cellSpursQueue.c'):
            check_rules(ninja, False, True)

    def test_jobs_only_requires_title_trace_hooks(self):
        ninja = synthetic_ninja(False, True).replace(
            'haze_job_trace.cpp.obj: CXX_COMPILER__HazeRecomp_ haze_job_trace.cpp\n  DEFINES = -DHAZE_JOB_PROFILE -DHAZE_JOB_TRACE',
            'haze_job_trace.cpp.obj: CXX_COMPILER__HazeRecomp_ haze_job_trace.cpp\n  DEFINES = -DHAZE_JOB_PROFILE')
        with self.assertRaisesRegex(ValueError, 'job trace compile gate'):
            check_rules(ninja, False, True)

    def test_hot_modules_inline_reads_in_ordinary_builds(self):
        def inline_ninja(inlined):
            blocks = synthetic_ninja(False, False).split('\n\n')
            blocks += [rule('HazeRecomp', source) for source in
                       ('ppu_recomp_000.cpp', 'ppu_recomp_001.cpp', 'ppu_recomp_002.cpp',
                        'ppu_recomp_004.cpp', 'ppu_recomp_005.cpp', 'ppu_recomp_006.cpp',
                        'ppu_hle_nids.cpp')]
            return '\n\n'.join(
                block + '\n  FLAGS = /FIport/haze_ppu_inline_reads.h'
                if any(f'{source}.obj:' in block.splitlines()[0] for source in inlined) else block
                for block in blocks)
        hot = ('ppu_recomp_003.cpp', 'ppu_recomp_005.cpp')
        check_rules(inline_ninja(hot), False, False, False)
        everything = hot + ('ppu_recomp_000.cpp', 'ppu_recomp_001.cpp', 'ppu_recomp_002.cpp',
                            'ppu_recomp_004.cpp', 'ppu_recomp_006.cpp', 'ppu_hle_nids.cpp')
        check_rules(inline_ninja(everything), False, False, True)
        with self.assertRaisesRegex(ValueError, 'ppu_recomp_005'):
            check_rules(inline_ninja(('ppu_recomp_003.cpp',)), False, False, False)
        with self.assertRaisesRegex(ValueError, 'ppu_recomp_000'):
            check_rules(inline_ninja(hot + ('ppu_recomp_000.cpp',)), False, False, False)


if __name__ == '__main__':
    unittest.main()
