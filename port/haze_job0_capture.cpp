#include "haze_job0_capture.h"

#if defined(HAZE_JOB_TRACE) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr unsigned kCaptureCountPerSite = 4;
constexpr unsigned kSiteCount = 3;
constexpr unsigned kRecordCount = kCaptureCountPerSite * kSiteCount;
constexpr std::size_t kRegisterBytes = 128u * 16u;
constexpr std::size_t kLocalStoreBytes = 256u * 1024u;
constexpr std::size_t kDirectoryBytes = 2048;
constexpr std::size_t kGatePathBytes = 2048;
constexpr std::uint64_t kGatePollMs = 250;

constexpr unsigned kCapturePcs[kSiteCount] = {0x3408u, 0x36F0u, 0x3F48u};

struct CaptureRecord {
    std::atomic<unsigned> ready{0};
    unsigned pc = 0;
    unsigned host_tid = 0;
    unsigned capture_index = 0;
    std::uint64_t qpc = 0;
    std::uint8_t registers[kRegisterBytes]{};
    std::uint8_t local_store[kLocalStoreBytes]{};
};

static_assert(sizeof(CaptureRecord) * kRecordCount < 4u * 1024u * 1024u,
    "job0 capture storage must stay below 4 MiB");

CaptureRecord g_records[kRecordCount];
std::atomic<unsigned> g_site_counts[kSiteCount]{};
std::atomic<unsigned> g_init_state{0};
// Store GetTickCount64()+1 so zero remains the "never checked" sentinel.
std::atomic<std::uint64_t> g_last_gate_poll_ms_plus_one{0};
std::atomic<bool> g_gate_open{false};
std::atomic<bool> g_accepting{false};
std::atomic<unsigned> g_writers{0};
std::atomic<bool> g_dump_started{false};
bool g_configured = false;
char g_directory[kDirectoryBytes]{};
char g_gate_path[kGatePathBytes]{};

