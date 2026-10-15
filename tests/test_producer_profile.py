#!/usr/bin/env python3
"""Compile and exercise the actual bounded producer-profile implementation."""
import argparse
import csv
import os
import pathlib
import subprocess
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


FIXTURE = r'''#include <windows.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <thread>
#include <vector>

static std::atomic<std::uint64_t> qpc_value{1000};
static std::atomic<std::uint64_t> cycle_value{5000};
static std::atomic<unsigned> qpc_calls{0}, cycle_calls{0}, frequency_calls{0};
static std::atomic<bool> fail_qpc{false}, fail_cycles{false};
static std::atomic<bool> reverse_cycles{false};
extern "C" int haze_producer_test_qpc(std::uint64_t* out) noexcept {
    ++qpc_calls;
    if (fail_qpc.load(std::memory_order_relaxed)) { *out = 0; return 0; }
    *out = qpc_value.fetch_add(100, std::memory_order_relaxed); return 1;
}
extern "C" int haze_producer_test_frequency(std::uint64_t* out) noexcept {
    ++frequency_calls; *out = 1000000; return 1;
}
extern "C" int haze_producer_test_cycles(std::uint64_t* out) noexcept {
    ++cycle_calls;
    if (fail_cycles.load(std::memory_order_relaxed)) { *out = 0; return 0; }
    if (reverse_cycles.load(std::memory_order_relaxed)) {
        *out = cycle_value.fetch_sub(1000, std::memory_order_relaxed); return 1;
    }
    *out = cycle_value.fetch_add(1000, std::memory_order_relaxed); return 1;
}

#include "../port/haze_producer_profile.cpp"

static constexpr std::uint64_t kToc = 0xE94EE0;

int main(int argc, char** argv) {
    assert(argc == 3);
    const char* mode = argv[1];
    _putenv_s("HAZE_PRODUCER_PROFILE_CSV", argv[2]);
    _putenv_s("HAZE_PRODUCER_PROFILE_COARSE", "");

    if (std::strcmp(mode, "coarse") == 0) {
        _putenv_s("HAZE_PRODUCER_PROFILE_COARSE", "1");
        {
            haze_producer_profile::frame_scope root(1, kToc, 0x00680FA0, 19);
            assert(root.active() && root.id() == 1);
            const unsigned clocks = qpc_calls.load();
            const unsigned cycles = cycle_calls.load();
            {
                haze_producer_profile::call_scope skipped(root, 0x007EBB6C, 0x006C3890);
                haze_producer_profile::call_scope wrong_callee(root, 0x006812F8, 0x006869F4);
                assert(!skipped.active() && !wrong_callee.active());
                assert(qpc_calls == clocks && cycle_calls == cycles);
                // A skipped wrapper must not hide a selected descendant.
                haze_producer_profile::call_scope selected(0x006812F8, 0x006869F0);
                assert(selected.active() && selected.id() == 2);
                {
                    haze_producer_profile::call_scope skipped_inner(0x00686A20, 0x0068EC40);
                    assert(!skipped_inner.active());
                    haze_producer_profile::call_scope child(0x00686A50, 0x00684ED8);
                    assert(child.active() && child.id() == 3);
                }
            }
            haze_producer_profile::call_scope sibling(0x00681184, 0x005FBB70);
            assert(sibling.active() && sibling.id() == 4);
        }
        haze_producer_profile::haze_producer_profile_dump();
        assert(qpc_calls == 9 && cycle_calls == 8);
        return 0;
    }

    if (std::strcmp(mode, "setup") == 0) {
        _putenv_s("HAZE_PRODUCER_PROFILE_COARSE", "1");
        {
            haze_producer_profile::frame_scope root(1, kToc, 0x00680FA0, 23);
            const unsigned clocks = qpc_calls.load();
            {
                haze_producer_profile::call_scope wrong(0x00681054, 0x00C27870);
                haze_producer_profile::call_scope timer(0x00681108, 0x00680D18);
                assert(!wrong.active() && !timer.active() && qpc_calls == clocks);
            }
            {
                haze_producer_profile::call_scope acquire(0x00681054, 0x00C2786C);
                assert(acquire.active());
            }
            for (unsigned i = 0; i != 2; ++i) {
                haze_producer_profile::call_scope queue(0x006810EC, 0x00680A30);
                assert(queue.active());
            }
            {
                haze_producer_profile::call_scope update(0x00681148, 0x0076F798);
                assert(update.active());
            }
        }
        haze_producer_profile::haze_producer_profile_dump();
        assert(qpc_calls == 11 && cycle_calls == 10);
        return 0;
    }

    if (std::strcmp(mode, "disabled") == 0) {
        _putenv_s("HAZE_PRODUCER_PROFILE_CSV", "");
        { haze_producer_profile::frame_scope wrong_thread(2, kToc, 0x680FA0);
          haze_producer_profile::frame_scope wrong_toc(1, 0, 0x680FA0);
          haze_producer_profile::frame_scope valid_but_disabled(1, kToc, 0x680FA0);
          assert(!wrong_thread.active() && !wrong_toc.active() && !valid_but_disabled.active()); }
        haze_producer_profile::haze_producer_profile_dump();
        assert(qpc_calls == 0 && cycle_calls == 0 && frequency_calls == 0);
        return 0;
    }

    if (std::strcmp(mode, "nested") == 0) {
        {
            haze_producer_profile::frame_scope root(1, kToc, 0x680FA0, 77);
            assert(root.active() && root.id() == 1);
            {
                haze_producer_profile::call_scope outer(root, 0x0068ECD8, 0x0068ECD8);
                assert(outer.active() && outer.id() == 2);
                {
                    // Mirrors generated nested func_00680FA0 instrumentation,
                    // which has no explicit parent argument.
                    haze_producer_profile::frame_scope recursive(1, kToc, 0x00680FA0);
                    assert(recursive.active() && recursive.id() == 3);
                    assert(recursive.generation() == 77);
                    haze_producer_profile::call_scope inner(recursive, 0x006DD638, 0x006DD638);
                    assert(inner.active() && inner.id() == 4);
                }
            }
            { haze_producer_profile::frame_scope nested(1, kToc, 0x00680FA0, 78, &root);
              assert(nested.active() && nested.id() == 5); }
        }
        haze_producer_profile::haze_producer_profile_dump();
        haze_producer_profile::haze_producer_profile_dump();
        if (qpc_calls.load() != cycle_calls.load() + 1 || qpc_calls.load() < 10) {
            std::fprintf(stderr, "clock counts qpc=%u cycles=%u frequency=%u\\n",
                qpc_calls.load(), cycle_calls.load(), frequency_calls.load());
            assert(false);
        }
        return 0;
    }

    if (std::strcmp(mode, "implicit-calls") == 0) {
        // An implicit call outside any frame must stay completely inert, even
        // when the output environment variable is present.
        {
            haze_producer_profile::call_scope no_parent(0x00686A20, 0x0068EC40);
            assert(!no_parent.active());
            assert(qpc_calls == 0 && cycle_calls == 0 && frequency_calls == 0);
        }
        {
            haze_producer_profile::frame_scope root(1, kToc, 0x00680FA0, 99);
            assert(root.active() && root.id() == 1);
            {
                haze_producer_profile::call_scope outer(0x00686A20, 0x0068EC40);
                assert(outer.active() && outer.id() == 2);
                assert(outer.generation() == 99);
                {
                    haze_producer_profile::call_scope inner(0x00686A44, 0x00684ED8);
                    assert(inner.active() && inner.id() == 3);
                    assert(inner.generation() == 99);
                }
                {
                    haze_producer_profile::call_scope sibling(0x00686A50, 0x00684ED8);
                    assert(sibling.active() && sibling.id() == 4);
                    assert(sibling.generation() == 99);
                }
            }
            {
                haze_producer_profile::call_scope restored(0x00686A54, 0x006F0DE0);
                assert(restored.active() && restored.id() == 5);
                assert(restored.generation() == 99);
            }
        }
        const auto before_unparented = qpc_calls.load();
        const auto cycles_before_unparented = cycle_calls.load();
        {
            haze_producer_profile::call_scope no_parent(0x00686A68, 0x006F2348);
            assert(!no_parent.active());
        }
        assert(qpc_calls == before_unparented && cycle_calls == cycles_before_unparented);
        haze_producer_profile::haze_producer_profile_dump();
        return 0;
    }

    if (std::strcmp(mode, "no-parent") == 0) {
        haze_producer_profile::call_scope no_parent(0x00686A20, 0x0068EC40);
        assert(!no_parent.active());
        assert(qpc_calls == 0 && cycle_calls == 0 && frequency_calls == 0);
        return 0;
    }

    if (std::strcmp(mode, "overflow") == 0) {
        for (unsigned i = 0; i < 20; ++i) {
            haze_producer_profile::frame_scope row(1, kToc, 0x680FA0, i);
            assert(row.active());
        }
        haze_producer_profile::haze_producer_profile_dump();
        return 0;
    }

    if (std::strcmp(mode, "invalid-clock") == 0) {
        {
            fail_qpc.store(true, std::memory_order_relaxed);
            haze_producer_profile::frame_scope qpc_invalid(1, kToc, 0x680FA0, 1);
            fail_qpc.store(false, std::memory_order_relaxed);
        }
        {
          haze_producer_profile::frame_scope cycle_invalid(1, kToc, 0x680FA0, 2);
          fail_cycles.store(true, std::memory_order_relaxed); }
        fail_cycles.store(false, std::memory_order_relaxed);
        {
            reverse_cycles.store(true, std::memory_order_relaxed);
            { haze_producer_profile::frame_scope cycle_decreasing(1, kToc, 0x680FA0, 3); }
            reverse_cycles.store(false, std::memory_order_relaxed);
        }
        haze_producer_profile::haze_producer_profile_dump();
        return 0;
    }

    if (std::strcmp(mode, "concurrent") == 0) {
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < 4; ++t) {
            threads.emplace_back([t] {
                for (unsigned i = 0; i < 8; ++i) {
                    haze_producer_profile::frame_scope row(1, kToc, 0x680FA0,
                        (std::uint64_t(t) << 32) | i);
                    assert(row.active());
                }
            });
        }
        for (auto& thread : threads) thread.join();
        haze_producer_profile::haze_producer_profile_dump();
        return 0;
    }

    if (std::strcmp(mode, "inflight") == 0) {
        std::atomic<bool> entered{false}, release{false};
        std::thread worker([&] {
            haze_producer_profile::frame_scope row(1, kToc, 0x680FA0, 1);
            assert(row.active());
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) SwitchToThread();
        });
        while (!entered.load(std::memory_order_acquire)) SwitchToThread();
        haze_producer_profile::haze_producer_profile_dump();
        release.store(true, std::memory_order_release);
        worker.join();
        return 0;
    }

    if (std::strcmp(mode, "overwrite-inflight") == 0) {
        std::atomic<bool> entered{false}, release{false};
        std::thread old_scope([&] {
            haze_producer_profile::frame_scope row(1, kToc, 0x680FA0, 1);
            assert(row.active());
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) SwitchToThread();
        });
        while (!entered.load(std::memory_order_acquire)) SwitchToThread();
        for (unsigned i = 0; i < 16; ++i) {
            haze_producer_profile::frame_scope row(1, kToc, 0x680FA0, i + 2);
            assert(row.active());
        }
        release.store(true, std::memory_order_release);
        old_scope.join();
        haze_producer_profile::haze_producer_profile_dump();
        return 0;
    }
    assert(false && "unknown fixture mode");
    return 2;
}
'''


