"""Exercise actual Windows thread ownership and exception-path restoration."""
import ctypes as c
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from diagnostics.benchmark_thread_priority import OwnedThreadPriority, record


@unittest.skipUnless(sys.platform == 'win32', 'Requires Windows thread handles')
class ThreadPriority(unittest.TestCase):
    def setUp(self):
        self.kernel = c.WinDLL('kernel32', use_last_error=True)
        self.pid = self.kernel.GetCurrentProcessId()
        self.tid = self.kernel.GetCurrentThreadId()

    def test_wrong_owner_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'does not belong'):
            OwnedThreadPriority(self.pid + 1, self.tid)

    def test_retained_handle_restores_priority(self):
        worker = OwnedThreadPriority(self.pid, self.tid)
        original = worker.original
        try:
            worker.set(-1)
            worker.verify(-1)
            self.assertGreater(worker.created_filetime, 0)
        finally:
            self.assertTrue(worker.close())
        check = OwnedThreadPriority(self.pid, self.tid)
        try:
            self.assertEqual(check.original, original)
        finally:
            check.close()

    def test_record_failure_restores_native_thread(self):
        check = OwnedThreadPriority(self.pid, self.tid)
        original = check.original
        check.close()
        closed = []

        def fail_after_priority_change():
            current = OwnedThreadPriority(self.pid, self.tid)
            try:
                self.assertEqual(current.original, -1)
            finally:
                current.close()
            raise RuntimeError('injected CPU-clock failure')

        with tempfile.TemporaryDirectory(dir=ROOT/'local') as folder:
            output = Path(folder)/'blocks.json'
            args = SimpleNamespace(pid=self.pid, tid=self.tid,
                exe=Path(sys.executable), warmup=0, seconds=0,
                priorities=[-1], cpu_work=1283, output=output)
            # Only the Haze process clock is replaced. Ownership checks, OS
            # priority mutation, exception restoration and handles are real.
            with patch('diagnostics.benchmark_thread_priority.process_clock',
                       return_value=(fail_after_priority_change, lambda: closed.append(True))):
                with self.assertRaisesRegex(RuntimeError, 'injected CPU-clock'):
                    record(args)
            result = json.loads(output.read_text())
            self.assertFalse(result['complete'])
            self.assertTrue(result['restored_priority'])
            self.assertEqual(closed, [True])
        check = OwnedThreadPriority(self.pid, self.tid)
        try:
            self.assertEqual(check.original, original)
        finally:
            check.close()


if __name__ == '__main__':
    unittest.main()