bool initialize(bool wait_for_initializer) noexcept {
    unsigned state = g_init_state.load(std::memory_order_acquire);
    if (state == 2) return true;

    unsigned expected = 0;
    if (g_init_state.compare_exchange_strong(expected, 1,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        const char* directory = std::getenv("HAZE_JOB0_CAPTURE_DIR");
        const char* gate_path = std::getenv("HAZE_JOB0_CAPTURE_GATE");
        if (directory && directory[0] && gate_path && gate_path[0] &&
            std::strlen(directory) < sizeof(g_directory) &&
            std::strlen(gate_path) < sizeof(g_gate_path)) {
            std::strcpy(g_directory, directory);
            std::strcpy(g_gate_path, gate_path);
            g_configured = true;
            g_accepting.store(true, std::memory_order_seq_cst);
        }
        g_init_state.store(2, std::memory_order_release);
        return true;
    } else {
        if (!wait_for_initializer) return false;
        while (g_init_state.load(std::memory_order_acquire) != 2) YieldProcessor();
        return true;
    }
}

int site_index(unsigned pc) noexcept {
    for (unsigned i = 0; i != kSiteCount; ++i)
        if (kCapturePcs[i] == pc) return static_cast<int>(i);
    return -1;
}

bool gate_file_exists() noexcept {
    const DWORD attributes = GetFileAttributesA(g_gate_path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool gate_is_open() noexcept {
    if (!g_configured) return false;
    const std::uint64_t now = GetTickCount64();
    std::uint64_t prior = g_last_gate_poll_ms_plus_one.load(std::memory_order_relaxed);
    const bool due = prior == 0 || (now >= prior - 1 && now - (prior - 1) >= kGatePollMs);
    if (due) {
        if (g_last_gate_poll_ms_plus_one.compare_exchange_strong(prior, now + 1,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            g_gate_open.store(gate_file_exists(), std::memory_order_release);
        }
    }
    return g_gate_open.load(std::memory_order_acquire);
}

bool enter_writer() noexcept {
    if (!g_accepting.load(std::memory_order_seq_cst)) return false;
    g_writers.fetch_add(1, std::memory_order_seq_cst);
    if (!g_accepting.load(std::memory_order_seq_cst)) {
        g_writers.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }
    return true;
}

void leave_writer() noexcept {
    g_writers.fetch_sub(1, std::memory_order_seq_cst);
}

int reserve_site_record(unsigned site) noexcept {
    unsigned index = g_site_counts[site].load(std::memory_order_relaxed);
    while (index < kCaptureCountPerSite) {
        if (g_site_counts[site].compare_exchange_weak(index, index + 1,
                std::memory_order_relaxed, std::memory_order_relaxed))
            return static_cast<int>(index);
    }
    return -1;
}

void put_u32_le(std::uint8_t* dst, std::uint32_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>(value);
    dst[1] = static_cast<std::uint8_t>(value >> 8);
    dst[2] = static_cast<std::uint8_t>(value >> 16);
    dst[3] = static_cast<std::uint8_t>(value >> 24);
}

void put_u64_le(std::uint8_t* dst, std::uint64_t value) noexcept {
    put_u32_le(dst, static_cast<std::uint32_t>(value));
    put_u32_le(dst + 4, static_cast<std::uint32_t>(value >> 32));
}

bool write_all(HANDLE file, const void* data, std::size_t bytes) noexcept {
    const std::uint8_t* cursor = static_cast<const std::uint8_t*>(data);
    while (bytes != 0) {
        const DWORD request = static_cast<DWORD>((bytes > 1024u * 1024u)
            ? 1024u * 1024u : bytes);
        DWORD written = 0;
        if (!WriteFile(file, cursor, request, &written, nullptr) || written == 0)
            return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

bool make_path(char* path, std::size_t capacity, const CaptureRecord& record) noexcept {
    const std::size_t directory_len = std::strlen(g_directory);
    const bool has_separator = directory_len != 0 &&
        (g_directory[directory_len - 1] == '\\' || g_directory[directory_len - 1] == '/');
    const int result = std::snprintf(path, capacity,
        has_separator ? "%shaze-job0-pc%08X-%u.bin" : "%s\\haze-job0-pc%08X-%u.bin",
        g_directory, record.pc, record.capture_index);
    return result > 0 && static_cast<std::size_t>(result) < capacity;
}

void dump_record(const CaptureRecord& record) noexcept {
    char path[kDirectoryBytes + 96]{};
    if (!make_path(path, sizeof(path), record)) return;

    // Binary v1: a 56-byte little-endian header followed by the host-native
    // 128x16-byte GPR image and the raw 256 KiB local store. The register and
    // LS payloads are copied without lane or guest-endian conversion.
    std::uint64_t hash = 14695981039346656037ull;
    const auto hash_bytes = [&hash](const std::uint8_t* bytes, std::size_t count) noexcept {
        for (std::size_t i = 0; i != count; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    hash_bytes(record.registers, sizeof(record.registers));
    hash_bytes(record.local_store, sizeof(record.local_store));

    std::uint8_t header[56]{};
    const std::uint8_t magic[8] = {'H', 'Z', 'J', '0', 'C', 'A', 'P', 0};
    std::memcpy(header, magic, sizeof(magic));
    put_u32_le(header + 8, 1); // version
    put_u32_le(header + 12, sizeof(header));
    put_u32_le(header + 16, record.pc);
    put_u32_le(header + 20, static_cast<std::uint32_t>(kRegisterBytes));
    put_u32_le(header + 24, static_cast<std::uint32_t>(kLocalStoreBytes));
    put_u32_le(header + 28, record.host_tid);
    put_u32_le(header + 32, record.capture_index);
    put_u64_le(header + 36, record.qpc);
    put_u64_le(header + 44, hash); // FNV-1a 64 of raw registers followed by raw LS.
    put_u32_le(header + 52, 0); // reserved

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return; // Never replace an existing fixture.
    const bool ok = write_all(file, header, sizeof(header)) &&
        write_all(file, record.registers, sizeof(record.registers)) &&
        write_all(file, record.local_store, sizeof(record.local_store));
    CloseHandle(file);
    if (!ok) DeleteFileA(path);
}

} // namespace

extern "C" void haze_job0_capture_input(unsigned pc, const void* registers,
    const void* ls) {
    const int site = site_index(pc);
    if (site < 0 || !registers || !ls) return;
    if (g_site_counts[site].load(std::memory_order_relaxed) >= kCaptureCountPerSite)
        return;

    // A concurrent first-use initializer may make this one sample miss; the
    // hot path never spins waiting for environment setup.
    if (!initialize(false)) return;
    if (!gate_is_open() || !enter_writer()) return;

    const int reserved_index = reserve_site_record(static_cast<unsigned>(site));
    if (reserved_index >= 0) {
        const unsigned index = static_cast<unsigned>(reserved_index);
        CaptureRecord& record = g_records[static_cast<unsigned>(site) * kCaptureCountPerSite + index];
        record.pc = pc;
        record.host_tid = GetCurrentThreadId();
        record.capture_index = index;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        record.qpc = static_cast<std::uint64_t>(now.QuadPart);
        std::memcpy(record.registers, registers, sizeof(record.registers));
        std::memcpy(record.local_store, ls, sizeof(record.local_store));
        record.ready.store(1, std::memory_order_release);
    }

    leave_writer();
}

extern "C" void haze_job0_capture_dump(void) {
    if (!initialize(true)) return;
    if (!g_configured || g_dump_started.exchange(true, std::memory_order_acq_rel)) return;

    g_accepting.store(false, std::memory_order_seq_cst);
    while (g_writers.load(std::memory_order_seq_cst) != 0) SwitchToThread();

    for (unsigned i = 0; i != kRecordCount; ++i) {
        const CaptureRecord& record = g_records[i];
        if (record.ready.load(std::memory_order_acquire) == 1) dump_record(record);
    }
}

#endif
