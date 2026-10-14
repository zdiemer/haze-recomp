#include "../port/haze_job_trace.h"

#if !defined(HAZE_JOB_TRACE) || !defined(_WIN32)
#error Build this test with HAZE_JOB_TRACE defined on Windows.
#endif
#if !defined(HAZE_JOB_TRACE_TEST_CAPACITY) || HAZE_JOB_TRACE_TEST_CAPACITY < 64
#error Build this test with HAZE_JOB_TRACE_TEST_CAPACITY >= 64.
#endif

#include <Windows.h>

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr unsigned kSubmit = static_cast<unsigned>(HazeJobTraceKind::Submit);
constexpr unsigned kDispatch = static_cast<unsigned>(HazeJobTraceKind::Dispatch);
constexpr unsigned kKernel = static_cast<unsigned>(HazeJobTraceKind::Kernel);
constexpr unsigned kWait = static_cast<unsigned>(HazeJobTraceKind::Wait);
constexpr unsigned kHandler = static_cast<unsigned>(HazeJobTraceKind::Handler);

bool run_disabled_child() {
    char exe[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return false;
    std::string command = "\"" + std::string(exe) + "\" --disabled-child";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
        &startup, &process)) return false;
    const DWORD wait_result = WaitForSingleObject(process.hProcess, 30000);
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 7);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

bool run_freeze_child() {
    char exe[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return false;
    std::string command = "\"" + std::string(exe) + "\" --freeze-child";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
        &startup, &process)) return false;
    const DWORD wait_result = WaitForSingleObject(process.hProcess, 30000);
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 7);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

bool make_temp_path(char* path, std::size_t capacity) {
    char directory[MAX_PATH]{};
    const DWORD length = GetTempPathA(MAX_PATH, directory);
    if (!length || length >= MAX_PATH) return false;
    if (!GetTempFileNameA(directory, "hjt", 0, path)) return false;
    DeleteFileA(path);
    return std::strlen(path) < capacity;
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> values;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = line.find(',', begin);
        if (end == std::string::npos) {
            values.push_back(line.substr(begin));
            break;
        }
        values.push_back(line.substr(begin, end - begin));
        begin = end + 1;
        if (begin == line.size()) {
            values.emplace_back();
            break;
        }
    }
    return values;
}

std::uint64_t number(const std::vector<std::string>& row, std::size_t column) {
    assert(column < row.size());
    return std::strtoull(row[column].c_str(), nullptr, 10);
}

int disabled_child() {
    if (_putenv_s("HAZE_JOB_TRACE_CSV", "") != 0) return 1;
    HazeJobTraceScope scope(kHandler, 1, 2, 3,
        reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(1)), 8);
    scope.set_slot(7);
    scope.set_dispatch(8, 9);
    haze_job_trace_dump();
    return 0;
}

