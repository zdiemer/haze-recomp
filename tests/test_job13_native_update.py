"""Compile original-retaining shadow comparisons for image32's full particle loop."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from patch_job13_native_update import patch, patch_baseline


def run(cmd, cwd, log):
    result=subprocess.run(cmd,cwd=cwd,capture_output=True,text=True)
    log.write_text(result.stdout+result.stderr,encoding='utf-8')
    print(result.stdout+result.stderr,end='')
    if result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}): {cmd[0]}')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler',required=True)
    p.add_argument('--sdk',type=Path,required=True)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--registration',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True)
    p.add_argument('--mode',choices=('baseline','control','shadow','execute'),default='shadow')
    p.add_argument('--diagnostics',choices=('on','off'),default='on')
    p.add_argument('--repetitions',type=int,default=1,
        help='2..1000 whole-batch runs; first is warmup and excluded from timing')
    p.add_argument('--fixture',type=Path,action='append',required=True)
    a=p.parse_args()
    if not 1<=a.repetitions<=1000 or (a.repetitions==1 and a.mode in ('baseline','control','execute')):
        if a.repetitions!=1 or a.mode in ('baseline','control','execute'):
            p.error('benchmark modes require --repetitions 2..1000')
    sdk=a.sdk.resolve(); out=a.output_dir.resolve()
    out.mkdir(parents=True,exist_ok=False)
    generated=out/'spu_recomp.c'
    source_text=a.source.read_text(encoding='utf-8')
    registration_text=a.registration.read_text(encoding='utf-8')
    diagnostics=a.diagnostics=='on'
    if a.mode=='baseline':
        patched_text=patch_baseline(source_text,registration_text)
    else:
        update_mode={'control':0,'shadow':1,'execute':2}[a.mode]
        patched_text=patch(source_text,registration_text,mode=update_mode,
                           native_math=True,diagnostics=diagnostics)
    generated.write_text(patched_text,encoding='utf-8',newline='')
    generated_header=out/'spu_recomp.h'
    generated_header.write_bytes(a.source.with_suffix('.h').read_bytes())
    common=['/nologo','/O2','/MD','/DSPU_FLOAT_NATIVE_RUNTIME=1',
        '/DSPU_USE_DISPATCH_MT','/D_CRT_SECURE_NO_WARNINGS','/clang:-msse4.1',
        '/clang:-fno-strict-aliasing','/DHAZE_JOB13_TEST_HAS_UPDATE='+str(int(a.mode!='baseline')),
        '/DHAZE_JOB13_TEST_DIAGNOSTICS='+str(int(diagnostics)),
        '/DHAZE_JOB13_TEST_REQUIRE_NATIVE='+str(int(a.mode=='execute' and diagnostics)),
        '/DHAZE_JOB13_NATIVE_UPDATE_DIAGNOSTICS='+str(int(diagnostics)),
        '/I'+str(sdk/'runtime/spu'),
        '/I'+str(ROOT/'port'),'/I'+str(out)]
    cobj=out/'spu_recomp.obj'; cppobj=out/'native_update.obj'; testobj=out/'native_update_test.obj'; floatobj=out/'spu_float_native.obj'
    run([a.compiler,*common,'/TC','/std:c17','/c',str(generated),'/Fo'+str(cobj)],out,out/'compile-generated.log')
    run([a.compiler,*common,'/TP','/std:c++17','/EHsc','/c',
        str(ROOT/'port/haze_job13_native_update.cpp'),'/Fo'+str(cppobj)],out,out/'compile-native.log')
    run([a.compiler,*common,'/TC','/std:c17','/c',
        str(ROOT/'tests/test_job13_native_update.c'),'/Fo'+str(testobj)],out,out/'compile-test.log')
    run([a.compiler,*common,'/TC','/std:c17','/c',
        str(sdk/'runtime/spu/spu_float_native.c'),'/Fo'+str(floatobj)],out,out/'compile-float.log')
    exe=out/'test_job13_native_update.exe'
    run([a.compiler,'/nologo',str(cobj),str(cppobj),str(testobj),str(floatobj),'/Fe'+str(exe)],out,out/'link.log')
    reports=[]
    for fixture in a.fixture:
        command=[str(exe),str(fixture.resolve())]
        if a.repetitions>1: command.append(str(a.repetitions))
        result=subprocess.run(command,cwd=out,
            capture_output=True,text=True,timeout=60)
        text=result.stdout+result.stderr
        print(text,end='')
        reports.append(dict(path=str(fixture.resolve()),exit_code=result.returncode,output=text))
    provenance={
        'source_path':str(a.source.resolve()),
        'source_sha256':hashlib.sha256(source_text.encode()).hexdigest(),
        'registration_path':str(a.registration.resolve()),
        'registration_sha256':hashlib.sha256(registration_text.encode()).hexdigest(),
        'patched_sha256':hashlib.sha256(patched_text.encode()).hexdigest(),
        'mode':a.mode,
        'diagnostics':diagnostics,
        'repetitions':a.repetitions,
        'native_divide':True,
        'native_math':True,
        'patchers':{name:hashlib.sha256((ROOT/'scripts'/name).read_bytes()).hexdigest()
            for name in ('patch_job13_native_update.py','patch_job13_record_loop.py',
                         'patch_job13_native_divide.py','patch_job13_native_math.py')},
        'owned_files':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
            for name in ('port/haze_job13_native_update.cpp','port/haze_job13_native_update.h',
                         'port/haze_job13_native_divide.h','port/haze_job13_native_math.h',
                         'tests/test_job13_native_update.c','tests/test_job13_native_update.py')},
        'fixtures':[{'path':str(x.resolve()),'sha256':hashlib.sha256(x.read_bytes()).hexdigest()} for x in a.fixture],
    }
    (out/'results.json').write_text(json.dumps(dict(provenance=provenance,fixtures=reports),indent=2)+'\n',encoding='utf-8')
    return 1 if any(x['exit_code'] for x in reports) else 0


if __name__=='__main__':
    raise SystemExit(main())
