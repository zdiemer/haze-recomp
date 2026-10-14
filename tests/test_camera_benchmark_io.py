"""Native fixture for CameraBenchmark's control-reader and audit ring.

The test extracts the production class and redirects only its fopen calls to a
small wrapper. A blocked control read makes producer-side file I/O observable
without sleeping in the game's update path.
"""
from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BEGIN = "/* BEGIN CAMERA BENCHMARK */"
END = "/* END CAMERA BENCHMARK */"


def extract(source: str) -> str:
    if source.count(BEGIN) != 1 or source.count(END) != 1:
        raise RuntimeError("expected one CameraBenchmark extraction region")
    start = source.index(BEGIN)
    end = source.index(END, start + len(BEGIN))
    if end <= start:
        raise RuntimeError("invalid CameraBenchmark marker order")
    return source[start:end]


def fixture_source(camera: str) -> str:
    camera = camera.replace("std::fopen(", "camera_test_fopen(")
    return f'''#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

static char expected_control[MAX_PATH];
static HANDLE reader_entered, release_reader, vm_read_entered, release_vm_read;
static volatile LONG block_control_once, block_vm_read;
static volatile LONG control_open_count, output_open_count;
static volatile DWORD control_open_tids[32];
static volatile LONG control_tid_count;
static volatile DWORD output_open_tid;
static volatile DWORD race_audit_tid;
static uint32_t guest_words[0x10000 / 4];

static FILE* camera_test_fopen(const char* path, const char* mode)
{{
    if (!strcmp(mode, "r") && !strcmp(path, expected_control)) {{
        const LONG index=InterlockedIncrement(&control_open_count)-1;
        const DWORD tid=GetCurrentThreadId();
        const LONG slot=InterlockedIncrement(&control_tid_count)-1;
        if (slot>=0 && slot<32) control_open_tids[slot]=tid;
        if (index==0 && InterlockedCompareExchange(&block_control_once,0,0)) {{
            SetEvent(reader_entered);
            if (WaitForSingleObject(release_reader,5000)!=WAIT_OBJECT_0) abort();
        }}
    }} else if (!strcmp(mode,"wb")) {{
        InterlockedIncrement(&output_open_count);
        output_open_tid=GetCurrentThreadId();
    }}
    return std::fopen(path,mode);
}}

uint32_t vm_read32(uint32_t address)
{{
    if (InterlockedCompareExchange(&block_vm_read,0,0) &&
        GetCurrentThreadId()==race_audit_tid &&
        InterlockedCompareExchange(&block_vm_read,0,0)==1) {{
        InterlockedExchange(&block_vm_read,2);
        SetEvent(vm_read_entered);
        if (WaitForSingleObject(release_vm_read,5000)!=WAIT_OBJECT_0) abort();
    }}
    if (address/4 >= sizeof(guest_words)/sizeof(guest_words[0])) abort();
    return guest_words[address/4];
}}
uint8_t vm_read8(uint32_t address)
{{
    const uint32_t value=vm_read32(address & ~3u);
    return (uint8_t)(value >> (24u-8u*(address&3u)));
}}
void vm_write32(uint32_t address,uint32_t value)
{{ if (address/4 >= sizeof(guest_words)/sizeof(guest_words[0])) abort(); guest_words[address/4]=value; }}
float read_float(uint32_t address)
{{ uint32_t bits=vm_read32(address); float value; std::memcpy(&value,&bits,sizeof value); return value; }}
void write_float(uint32_t address,float value)
{{ uint32_t bits; std::memcpy(&bits,&value,sizeof bits); vm_write32(address,bits); }}

struct ppu_context {{ uint64_t gpr[32]; }};
static void write_initial_float(uint32_t address,float value)
{{ uint32_t bits; std::memcpy(&bits,&value,sizeof bits); vm_write32(address,bits); }}

{camera}

static DWORD WINAPI run_audit(void* opaque)
{{
    auto* benchmark=static_cast<CameraBenchmark*>(opaque);
    race_audit_tid=GetCurrentThreadId();
    for (unsigned i=0;i<10000;i++) benchmark->audit(0x1000);
    return 0;
}}

static void initialize_guest_motor()
{{
    std::memset(guest_words,0,sizeof guest_words);
    write_initial_float(0x1000+0xb0,0.2f);
    write_initial_float(0x1000+0xb4,0.1f);
    write_initial_float(0x1000+0xac,0.25f);
    write_initial_float(0x1000+0x84,-0.5f);
    write_initial_float(0x1000+0x80,0.5f);
    vm_write32(0x1000+0xa0,1);
}}

static int run_disabled()
{{
    _putenv("HAZE_BENCHMARK_CAMERA_FILE=");
    CameraBenchmark& benchmark=camera_benchmark();
    initialize_guest_motor();
    const float yaw=read_float(0x10b0),pitch=read_float(0x10ac);
    if (benchmark.apply(0x1000)) return 11;
    if (benchmark.reader || benchmark.stop_event || control_open_count || output_open_count) return 12;
    if (read_float(0x10b0)!=yaw || read_float(0x10ac)!=pitch) return 13;
    benchmark.dump();
    return 0;
}}

static int run_active(const char* control)
{{
    std::strncpy(expected_control,control,sizeof expected_control-1);
    expected_control[sizeof expected_control-1]=0;
    _putenv_s("HAZE_BENCHMARK_CAMERA_FILE",control);
    reader_entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    release_reader=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!reader_entered || !release_reader) return 20;
    block_control_once=1;
    {{ FILE* f=std::fopen(control,"wb"); if(!f) return 21; std::fputs("1\\n",f); std::fclose(f); }}
    initialize_guest_motor();
    const DWORD main_tid=GetCurrentThreadId();
    CameraBenchmark& benchmark=camera_benchmark();
    const float initial_yaw=read_float(0x10b0),initial_pitch=read_float(0x10ac);
    if (benchmark.apply(0x1000)) return 22; // Reader has not published token 1 yet.
    if (WaitForSingleObject(reader_entered,5000)!=WAIT_OBJECT_0) return 23;
    const ULONGLONG before=GetTickCount64();
    for (unsigned i=0;i<32;i++) if (benchmark.apply(0x1000)) return 24;
    if (GetTickCount64()-before>100 || control_open_count!=1) return 25;
    if (read_float(0x10b0)!=initial_yaw || read_float(0x10ac)!=initial_pitch) return 26;
    if (control_tid_count<1 || control_open_tids[0]==main_tid) return 27;
    SetEvent(release_reader);
    for (unsigned i=0;i<500 && benchmark.requested_token.load(std::memory_order_acquire)!=1;i++) Sleep(2);
    if (benchmark.requested_token.load(std::memory_order_acquire)!=1) return 28;
    Sleep(10);
    if (!benchmark.apply(0x1000)) return 29;
    const double phase=double(benchmark.sample_qpc-benchmark.origin)/benchmark.frequency;
    constexpr double pi=3.14159265358979323846;
    const double expected_yaw=benchmark.anchor_yaw+(pi/6)*std::sin(phase*(2*pi/4));
    const double actual_yaw=double(read_float(0x10b0))+read_float(0x10b4);
    if (std::fabs(expected_yaw-actual_yaw)>1e-5 ||
        std::fabs(benchmark.wanted_pitch-0.25)>1e-5 ||
        std::fabs(read_float(0x10ac)-0.25)>1e-5) return 30;
    if (vm_read32(0x1014)||vm_read32(0x1018)||vm_read32(0x10c8)||vm_read32(0x10cc)) return 31;
    benchmark.audit(0x1000);
    const float active_yaw=read_float(0x10b0),active_pitch=read_float(0x10ac);
    {{ FILE* f=std::fopen(control,"wb"); if(!f) return 32; std::fputs("0\\n",f); std::fclose(f); }}
    for (unsigned i=0;i<500 && benchmark.requested_token.load(std::memory_order_acquire)!=0;i++) Sleep(2);
    if (benchmark.requested_token.load(std::memory_order_acquire)!=0) return 33;
    if (benchmark.apply(0x1000)) return 34;
    if (read_float(0x10b0)!=active_yaw || read_float(0x10ac)!=active_pitch) return 35;
    for (LONG i=0;i<control_tid_count && i<32;i++)
        if (control_open_tids[i]!=control_open_tids[0]) return 36;

    benchmark.dump();
    if (!benchmark.dumped || benchmark.reader || benchmark.stop_event) return 37;
    const char* output="";
    char output_path[MAX_PATH];
    if (std::snprintf(output_path,sizeof output_path,"%s.csv",control)<=0) return 38;
    {{ FILE* f=std::fopen(output_path,"ab"); if(!f) return 39; std::fputs("# one-shot\\n",f); std::fclose(f); }}
    benchmark.dump();
    if (output_open_count!=1 || output_open_tid!=main_tid) return 40;
    std::ifstream saved(output_path,std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(saved)),{{}});
    if (bytes.size()<11 || bytes.compare(bytes.size()-11,11,"# one-shot\\n")!=0) return 41;
    (void)output;
    return 0;
}}

static int run_wrap(const char* control)
{{
    _putenv_s("HAZE_BENCHMARK_CAMERA_FILE",control);
    CameraBenchmark& benchmark=camera_benchmark();
    initialize_guest_motor();
    benchmark.frequency=1000000; benchmark.origin=100; benchmark.token=7;
    benchmark.wanted_yaw=0.25; benchmark.wanted_pitch=0.1;
    for (uint64_t i=1;i<=CameraBenchmark::capacity+3;i++) {{
        benchmark.sample_qpc=(LONGLONG)(100+i);
        benchmark.audit(0x1000);
    }}
    benchmark.dump();
    if (benchmark.recorded!=CameraBenchmark::capacity+3 ||
        benchmark.retained!=CameraBenchmark::capacity || benchmark.next!=3) return 50;
    return 0;
}}

static int run_race(const char* control)
{{
    _putenv_s("HAZE_BENCHMARK_CAMERA_FILE",control);
    vm_read_entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    release_vm_read=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!vm_read_entered || !release_vm_read) return 60;
    CameraBenchmark& benchmark=camera_benchmark();
    initialize_guest_motor();
    benchmark.frequency=1000000; benchmark.sample_qpc=100; benchmark.origin=0;
    benchmark.token=1; benchmark.wanted_yaw=0.2; benchmark.wanted_pitch=0.1;
    block_vm_read=1;
    HANDLE worker=CreateThread(nullptr,0,run_audit,&benchmark,0,nullptr);
    if (!worker || WaitForSingleObject(vm_read_entered,5000)!=WAIT_OBJECT_0) return 61;
    benchmark.dump(); // Audit is paused while constructing its sample, before taking sample_lock.
    if (!benchmark.dumped || benchmark.recorded!=0) return 62;
    SetEvent(release_vm_read);
    if (WaitForSingleObject(worker,5000)!=WAIT_OBJECT_0) return 63;
    CloseHandle(worker);
    if (benchmark.recorded!=0) return 64;
    return 0;
}}

int main(int argc,char** argv)
{{
    if (argc<2) return 2;
    if (!strcmp(argv[1],"disabled")) return run_disabled();
    if (argc!=3) return 3;
    if (!strcmp(argv[1],"active")) return run_active(argv[2]);
    if (!strcmp(argv[1],"wrap")) return run_wrap(argv[2]);
    if (!strcmp(argv[1],"race")) return run_race(argv[2]);
    return 4;
}}
'''