std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int freeze_child() {
    char path[MAX_PATH]{};
    if (!make_temp_path(path, sizeof(path)) ||
        _putenv_s("HAZE_JOB_TRACE_CSV", path) != 0) return 2;

    std::atomic<bool> release{false};
    std::atomic<unsigned> started{0};
    std::atomic<unsigned> produced{0};
    std::thread producers[4];
    for (unsigned i = 0; i < 4; ++i) {
        producers[i] = std::thread([&, i] {
            HazeJobTraceScope pinned_parent(kHandler, 60 + i, 0x9000 + i, 0x9010 + i, nullptr, 0);
            started.fetch_add(1, std::memory_order_release);
            std::uint64_t sequence = 0;
            while (!release.load(std::memory_order_acquire)) {
                const std::uint64_t args[] = {i, sequence++};
                {
                    HazeJobTraceScope scope(kKernel, 60 + i, 0xa000 + i, 0xa010 + i, args, 2);
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (started.load(std::memory_order_acquire) != 4) SwitchToThread();
    while (produced.load(std::memory_order_relaxed) < 4) SwitchToThread();

    HazeJobTraceScope active_parent(kHandler, 70, 0xb000, 0xb010, nullptr, 0);
    haze_job_trace_dump();
    const std::string frozen_file = read_file(path);
    release.store(true, std::memory_order_release);
    for (auto& producer : producers) producer.join();
    const std::string after_join = read_file(path);
    DeleteFileA(path);
    if (frozen_file.empty() || frozen_file != after_join) return 3;

    std::istringstream input(after_join);
    std::string line;
    if (!std::getline(input, line)) return 4;
    unsigned events = 0;
    std::vector<std::string> summary;
    while (std::getline(input, line)) {
        auto row = split(line);
        if (!row.empty() && row[0] == "summary") {
            summary = std::move(row);
            continue;
        }
        if (row.empty() || row[0] != "event" || row.size() != 36) return 5;
        ++events;
    }
    if (events == 0 || events > HAZE_JOB_TRACE_CAPACITY || summary.size() != 36 ||
        number(summary, 31) == 0) return 6;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--disabled-child") return disabled_child();
    if (argc > 1 && std::string(argv[1]) == "--freeze-child") return freeze_child();
    assert(run_disabled_child());

    char path[MAX_PATH]{};
    assert(make_temp_path(path, sizeof(path)));
    assert(_putenv_s("HAZE_JOB_TRACE_CSV", path) == 0);

    // More than one ring length verifies sequence-based wrap and retention.
    for (unsigned i = 0; i < HAZE_JOB_TRACE_CAPACITY * 2u + 3u; ++i) {
        const std::uint64_t args[] = {i, i + 1};
        HazeJobTraceScope scope(kKernel, 10, 0x1000 + i, 0x2000 + i, args, 2);
    }

    // Concurrent producers write independent completed spans.
    std::thread producers[4];
    for (unsigned i = 0; i < 4; ++i) {
        producers[i] = std::thread([i] {
            const std::uint64_t args[] = {0xabc000u + i};
            HazeJobTraceScope scope(kDispatch, 40 + i, 0x3000 + i, 0x4000 + i, args, 1);
            scope.set_dispatch(0x5000 + i, 0x6000 + i);
        });
    }
    for (auto& producer : producers) producer.join();

    // A submit establishes a per-slot generation inherited by its nested calls.
    LARGE_INTEGER interval_begin{}, interval_end{};
    {
        const std::uint64_t args[] = {0x11, 0x22, 0x33};
        HazeJobTraceScope submit(kSubmit, 3, 0x7000, 0x7100, args, 3);
        submit.set_slot(7);
        submit.set_route(2);
        submit.set_slot_base(0x12340000);
        QueryPerformanceCounter(&interval_begin);
        QueryPerformanceCounter(&interval_end);
        haze_job_trace_interval(static_cast<unsigned>(HazeJobTraceKind::SpuCreateThread),
            interval_begin.QuadPart, interval_end.QuadPart, args, 3);
        {
            HazeJobTraceScope dispatch(kDispatch, 3, 0x7200, 0x7210, nullptr, 0);
            dispatch.set_dispatch(0xdead, 0xbeef);
            {
                HazeJobTraceScope handler(kHandler, 3, 0x7300, 0x7310, nullptr, 0);
            }
        }
        {
            const std::uint64_t wait_args[] = {7};
            HazeJobTraceScope wait(kWait, 3, 0x7400, 0x7410, wait_args, 1);
            wait.set_slot(7);
        }
    }
    {
        HazeJobTraceScope submit(kSubmit, 3, 0x7500, 0x7510, nullptr, 0);
        submit.set_slot(7);
        HazeJobTraceScope wait(kWait, 3, 0x7600, 0x7610, nullptr, 0);
        wait.set_slot(7);
    }
    {
        HazeJobTraceScope invalid(kHandler, 8, 0x7700, 0x7710, nullptr, 0);
        invalid.set_slot(512);
    }

    // Dump must not wait for the still-active parent; its completed child marks
    // the absent parent explicitly in the CSV.
    HazeJobTraceScope active_parent(kHandler, 9, 0x8000, 0x8010, nullptr, 0);
    {
        HazeJobTraceScope completed_child(kKernel, 9, 0x8100, 0x8110, nullptr, 0);
    }
    haze_job_trace_dump();

    std::ifstream input(path);
    assert(input.good());
    std::string line;
    assert(std::getline(input, line));
    assert(line.find("parent_missing_in_dump") != std::string::npos);
    std::vector<std::vector<std::string>> events;
    std::vector<std::string> summary;
    std::uint64_t prior_ticket = 0;
    bool first_ticket = true;
    while (std::getline(input, line)) {
        auto row = split(line);
        assert(row.size() == 36);
        if (row[0] == "summary") {
            summary = std::move(row);
            continue;
        }
        assert(row[0] == "event");
        assert(number(row, 2) <= number(row, 3));
        assert(number(row, 9) <= 8);
        const auto ticket = number(row, 1);
        assert(first_ticket || ticket > prior_ticket);
        first_ticket = false;
        prior_ticket = ticket;
        events.push_back(std::move(row));
    }
    assert(!summary.empty());
    assert(number(summary, 29) > 0); // older issued tickets outside retained window
    assert(number(summary, 31) >= 1); // active outer scope at dump
    assert(number(summary, 32) > 0); // maximum end QPC among discarded records
    assert(number(summary, 33) > 0); // QPC frequency for offline duration conversion

    bool saw_parallel[4]{};
    bool saw_invalid_slot = false;
    bool saw_submit_gen1 = false, saw_submit_gen2 = false;
    bool saw_wait_gen1 = false, saw_wait_gen2 = false;
    bool saw_dispatch_result = false, saw_missing_parent = false;
    bool saw_external_interval = false;
    std::uint64_t submit_span = 0, dispatch_span = 0, handler_span = 0;
    std::uint64_t dispatch_parent = 0, handler_parent = 0;
    for (const auto& row : events) {
        const auto kind = number(row, 6);
        const auto guest_thread = number(row, 5);
        if (kind == static_cast<unsigned>(HazeJobTraceKind::SpuCreateThread)) {
            saw_external_interval = true;
            assert(number(row, 2) == static_cast<std::uint64_t>(interval_begin.QuadPart));
            assert(number(row, 3) == static_cast<std::uint64_t>(interval_end.QuadPart));
            assert(number(row, 34) == 2);
            assert(number(row, 35) == 0x12340000);
            assert(number(row, 18) == 7 && number(row, 19) == 1);
        }
        if (kind == kHandler && guest_thread == 8) {
            saw_invalid_slot = true;
            assert(number(row, 18) == 0xffffffffu);
        }
        if (kind == kDispatch && guest_thread >= 40 && guest_thread < 44) {
            saw_parallel[guest_thread - 40] = true;
            assert(number(row, 20) == 0x5000 + guest_thread - 40);
            assert(number(row, 21) == 0x6000 + guest_thread - 40);
        }
        if (kind == kSubmit && guest_thread == 3 && number(row, 18) == 7) {
            const auto generation = number(row, 19);
            if (generation == 1) { saw_submit_gen1 = true; submit_span = number(row, 22); }
            if (generation == 2) saw_submit_gen2 = true;
        }
        if (kind == kWait && guest_thread == 3 && number(row, 18) == 7) {
            if (number(row, 19) == 1) saw_wait_gen1 = true;
            if (number(row, 19) == 2) saw_wait_gen2 = true;
        }
        if (kind == kDispatch && guest_thread == 3 && number(row, 20) == 0xdead) {
            assert(number(row, 34) == 2);
            saw_dispatch_result = true;
            dispatch_span = number(row, 22);
            dispatch_parent = number(row, 23);
            assert(number(row, 24) == 1);
        }
        if (kind == kHandler && guest_thread == 3) {
            handler_span = number(row, 22);
            handler_parent = number(row, 23);
            assert(number(row, 24) == 2);
        }
        if (kind == kKernel && guest_thread == 9 && number(row, 25) == 1)
            saw_missing_parent = true;
    }
    for (bool saw : saw_parallel) assert(saw);
    assert(saw_invalid_slot);
    assert(saw_submit_gen1 && saw_submit_gen2 && saw_wait_gen1 && saw_wait_gen2);
    assert(saw_dispatch_result && handler_span != 0 && saw_missing_parent);
    assert(saw_external_interval);
    assert(dispatch_parent == submit_span && handler_parent == dispatch_span);

    DeleteFileA(path);
    assert(run_freeze_child());
    std::cout << "job trace tests passed: wrapped ring, 4 parallel producers, generations, nesting, active-scope dump, disabled no-op\n";
    return 0;
}
