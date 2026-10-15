"""Build a labeled private replay variant and validate captured fixtures.

Requires a VS x64 environment. Generated game operations and captures stay in
the supplied private output directory, never in source commits.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from patch_job13_capture import validate


def artifact(path):
    return dict(path=str(path.resolve()), sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,required=True)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--registration',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True)
    p.add_argument('--compiler',required=True)
    p.add_argument('--fixture',type=Path,action='append',default=[])
    mode=p.add_mutually_exclusive_group()
    mode.add_argument('--native-divide',action='store_true')
    mode.add_argument('--division-property',action='store_true')
    mode.add_argument('--native-math',action='store_true')
    mode.add_argument('--math-shadow',action='store_true')
    mode.add_argument('--math-property',action='store_true')
    p.add_argument('--fused-records',action='store_true',help='Fuse the audited record loop; may compose with native divide')
    a=p.parse_args();out=a.output_dir.resolve();sdk=a.sdk.resolve()
    property_test=a.division_property or a.math_property
    math_mode=a.native_math or a.math_shadow or a.math_property
    if property_test and not a.fixture:
        p.error('Property tests require --fixture to load original image constants')
    if (property_test or math_mode) and a.fused_records:
        p.error('--fused-records cannot be used with property or native-math modes')
    variant='division-property' if a.division_property else 'native-divide' if a.native_divide else 'original'
    if a.fused_records:
        variant='native-divide+fused-records' if a.native_divide else 'fused-records'
    if math_mode:
        variant='native-math-semantic' if a.native_math else 'native-math-shadow' if a.math_shadow else 'native-math-property'
    validate(a.source.read_text(),a.registration.read_text())
    out.mkdir(parents=True,exist_ok=False)
    code=a.source.read_text()
    if a.native_math or a.math_shadow:
        from patch_job13_native_math import patch
        code=patch(code,a.registration.read_text(),execute_native=a.native_math,diagnostics=a.math_shadow)
    elif a.fused_records:
        from patch_job13_record_loop import patch
        code=patch(code,a.registration.read_text(),native_divide=a.native_divide)
    elif a.native_divide:
        from patch_job13_native_divide import patch
        code=patch(code,a.registration.read_text())
    original=out/'spu_recomp.c';original.write_text(code)
    shutil.copy2(a.source.with_suffix('.h'),out/'spu_recomp.h')
    exe=out/'replay_job13_capture.exe'
    harness=('test_job13_native_math_semantic.c' if a.native_math else
             'test_job13_native_math_replay.c' if a.math_shadow else
             'test_job13_native_math.c' if a.math_property else
             'test_job13_native_divide.c' if a.division_property else 'replay_job13_capture.c')
    cmd=[a.compiler,'/nologo','/TC','/std:c17','/O2','/MD','/DSPU_FLOAT_NATIVE_RUNTIME=1',
        '/DHAZE_JOB13_REPLAY_VARIANT="'+variant+'"',
        '/DSPU_USE_DISPATCH_MT','/D_CRT_SECURE_NO_WARNINGS','/clang:-msse4.1',
        '/clang:-fno-strict-aliasing','/I'+str(sdk/'runtime/spu'),
        '/I'+str(ROOT/'port'),'/I'+str(out),str(ROOT/'tests'/harness),
        str(original),str(sdk/'runtime/spu/spu_float_native.c'),'/Fe'+str(exe)]
    result=subprocess.run(cmd,cwd=out,capture_output=True,text=True)
    (out/'compile.log').write_text(result.stdout+result.stderr)
    print(result.stdout+result.stderr,end='')
    identities=dict(input_source=artifact(a.source),compiled_source=artifact(original),
                    registration=artifact(a.registration),generated_header=artifact(out/'spu_recomp.h'),
                    capture_header=artifact(ROOT/'port/haze_job13_capture.h'),
                    replay_harness=artifact(ROOT/'tests/replay_job13_capture.c'))
    if a.native_divide or a.division_property:
        identities['native_divide_header']=artifact(ROOT/'port/haze_job13_native_divide.h')
        identities['native_divide_patch']=artifact(ROOT/'scripts/patch_job13_native_divide.py')
    if a.fused_records:
        identities['record_loop_patch']=artifact(ROOT/'scripts/patch_job13_record_loop.py')
    if a.division_property:
        identities['property_harness']=artifact(ROOT/'tests/test_job13_native_divide.c')
    if math_mode:
        identities['native_math_header']=artifact(ROOT/'port/haze_job13_native_math.h')
        identities['native_math_patch']=artifact(ROOT/'scripts/patch_job13_native_math.py')
        identities['selected_harness']=artifact(ROOT/'tests'/harness)
    report=dict(variant=variant,compile_command=cmd,compile_exit_code=result.returncode,
                artifacts=identities,fixtures=[])
    if result.returncode:
        report['status']='compile-failed'
        (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        return result.returncode
    identities['binary']=artifact(exe)
    env=dict(os.environ)
    for name in ('SPU_FLOAT_NATIVE_VALIDATE','SPU_FLOAT_NATIVE_FILE'):
        env.pop(name,None)
    tests=report['fixtures']
    # Both entry points require a capture. Property mode executes its distinct
    # main against each fixture's constants; it is not a whole-batch replay.
    for f in a.fixture:
        result=subprocess.run([str(exe),str(f.resolve())],env=env,capture_output=True,text=True,timeout=30)
        tests.append(dict(variant=variant,kind='scalar-property' if property_test else 'observable-ABI-replay' if a.native_math else 'whole-batch-replay',
                          path=str(f.resolve()),sha256=hashlib.sha256(f.read_bytes()).hexdigest(),
                          exit_code=result.returncode,output=result.stdout+result.stderr))
        print(result.stdout+result.stderr,end='')
    report['status']='build-only' if not tests else 'failed' if any(t['exit_code'] for t in tests) else 'passed'
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    if any(t['exit_code'] for t in tests):return 1
    if not tests:
        print(f'BUILD ONLY variant={variant} binary={exe}; no replay executed')
    elif property_test:
        print(f'PASS variant={variant} {len(tests)} property executions; no whole-batch replay')
    else:
        print(f'PASS variant={variant} {len(tests)} whole-batch replays')
    return 0


if __name__=='__main__':sys.exit(main())