def parse_csv(path):
    lines = pathlib.Path(path).read_text(encoding="ascii").splitlines()
    assert lines and lines[0].startswith("#qpc_frequency,")
    metadata_fields = next(csv.reader([lines[0][1:]]))
    metadata = dict(zip(metadata_fields[::2], metadata_fields[1::2]))
    rows = list(csv.DictReader(lines[1:]))
    return metadata, rows


class ProducerProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out_dir = pathlib.Path(cls.output_dir_arg).resolve()
        cls.out_dir.mkdir(parents=True, exist_ok=True)
        cls.exe = cls.out_dir / "producer-profile-fixture.exe"
        cls.fixture = cls.out_dir / "producer-profile-fixture.cpp"
        cls.fixture.write_text(FIXTURE.replace(
            '#include "../port/haze_producer_profile.cpp"',
            '#include "' + (ROOT / 'port/haze_producer_profile.cpp').as_posix() + '"'),
            encoding="utf-8")
        command = [cls.compiler_arg, "/nologo", "/std:c++17", "/EHsc", "/O2",
            "/DHAZE_PRODUCER_PROFILE_TEST_CLOCKS", "/DHAZE_PRODUCER_PROFILE_TEST_CAPACITY=16",
            str(cls.fixture), "/Fe" + str(cls.exe),
            "/Fo" + str(cls.out_dir / "producer-profile-fixture.obj")]
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError("fixture compilation failed:\n" + result.stdout + result.stderr)

    def run_mode(self, mode, path=None):
        path = path or (self.out_dir / ("producer-profile-" + mode + ".csv"))
        path.unlink(missing_ok=True)
        result = subprocess.run([str(self.exe), mode, str(path)], cwd=ROOT,
            text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return path

    def test_disabled_is_clock_and_file_free(self):
        path = self.run_mode("disabled")
        self.assertFalse(path.exists())

    def test_coarse_filter_omits_clocks_and_preserves_ancestor_tree(self):
        metadata, rows = parse_csv(self.run_mode("coarse"))
        self.assertEqual(metadata["coarse"], "1")
        self.assertEqual(metadata["begun"], "4")
        self.assertEqual(metadata["completed"], "4")
        self.assertEqual([row["parent_id"] for row in rows], ["0", "1", "2", "1"])
        self.assertEqual([row["generation"] for row in rows], ["19"] * 4)
        self.assertEqual([int(row["callsite_lr"]) for row in rows],
                         [0, 0x006812F8, 0x00686A50, 0x00681184])

    def test_coarse_setup_keeps_repeated_queue_calls_and_rejects_wrong_edges(self):
        metadata, rows = parse_csv(self.run_mode("setup"))
        self.assertEqual(metadata["begun"], "5")
        self.assertEqual(metadata["completed"], "5")
        self.assertEqual([row["parent_id"] for row in rows], ["0", "1", "1", "1", "1"])
        self.assertEqual([row["generation"] for row in rows], ["23"] * 5)
        self.assertEqual([int(row["callsite_lr"]) for row in rows],
                         [0, 0x00681054, 0x006810EC, 0x006810EC, 0x00681148])

    def test_nested_frame_calls_and_idempotent_dump(self):
        path = self.run_mode("nested")
        metadata, rows = parse_csv(path)
        self.assertEqual(int(metadata["begun"]), int(metadata["completed"]) +
            int(metadata["late_discarded"]) + int(metadata["inflight_at_dump"]))
        self.assertEqual(metadata["begun"], "5")
        self.assertEqual(metadata["completed"], "5")
        self.assertEqual(metadata["inflight_at_dump"], "0")
        self.assertEqual([row["kind"] for row in rows], ["0", "1", "0", "1", "0"])
        self.assertEqual([row["parent_id"] for row in rows], ["0", "1", "2", "3", "1"])
        self.assertEqual(rows[0]["generation"], "77")
        self.assertEqual(rows[2]["generation"], "77")
        self.assertEqual(rows[0]["guest_ea"], str(0x680FA0))
        self.assertEqual(rows[1]["callsite_lr"], str(0x0068ECD8))
        self.assertEqual(rows[3]["callee_ea"], str(0x006DD638))
        for row in rows:
            self.assertGreater(int(row["qpc_end"]), int(row["qpc_begin"]))
            self.assertEqual(row["qpc_valid"], "1")
            self.assertEqual(row["cycles_valid"], "1")
            self.assertGreater(int(row["cycles_delta"]), 0)

    def test_implicit_calls_inherit_active_tls_scope_and_restore_parent(self):
        metadata, rows = parse_csv(self.run_mode("implicit-calls"))
        self.assertEqual(metadata["begun"], "5")
        self.assertEqual(metadata["completed"], "5")
        self.assertEqual([row["kind"] for row in rows], ["0", "1", "1", "1", "1"])
        self.assertEqual([row["parent_id"] for row in rows], ["0", "1", "2", "2", "1"])
        self.assertEqual([row["generation"] for row in rows], ["99"] * 5)
        self.assertEqual([row["callsite_lr"] for row in rows[1:]], [
            str(0x00686A20), str(0x00686A44), str(0x00686A50), str(0x00686A54)])
        self.assertTrue(all(row["completed"] == "1" and row["qpc_valid"] == "1"
                            for row in rows))

    def test_implicit_call_without_parent_does_not_initialize_profile(self):
        path = self.out_dir / "producer-profile-no-parent.csv"
        path.unlink(missing_ok=True)
        result = subprocess.run([str(self.exe), "no-parent", str(path)], cwd=ROOT,
            text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(path.exists())

    def test_bounded_ring_reports_overwrites_and_retained_span(self):
        metadata, rows = parse_csv(self.run_mode("overflow"))
        self.assertEqual(int(metadata["begun"]), int(metadata["completed"]) +
            int(metadata["late_discarded"]) + int(metadata["inflight_at_dump"]))
        self.assertEqual(metadata["begun"], "20")
        self.assertEqual(metadata["completed"], "20")
        self.assertEqual(metadata["overwritten"], "4")
        self.assertEqual(metadata["retained_rows"], "16")
        self.assertEqual(metadata["retained_completed_rows"], "16")
        self.assertEqual(metadata["retained_inflight_rows"], "0")
        self.assertEqual(metadata["retained_first_id"], "5")
        self.assertEqual(metadata["retained_last_id"], "20")
        self.assertGreater(int(metadata["max_discarded_end_qpc"]), 0)
        self.assertEqual([int(row["id"]) for row in rows], list(range(5, 21)))

    def test_clock_failures_are_separately_marked(self):
        metadata, rows = parse_csv(self.run_mode("invalid-clock"))
        self.assertEqual(metadata["begun"], "3")
        self.assertEqual(metadata["completed"], "3")
        self.assertEqual(rows[0]["qpc_valid"], "0")
        self.assertEqual(rows[0]["cycles_valid"], "1")
        self.assertEqual(rows[1]["qpc_valid"], "1")
        self.assertEqual(rows[1]["cycles_valid"], "0")
        self.assertEqual(rows[2]["qpc_valid"], "1")
        self.assertEqual(rows[2]["cycles_valid"], "0")
        self.assertEqual(rows[2]["cycles_delta"], "0")

    def test_concurrent_scopes_have_exact_once_ids(self):
        metadata, rows = parse_csv(self.run_mode("concurrent"))
        self.assertEqual(int(metadata["begun"]), int(metadata["completed"]) +
            int(metadata["late_discarded"]) + int(metadata["inflight_at_dump"]))
        self.assertEqual(metadata["begun"], "32")
        self.assertEqual(metadata["completed"], "32")
        self.assertEqual(metadata["overwritten"], "16")
        ids = [int(row["id"]) for row in rows]
        self.assertEqual(ids, list(range(17, 33)))
        self.assertEqual(len(set(ids)), len(ids))
        self.assertEqual(metadata["retained_rows"], "16")

    def test_dump_freezes_and_reports_inflight_scope_without_waiting(self):
        metadata, rows = parse_csv(self.run_mode("inflight"))
        self.assertEqual(int(metadata["begun"]), int(metadata["completed"]) +
            int(metadata["late_discarded"]) + int(metadata["inflight_at_dump"]))
        self.assertEqual(metadata["begun"], "1")
        self.assertEqual(metadata["completed"], "0")
        self.assertEqual(metadata["inflight_at_dump"], "1")
        self.assertEqual(metadata["retained_rows"], "1")
        self.assertEqual(metadata["retained_inflight_rows"], "1")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["completed"], "0")
        self.assertEqual(rows[0]["qpc_end"], "0")
        self.assertEqual(rows[0]["qpc_valid"], "0")

    def test_late_completion_cannot_corrupt_reused_ring_slot(self):
        metadata, rows = parse_csv(self.run_mode("overwrite-inflight"))
        self.assertEqual(int(metadata["begun"]), int(metadata["completed"]) +
            int(metadata["late_discarded"]) + int(metadata["inflight_at_dump"]))
        self.assertEqual(metadata["begun"], "17")
        self.assertEqual(metadata["completed"], "16")
        self.assertEqual(metadata["overwritten"], "1")
        self.assertEqual(metadata["overwritten_inflight"], "1")
        self.assertEqual(metadata["late_discarded"], "1")
        self.assertGreater(int(metadata["earliest_overwritten_inflight_begin_qpc"]), 0)
        self.assertEqual([int(row["id"]) for row in rows], list(range(2, 18)))
        self.assertTrue(all(row["completed"] == "1" for row in rows))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--output-dir", required=True)
    args, remaining = parser.parse_known_args()
    ProducerProfileTests.compiler_arg = args.compiler
    ProducerProfileTests.output_dir_arg = args.output_dir
    unittest.main(argv=[sys.argv[0], *remaining])