def compiler_command(compiler: str, source: Path, exe: Path) -> list[str]:
    return [compiler, "/nologo", "/TP", "/std:c++17", "/EHsc", "/O2", "/MT", "/W4",
            "/wd4100", "/D_WIN32", str(source), f"/Fe:{exe}", f"/Fo:{exe.with_suffix('.obj')}"]


def run_case(exe: Path, mode: str, control: Path | None) -> None:
    env=os.environ.copy()
    env.pop("HAZE_BENCHMARK_CAMERA_FILE",None)
    args=[str(exe),mode]
    if control is not None:
        args.append(str(control))
    result=subprocess.run(args,env=env,capture_output=True,text=True)
    if result.returncode:
        raise AssertionError(f"{mode} failed ({result.returncode})\n{result.stdout}\n{result.stderr}")


def validate_active(control: Path) -> None:
    path=Path(str(control)+".csv")
    raw=path.read_text(encoding="utf-8").splitlines()
    if not raw or raw[-1]!="# one-shot":
        raise AssertionError("second dump rewrote the CSV or appended another dump")
    rows=list(csv.DictReader(raw[0:-1]))
    if len(rows)!=1 or rows[0]["token"]!="1":
        raise AssertionError(f"expected exactly the active-token audit row, got {rows[:3]}")


