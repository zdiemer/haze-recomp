"""Compile and exercise the actual SDK milestone implementation on Windows."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEST_ARGS = None
FIXTURE = r'''
#include <windows.h>
#include <process.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "ps3emu/milestone.h"

static HANDLE g_writer_entered;
static HANDLE g_writer_release;
static HANDLE g_checkpoint_written;
static HANDLE g_writer_started_event;
static std::atomic<int> g_gate_enabled{0};
static std::atomic<int> g_writer_started{0};

extern "C" void ps3_ms_test_writer_gate(void) {
    g_writer_started.fetch_add(1,std::memory_order_relaxed);
    if (g_writer_started_event) SetEvent(g_writer_started_event);
    if (g_gate_enabled.load(std::memory_order_acquire)) {
        SetEvent(g_writer_entered);
        WaitForSingleObject(g_writer_release, INFINITE);
    }
}
extern "C" void ps3_ms_test_record_written(unsigned type) {
    if (type == 3) SetEvent(g_checkpoint_written);
}

static DWORD producer(LPVOID) {
    ps3_ms("beta");
    ps3_ms("alpha");
    ps3_ms_kv("scalar:answer", 43);
    return 0;
}

static int blocked_writer_case(void) {
    g_writer_entered=CreateEventA(NULL,TRUE,FALSE,NULL);
    g_writer_release=CreateEventA(NULL,TRUE,FALSE,NULL);
    g_checkpoint_written=CreateEventA(NULL,TRUE,FALSE,NULL);
    g_gate_enabled.store(1,std::memory_order_release);
    ps3_ms("alpha");
    if (WaitForSingleObject(g_writer_entered,5000)!=WAIT_OBJECT_0) return 11;

    HANDLE thread=CreateThread(NULL,0,producer,NULL,0,NULL);
    if (!thread) return 12;
    const bool producer_finished=WaitForSingleObject(thread,1000)==WAIT_OBJECT_0;
    CloseHandle(thread);
    if (!producer_finished) {
        SetEvent(g_writer_release);
        return 13;
    }

    /* Fill the bounded queue while the writer remains parked. Overflow must
       be explicit, and these calls must never wait for the writer. */
    char key[64];
    for (int i=0;i<6000;i++) {
        std::snprintf(key,sizeof key,"overflow:%d",i);
        ps3_ms_kv(key,i);
    }
    SetEvent(g_writer_release);
    g_gate_enabled.store(0,std::memory_order_release);
    std::thread a([]{ ps3_ms_dump(); });
    std::thread b([]{ ps3_ms_dump(); });
    a.join(); b.join();
    ps3_ms_dump(); /* Idempotent even after concurrent callers. */
    CloseHandle(g_writer_entered);
    CloseHandle(g_writer_release);
    CloseHandle(g_checkpoint_written);
    return 0;
}

static int checkpoint_case(void) {
    g_writer_entered=CreateEventA(NULL,TRUE,FALSE,NULL);
    g_writer_release=CreateEventA(NULL,TRUE,FALSE,NULL);
    g_checkpoint_written=CreateEventA(NULL,TRUE,FALSE,NULL);
    ps3_ms("progress");
    if (WaitForSingleObject(g_checkpoint_written,5000)!=WAIT_OBJECT_0) return 21;
    /* No guest calls after this point: writer-owned timed checkpointing must
       preserve progress if an external watchdog terminates the process. */
    _exit(0);
}

static DWORD flood_producer(LPVOID) {
    long long i=0;
    while (WaitForSingleObject(g_checkpoint_written,0)!=WAIT_OBJECT_0)
        ps3_ms_kv("flood",i++);
    return 0;
}

static int continuous_queue_case(void) {
    g_checkpoint_written=CreateEventA(NULL,TRUE,FALSE,NULL);
    if (!g_checkpoint_written) return 31;
    ps3_ms("continuous");
    HANDLE producer_thread=CreateThread(NULL,0,flood_producer,NULL,0,NULL);
    if (!producer_thread) return 32;
    DWORD wait=WaitForSingleObject(g_checkpoint_written,7000);
    SetEvent(g_checkpoint_written);
    WaitForSingleObject(producer_thread,5000);
    CloseHandle(producer_thread);
    if (wait!=WAIT_OBJECT_0) return 33;
    ps3_ms_dump();
    CloseHandle(g_checkpoint_written);
    return 0;
}

static int open_failure_case(void) {
    g_writer_started_event=CreateEventA(NULL,TRUE,FALSE,NULL);
    if (!g_writer_started_event) return 41;
    ps3_ms("not-written");
    if (WaitForSingleObject(g_writer_started_event,5000)!=WAIT_OBJECT_0) return 42;
    FILETIME create_time,exit_time,kernel_before,user_before,kernel_after,user_after;
    if (!GetProcessTimes(GetCurrentProcess(),&create_time,&exit_time,&kernel_before,&user_before)) return 43;
    Sleep(300);
    if (!GetProcessTimes(GetCurrentProcess(),&create_time,&exit_time,&kernel_after,&user_after)) return 44;
    ULARGE_INTEGER cpu_before,cpu_after;
    cpu_before.LowPart=kernel_before.dwLowDateTime; cpu_before.HighPart=kernel_before.dwHighDateTime;
    ULARGE_INTEGER user_before_u;
    user_before_u.LowPart=user_before.dwLowDateTime; user_before_u.HighPart=user_before.dwHighDateTime;
    cpu_before.QuadPart+=user_before_u.QuadPart;
    cpu_after.LowPart=kernel_after.dwLowDateTime; cpu_after.HighPart=kernel_after.dwHighDateTime;
    ULARGE_INTEGER user_after_u;
    user_after_u.LowPart=user_after.dwLowDateTime; user_after_u.HighPart=user_after.dwHighDateTime;
    cpu_after.QuadPart+=user_after_u.QuadPart;
    /* FILETIME uses 100 ns units. An expired-deadline busy loop would consume
       most of this 300 ms interval; a failed writer should be asleep. */
    if (cpu_after.QuadPart-cpu_before.QuadPart>1000000ull) return 45;
    ps3_ms_dump();
    CloseHandle(g_writer_started_event);
    /* Asynchronous fopen failure must disable output without a crash or hang. */
    return 0;
}

int main(int argc,char** argv) {
    if (argc<2) return 2;
    if (std::strcmp(argv[1],"off")==0) {
        ps3_ms("disabled"); ps3_ms_kv("disabled",1); ps3_ms_dump();
        return g_writer_started.load(std::memory_order_relaxed) ? 3 : 0;
    }
    if (std::strcmp(argv[1],"blocked")==0) return blocked_writer_case();
    if (std::strcmp(argv[1],"checkpoint")==0) return checkpoint_case();
    if (std::strcmp(argv[1],"continuous")==0) return continuous_queue_case();
    if (std::strcmp(argv[1],"openfail")==0) return open_failure_case();
    return 2;
}
'''


class AsyncMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name != 'nt':
            raise unittest.SkipTest('The focused fixture uses the native Windows runtime')
        cls.compiler, cls.sdk, cls.outdir = TEST_ARGS.compiler, TEST_ARGS.sdk.resolve(), TEST_ARGS.output_dir.resolve()
        cls.outdir.mkdir(parents=True, exist_ok=True)
        cls.fixture = cls.outdir/'milestone_async_fixture.cpp'
        cls.exe = cls.outdir/'milestone_async_fixture.exe'
        cls.fixture.write_text(FIXTURE)
        command = [str(cls.compiler), '/nologo', '/std:c++20', '/EHsc',
            '/DPS3_MILESTONE_TEST_HOOKS',
            '/I'+str(cls.sdk/'include'), '/I'+str(cls.sdk/'runtime'),
            '/Fe:'+str(cls.exe), str(cls.fixture), str(cls.sdk/'runtime/milestone.c')]
        built = subprocess.run(command, cwd=cls.sdk, capture_output=True, text=True, timeout=120)
        if built.returncode:
            raise AssertionError('fixture compilation failed:\n'+built.stdout+'\n'+built.stderr)

    def run_mode(self, mode, output=None):
        env=dict(os.environ)
        env.pop('PS3_MILESTONE_OUT',None)
        if output is not None:
            env['PS3_MILESTONE_OUT']=str(output)
        return subprocess.run([str(self.exe),mode],env=env,capture_output=True,
                              timeout=30)

    def test_disabled_creates_no_output(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'local') as folder:
            output=Path(folder)/'disabled.txt'
            result=self.run_mode('off')
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
            self.assertFalse(output.exists())

    def test_blocked_writer_does_not_block_producers_and_overflow_is_reported(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'local') as folder:
            output=Path(folder)/'blocked.txt'
            result=self.run_mode('blocked',output)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
            lines=output.read_text().splitlines()
            stream=[line.split('\t',1) for line in lines if line and line[0].isdigit()]
            self.assertEqual(stream,[['1','alpha'],['2','beta']])
            self.assertIn('kv\tscalar:answer\t43',lines)
            self.assertTrue(any(line.startswith('# async-overflow records=') for line in lines))
            self.assertEqual(lines.count('# end'),1)

    def test_checkpoint_is_flushed_without_clean_exit_or_more_guest_calls(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'local') as folder:
            output=Path(folder)/'checkpoint.txt'
            result=self.run_mode('checkpoint',output)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
            lines=output.read_text().splitlines()
            self.assertIn('# counts',lines)
            self.assertIn('count\tprogress\t1',lines)
            self.assertNotIn('# end',lines)

    def test_async_open_failure_disables_without_crash(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'local') as folder:
            missing=Path(folder)/'missing'/'cannot-open.txt'
            result=self.run_mode('openfail',missing)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
            self.assertIn(b'cannot open configured output',result.stderr)
            self.assertFalse(missing.exists())

    def test_checkpoint_is_not_starved_by_continuous_queue_traffic(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'local') as folder:
            output=Path(folder)/'continuous.txt'
            result=self.run_mode('continuous',output)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
            self.assertIn('# counts',output.read_text().splitlines())


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--sdk',type=Path,default=ROOT/'external/ps3recomp')
    parser.add_argument('--output-dir',type=Path,default=ROOT/'local')
    TEST_ARGS=parser.parse_args()
    unittest.main(argv=[__file__])
