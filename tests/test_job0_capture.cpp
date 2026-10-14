#include "../port/haze_job0_capture.h"

#if !defined(HAZE_JOB_TRACE) || !defined(_WIN32)
#error Build this test with HAZE_JOB_TRACE defined on Windows.
#endif

#include <Windows.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr unsigned kPcs[] = {0x3408u, 0x36F0u, 0x3F48u};
constexpr std::size_t kHeaderBytes = 56;
constexpr std::size_t kRegisterBytes = 128u * 16u;
constexpr std::size_t kLocalStoreBytes = 256u * 1024u;

std::uint32_t read_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t read_u64_le(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(read_u32_le(p)) |
        (static_cast<std::uint64_t>(read_u32_le(p + 4)) << 32);
}

std::uint64_t expected_hash(std::uint8_t pattern) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i != kRegisterBytes + kLocalStoreBytes; ++i) {
        hash ^= pattern;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t payload_hash(const std::uint8_t* bytes, std::size_t count) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i != count; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool make_temp_directory(char* dir, std::size_t capacity) {
    char temp[MAX_PATH]{};
    const DWORD length = GetTempPathA(MAX_PATH, temp);
    if (!length || length >= MAX_PATH) return false;
    char path[MAX_PATH]{};
    if (!GetTempFileNameA(temp, "hj0", 0, path)) return false;
    DeleteFileA(path);
    if (!CreateDirectoryA(path, nullptr)) return false;
    if (std::strlen(path) + 1 > capacity) {
        RemoveDirectoryA(path);
        return false;
    }
    std::strcpy(dir, path);
    return true;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string capture_path(const char* dir, unsigned pc, unsigned index) {
    char path[MAX_PATH]{};
    const int n = std::snprintf(path, sizeof(path), "%s\\haze-job0-pc%08X-%u.bin",
        dir, pc, index);
    assert(n > 0 && static_cast<std::size_t>(n) < sizeof(path));
    return path;
}

void check_capture(const std::string& path, unsigned pc, unsigned index,
    std::uint8_t expected_pattern) {
    const std::string data = read_file(path);
    assert(data.size() == kHeaderBytes + kRegisterBytes + kLocalStoreBytes);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::uint8_t magic[] = {'H', 'Z', 'J', '0', 'C', 'A', 'P', 0};
    assert(std::memcmp(bytes, magic, sizeof(magic)) == 0);
    assert(read_u32_le(bytes + 8) == 1);
    assert(read_u32_le(bytes + 12) == kHeaderBytes);
    assert(read_u32_le(bytes + 16) == pc);
    assert(read_u32_le(bytes + 20) == kRegisterBytes);
    assert(read_u32_le(bytes + 24) == kLocalStoreBytes);
    assert(read_u32_le(bytes + 32) == index);
    assert(read_u64_le(bytes + 44) == expected_hash(expected_pattern));
    assert(read_u32_le(bytes + 52) == 0);
    for (std::size_t i = kHeaderBytes; i != data.size(); ++i)
        assert(bytes[i] == expected_pattern);
}

bool run_concurrent_child() {
    char exe[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return false;
    std::string command = "\"" + std::string(exe) + "\" --concurrent-child";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, &command[0], nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process)) return false;
    const DWORD wait_result = WaitForSingleObject(process.hProcess, 30000);
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 9);
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

bool check_concurrent_capture(const std::string& path, unsigned pc,
    unsigned site, unsigned index) {
    const std::string data = read_file(path);
    if (data.size() != kHeaderBytes + kRegisterBytes + kLocalStoreBytes) return false;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::uint8_t magic[] = {'H', 'Z', 'J', '0', 'C', 'A', 'P', 0};
    if (std::memcmp(bytes, magic, sizeof(magic)) != 0 ||
        read_u32_le(bytes + 8) != 1 || read_u32_le(bytes + 12) != kHeaderBytes ||
        read_u32_le(bytes + 16) != pc || read_u32_le(bytes + 20) != kRegisterBytes ||
        read_u32_le(bytes + 24) != kLocalStoreBytes || read_u32_le(bytes + 32) != index ||
        read_u32_le(bytes + 52) != 0)
        return false;

    const std::uint8_t* payload = bytes + kHeaderBytes;
    const std::uint8_t pattern = payload[0];
    const std::uint8_t minimum = static_cast<std::uint8_t>(0x60 + site * 16);
    if (pattern < minimum || pattern > minimum + 8) return false;
    for (std::size_t i = 1; i != kRegisterBytes + kLocalStoreBytes; ++i)
        if (payload[i] != pattern) return false;
    return read_u64_le(bytes + 44) == payload_hash(payload,
        kRegisterBytes + kLocalStoreBytes);
}

int concurrent_child() {
    char dir[MAX_PATH]{};
    if (!make_temp_directory(dir, sizeof(dir))) return 10;
    const std::string gate = std::string(dir) + "\\capture.gate";
    if (_putenv_s("HAZE_JOB0_CAPTURE_DIR", dir) != 0 ||
        _putenv_s("HAZE_JOB0_CAPTURE_GATE", gate.c_str()) != 0)
        return 11;
    HANDLE gate_file = CreateFileA(gate.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (gate_file == INVALID_HANDLE_VALUE) return 12;
    CloseHandle(gate_file);

    // Seed one complete capture for each PC, then race the remaining per-site
    // capacity against shutdown dump while producer threads are active.
    std::uint8_t registers[kRegisterBytes];
    std::uint8_t ls[kLocalStoreBytes];
    for (unsigned site = 0; site != 3; ++site) {
        const auto pattern = static_cast<std::uint8_t>(0x60 + site * 16);
        std::memset(registers, pattern, sizeof(registers));
        std::memset(ls, pattern, sizeof(ls));
        haze_job0_capture_input(kPcs[site], registers, ls);
    }

    constexpr unsigned kThreadsPerSite = 8;
    std::atomic<unsigned> started{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> producers;
    producers.reserve(3 * kThreadsPerSite);
    for (unsigned site = 0; site != 3; ++site) {
        for (unsigned worker = 0; worker != kThreadsPerSite; ++worker) {
            producers.emplace_back([&, site, worker] {
                std::vector<std::uint8_t> thread_registers(kRegisterBytes);
                std::vector<std::uint8_t> thread_ls(kLocalStoreBytes);
                const auto pattern = static_cast<std::uint8_t>(0x61 + site * 16 + worker);
                std::memset(thread_registers.data(), pattern, thread_registers.size());
                std::memset(thread_ls.data(), pattern, thread_ls.size());
                started.fetch_add(1, std::memory_order_release);
                while (!release.load(std::memory_order_acquire)) SwitchToThread();
                haze_job0_capture_input(kPcs[site], thread_registers.data(), thread_ls.data());
            });
        }
    }
    while (started.load(std::memory_order_acquire) != 3 * kThreadsPerSite) SwitchToThread();
    release.store(true, std::memory_order_release);
    haze_job0_capture_dump();
    for (auto& producer : producers) producer.join();

    // Calls after freeze must not change files or claim a later slot.
    for (unsigned site = 0; site != 3; ++site)
        haze_job0_capture_input(kPcs[site], registers, ls);
    haze_job0_capture_dump();

    unsigned complete_files = 0;
    for (unsigned site = 0; site != 3; ++site) {
        bool saw_gap = false;
        for (unsigned index = 0; index != 5; ++index) {
            const std::string path = capture_path(dir, kPcs[site], index);
            const DWORD attributes = GetFileAttributesA(path.c_str());
            const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
            if (index == 4 && exists) return 13;
            if (index < 4) {
                if (!exists) saw_gap = true;
                else {
                    if (saw_gap || !check_concurrent_capture(path, kPcs[site], site, index))
                        return 14;
                    ++complete_files;
                }
            }
        }
    }
    if (complete_files < 3) return 15;

    for (unsigned site = 0; site != 3; ++site)
        for (unsigned index = 0; index != 4; ++index)
            DeleteFileA(capture_path(dir, kPcs[site], index).c_str());
    DeleteFileA(gate.c_str());
    RemoveDirectoryA(dir);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--concurrent-child") == 0)
        return concurrent_child();

    char dir[MAX_PATH]{};
    assert(make_temp_directory(dir, sizeof(dir)));
    const std::string gate = std::string(dir) + "\\capture.gate";
    assert(_putenv_s("HAZE_JOB0_CAPTURE_DIR", dir) == 0);
    assert(_putenv_s("HAZE_JOB0_CAPTURE_GATE", gate.c_str()) == 0);

    std::uint8_t registers[kRegisterBytes]{};
    std::uint8_t ls[kLocalStoreBytes]{};
    // The first call initializes the recorder while the gate is absent.
    haze_job0_capture_input(kPcs[0], registers, ls);

    HANDLE gate_file = CreateFileA(gate.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(gate_file != INVALID_HANDLE_VALUE);
    CloseHandle(gate_file);
    Sleep(300); // The shared gate poll is deliberately rate-limited to 250 ms.

    haze_job0_capture_input(0xFFFFu, registers, ls); // unsupported sites are ignored
    for (unsigned site = 0; site != 3; ++site) {
        for (unsigned index = 0; index != 5; ++index) {
            const std::uint8_t pattern = static_cast<std::uint8_t>(0x20 + site * 8 + index);
            std::memset(registers, pattern, sizeof(registers));
            std::memset(ls, pattern, sizeof(ls));
            haze_job0_capture_input(kPcs[site], registers, ls);
        }
    }

    // An existing fixture path must remain untouched by exclusive-create dump.
    const std::string protected_path = capture_path(dir, kPcs[0], 0);
    HANDLE protected_file = CreateFileA(protected_path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(protected_file != INVALID_HANDLE_VALUE);
    const char sentinel[] = "keep-existing-fixture";
    DWORD written = 0;
    assert(WriteFile(protected_file, sentinel, sizeof(sentinel), &written, nullptr));
    assert(written == sizeof(sentinel));
    CloseHandle(protected_file);

    haze_job0_capture_dump();
    assert(read_file(protected_path) == std::string(sentinel, sizeof(sentinel)));
    for (unsigned site = 0; site != 3; ++site) {
        for (unsigned index = 0; index != 4; ++index) {
            if (site == 0 && index == 0) continue;
            check_capture(capture_path(dir, kPcs[site], index), kPcs[site], index,
                static_cast<std::uint8_t>(0x20 + site * 8 + index));
        }
        assert(GetFileAttributesA(capture_path(dir, kPcs[site], 4).c_str()) == INVALID_FILE_ATTRIBUTES);
    }

    for (unsigned site = 0; site != 3; ++site)
        for (unsigned index = 0; index != 5; ++index)
            DeleteFileA(capture_path(dir, kPcs[site], index).c_str());
    DeleteFileA(gate.c_str());
    RemoveDirectoryA(dir);
    _putenv_s("HAZE_JOB0_CAPTURE_DIR", "");
    _putenv_s("HAZE_JOB0_CAPTURE_GATE", "");
    assert(run_concurrent_child());
    return 0;
}