def validate_wrap(control: Path) -> None:
    path=Path(str(control)+".csv")
    with path.open(newline="",encoding="utf-8") as stream:
        reader=csv.DictReader(stream)
        first=next(reader)
        count=1
        last=first
        for last in reader:
            count+=1
    capacity=65536
    if count!=capacity or first["qpc"]!="104" or last["qpc"]!="65639":
        raise AssertionError(f"ring did not retain chronological wrapped tail: count={count}, first={first}, last={last}")


def validate_race(control: Path) -> None:
    path=Path(str(control)+".csv")
    with path.open(newline="",encoding="utf-8") as stream:
        reader=csv.reader(stream)
        header=next(reader)
        if len(header)!=9:
            raise AssertionError(f"bad camera audit CSV header: {header}")
        for row in reader:
            if len(row)!=len(header):
                raise AssertionError(f"partial row published during dump race: {row}")
            raise AssertionError(f"paused pre-lock audit unexpectedly entered snapshot: {row}")


def main() -> int:
    parser=argparse.ArgumentParser()
    parser.add_argument("--compiler",required=True,help="clang-cl executable")
    parser.add_argument("--source",type=Path,default=ROOT/"port/haze_camera.cpp")
    parser.add_argument("--output-dir",type=Path,default=ROOT/"local/test-camera-benchmark-io")
    args=parser.parse_args()
    camera=extract(args.source.read_text(encoding="utf-8"))
    args.output_dir.mkdir(parents=True,exist_ok=True)
    fixture=args.output_dir/"camera_benchmark_io_fixture.cpp"
    fixture.write_text(fixture_source(camera),encoding="utf-8")
    exe=args.output_dir/"camera_benchmark_io_fixture.exe"
    built=subprocess.run(compiler_command(args.compiler,fixture,exe),capture_output=True,text=True)
    if built.returncode:
        raise RuntimeError(f"compile failed ({built.returncode})\n{built.stdout}\n{built.stderr}")
    with tempfile.TemporaryDirectory(prefix="haze-camera-bench-") as temporary:
        base=Path(temporary)
        run_case(exe,"disabled",None)
        print("PASS disabled path starts no reader and leaves angles unchanged")
        active=base/"control-active.txt"
        run_case(exe,"active",active)
        validate_active(active)
        print("PASS reader-only file I/O, nonblocking apply, token-zero behavior, angle updates, shutdown/dump one-shot")
        wrap=base/"control-wrap.txt"
        run_case(exe,"wrap",wrap)
        validate_wrap(wrap)
        print("PASS audit ring wrap retains the newest rows in chronological order")
        race=base/"control-race.txt"
        run_case(exe,"race",race)
        validate_race(race)
        print("PASS dump excludes an audit paused before the publication lock")
    print("4 focused camera benchmark I/O cases passed")
    return 0


if __name__=="__main__":
    raise SystemExit(main())
