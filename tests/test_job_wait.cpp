// Exercises the actual guarded native boundary with stub guest RAM. This is a
// contract test only; it makes no timing or performance claim.
#include "ppu_recomp.h"
#include "../port/haze_job_wait_policy.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

void haze_job_wait_call(ppu_context*);
bool haze_job_wait_usleep(ppu_context*);
void haze_job_wait_dump_counts(void);

static constexpr uint32_t kToc = 0x00E94EE0u;
static constexpr uint32_t kAnchorEa = kToc - 0x7D6Cu;
static constexpr uint32_t kAnchor = 0x00018000u;
static constexpr uint32_t kTableEa = kAnchor - 0x8000u;
static constexpr uint32_t kTable = 0x00010000u;
static constexpr uint32_t kSlot = 7u;
static constexpr uint32_t kStateEa = kTable + kSlot * 0x1450u + 8u;
static constexpr uint32_t kQueueRootEa = kToc - 0x7C1Cu;
static constexpr uint32_t kQueueRoot = 0x0001A000u;
static constexpr uint32_t kQueueTableEa = kQueueRoot - 0x7FD8u;
static constexpr uint32_t kQueueTable = 0x00014000u;
static constexpr uint32_t kQueueEa = 0x00020000u;
static constexpr uint32_t kQueueBuffer = 0x00030000u;
static uint8_t guest_memory[0x40000] = {};
extern "C" uint8_t* vm_base = guest_memory;

static unsigned range_calls;
static unsigned read32_calls;
static unsigned read8_calls;
static bool reject_range;
static bool native_memory_available = true;
static uint32_t anchor_value = kAnchor;
static uint32_t queue_ea_value = kQueueEa;
static uint32_t rejected_range_address;
static bool lock_held;
static unsigned lock_calls;
static unsigned unlock_calls;
static bool throw_inside;
static unsigned syscall_duration = 330;
static bool handled;
static unsigned original_syscalls;

extern "C" int ppu_native_memory_range(uint32_t address, uint32_t size) {
    ++range_calls;
    if (reject_range || !size || address == rejected_range_address) return 0;
    return address >= 0x10000u && static_cast<uint64_t>(address) + size <= 0xC0000000u;
}

extern "C" int ppu_native_memory_available(void) { return native_memory_available ? 1 : 0; }

extern "C" void spu_lockline_lock(void) {
    assert(!lock_held && "queue lock nested unexpectedly");
    lock_held = true;
    ++lock_calls;
}

extern "C" void spu_lockline_unlock(void) {
    assert(lock_held && "queue lock released without acquisition");
    lock_held = false;
    ++unlock_calls;
}

static uint32_t guest_be32(uint32_t address) {
    const uint8_t* p = guest_memory + address;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t guest_be64(uint32_t address) {
    return (uint64_t)guest_be32(address) << 32 | guest_be32(address + 4u);
}

static void set_guest_be32(uint32_t address, uint32_t value) {
    guest_memory[address] = static_cast<uint8_t>(value >> 24);
    guest_memory[address + 1u] = static_cast<uint8_t>(value >> 16);
    guest_memory[address + 2u] = static_cast<uint8_t>(value >> 8);
    guest_memory[address + 3u] = static_cast<uint8_t>(value);
}

static void set_guest_be64(uint32_t address, uint64_t value) {
    set_guest_be32(address, static_cast<uint32_t>(value >> 32));
    set_guest_be32(address + 4u, static_cast<uint32_t>(value));
}

extern "C" uint32_t vm_read32(uint64_t address) {
    ++read32_calls;
    if (read32_calls == 1) { assert(address == kAnchorEa); return anchor_value; }
    if (read32_calls == 2) { assert(address == kTableEa); return kTable; }
    if (read32_calls == 3) { assert(address == kQueueRootEa); return kQueueRoot; }
    if (read32_calls == 4) { assert(address == kQueueTableEa); return kQueueTable; }
    if (read32_calls == 5) { assert(address == kQueueTable); return queue_ea_value; }
    assert(lock_held && "queue header read must be protected by the lock-line lock");
    assert(address >= kQueueEa && address < kQueueEa + 128u);
    return guest_be32(static_cast<uint32_t>(address));
}

extern "C" uint64_t vm_read64(uint64_t address) {
    assert(lock_held && "queue buffer descriptor read must be locked");
    assert(address == queue_ea_value + 0x10u);
    return guest_be64(static_cast<uint32_t>(address));
}

extern "C" uint8_t vm_read8(uint64_t address) {
    ++read8_calls;
    assert(false && "unexpected guest byte read");
    return 0;
}

void func_0068EFC0(ppu_context* ctx) {
    ctx->lr = 0x0068EFF0;
    ctx->gpr[11] = 0x8D;
    ctx->gpr[3] = syscall_duration;
    handled = haze_job_wait_usleep(ctx);
    if (!handled) {
        ++original_syscalls;
        // Model the original syscall's result and preserve its requested time.
        ctx->gpr[3] = 0;
    }
    if (throw_inside) throw std::runtime_error("synthetic unwind");
}

static ppu_context caller() {
    ppu_context ctx = {};
    ctx.thread_id = 1;
    ctx.gpr[2] = kToc;
    ctx.lr = 0x00682574;
    ctx.gpr[31] = kSlot;
    return ctx;
}

static void reset_guest(uint8_t state, uint8_t completion) {
    range_calls = read32_calls = read8_calls = original_syscalls = 0;
    reject_range = false;
    native_memory_available = true;
    anchor_value = kAnchor;
    queue_ea_value = kQueueEa;
    rejected_range_address = 0;
    lock_held = false;
    lock_calls = unlock_calls = 0;
    std::memset(guest_memory, 0, sizeof(guest_memory));
    guest_memory[kStateEa] = state;
    guest_memory[kStateEa + 1u] = completion;
    set_guest_be32(kQueueEa + 0x08u, 16u);
    set_guest_be32(kQueueEa + 0x0Cu, 512u);
    set_guest_be64(kQueueEa + 0x10u, kQueueBuffer);
    set_guest_be32(kQueueEa + 0x1Cu, 1u);
    syscall_duration = 330;
    handled = false;
    throw_inside = false;
}

static void test_completion_queue() {
    uint8_t before[sizeof(guest_memory)];

    // Queue wraparound remains nonempty with head at the last index and tail at zero.
    reset_guest(1, 2);
    set_guest_be32(kQueueEa + 0x00u, 1023u);
    set_guest_be32(kQueueEa + 0x04u, 0u);
    std::memcpy(before, guest_memory, sizeof(guest_memory));
    auto wrapped = caller();
    haze_job_wait_call(&wrapped);
    assert(handled && original_syscalls == 0);
    assert(lock_calls == 1 && unlock_calls == 1 && !lock_held);
    assert(std::memcmp(before, guest_memory, sizeof(guest_memory)) == 0);

    // The routing flag alone never wakes the wait: an empty queue expires.
    reset_guest(1, 2);
    std::memcpy(before, guest_memory, sizeof(guest_memory));
    auto empty = caller();
    haze_job_wait_call(&empty);
    assert(handled && original_syscalls == 0 && lock_calls > 0 && lock_calls == unlock_calls);
    assert(std::memcmp(before, guest_memory, sizeof(guest_memory)) == 0);

    // A claimed/busy index is valid but not ready; it keeps waiting to deadline.
    reset_guest(1, 2);
    set_guest_be32(kQueueEa + 0x00u, 0xFFFFFFFFu); // ~0 encodes head index zero, busy.
    std::memcpy(before, guest_memory, sizeof(guest_memory));
    auto busy = caller();
    haze_job_wait_call(&busy);
    assert(handled && original_syscalls == 0 && lock_calls > 0 && lock_calls == unlock_calls);
    assert(std::memcmp(before, guest_memory, sizeof(guest_memory)) == 0);

    // No route flag means no queue-chain resolution or queue lock acquisition.
    reset_guest(1, 0);
    auto no_route = caller();
    haze_job_wait_call(&no_route);
    assert(handled && read32_calls == 2 && lock_calls == 0 && unlock_calls == 0);

    auto expect_invalid_queue_fallback = [&before](void (*configure)()) {
        reset_guest(1, 2);
        configure();
        std::memcpy(before, guest_memory, sizeof(guest_memory));
        auto ctx = caller();
        haze_job_wait_call(&ctx);
        assert(!handled && original_syscalls == 1);
        assert(lock_calls == unlock_calls && !lock_held);
        assert(std::memcmp(before, guest_memory, sizeof(guest_memory)) == 0);
    };
    expect_invalid_queue_fallback([] { queue_ea_value = kQueueEa + 1u; });
    expect_invalid_queue_fallback([] { rejected_range_address = kQueueEa; });
    expect_invalid_queue_fallback([] { rejected_range_address = kQueueBuffer; });
    expect_invalid_queue_fallback([] { set_guest_be32(kQueueEa + 0x08u, 32u); });
    expect_invalid_queue_fallback([] { set_guest_be32(kQueueEa + 0x0Cu, 0u); });
    expect_invalid_queue_fallback([] { set_guest_be32(kQueueEa + 0x0Cu, 0x40000000u); });
    expect_invalid_queue_fallback([] { set_guest_be64(kQueueEa + 0x10u, kQueueBuffer + 1u); });
    expect_invalid_queue_fallback([] { set_guest_be64(kQueueEa + 0x10u, 0xBFFFF000u); });
    expect_invalid_queue_fallback([] { set_guest_be32(kQueueEa + 0x1Cu, 2u); });
    expect_invalid_queue_fallback([] { set_guest_be32(kQueueEa + 0x00u, 1024u); });
    expect_invalid_queue_fallback([] { set_guest_be32(kQueueEa + 0x04u, 513u); });
}

static void test_policy() {
    using namespace haze_job_wait_policy;
    unsigned probes = 0, clocks = 0, pauses = 0;
    auto ready_entry = wait_until_ready_or_deadline(10, 5,
        [&] { ++probes; return ProbeResult::ready_primary; },
        [&] { ++clocks; return ClockSample{true, 10}; },
        [&] { ++pauses; });
    assert(ready_entry == WaitResult::ready_primary && probes == 1 && clocks == 0 && pauses == 0);

    probes = clocks = pauses = 0;
    auto ready_during_wait = wait_until_ready_or_deadline(100, 5,
        [&] { return ++probes == 1 ? ProbeResult::waiting : ProbeResult::ready_queue; },
        [&] { ++clocks; return ClockSample{true, 101}; },
        [&] { ++pauses; });
    assert(ready_during_wait == WaitResult::ready_queue && probes == 2 && clocks == 1 && pauses == 1);

    probes = clocks = pauses = 0;
    auto expired = wait_until_ready_or_deadline(100, 5,
        [&] { ++probes; return ProbeResult::waiting; },
        [&] { ++clocks; return ClockSample{true, clocks == 1 ? 104u : 105u}; },
        [&] { ++pauses; });
    assert(expired == WaitResult::expired && probes == 2 && clocks == 2 && pauses == 1);

    auto clock_error = wait_until_ready_or_deadline(100, 5,
        [] { return ProbeResult::waiting; },
        [] { return ClockSample{true, 99}; }, [] {});
    assert(clock_error == WaitResult::clock_error);
    auto invalid = wait_until_ready_or_deadline(100, 5,
        [] { return ProbeResult::invalid; },
        [] { return ClockSample{false, 0}; }, [] {});
    assert(invalid == WaitResult::invalid);
}

#ifdef _WIN32
static void test_final_counts() {
    FILE* capture = std::tmpfile();
    assert(capture);
    const int saved_stderr = _dup(_fileno(stderr));
    assert(saved_stderr >= 0);
    assert(_dup2(_fileno(capture), _fileno(stderr)) == 0);
    haze_job_wait_dump_counts();
    std::fflush(stderr);
    assert(_dup2(saved_stderr, _fileno(stderr)) == 0);
    _close(saved_stderr);

    std::rewind(capture);
    char line[512] = {};
    assert(std::fgets(line, sizeof(line), capture));
    unsigned long long native = 0, ready = 0, primary = 0, queue = 0;
    unsigned long long expired = 0, fallback = 0;
    assert(std::sscanf(line,
        "[haze-job-wait-counts] native=%llu ready=%llu ready_primary=%llu ready_queue=%llu expired=%llu fallback=%llu",
        &native, &ready, &primary, &queue, &expired, &fallback) == 6);
    std::fclose(capture);

    // Counts are per completed guarded delay, not per poll. Empty and busy
    // queue cases must be expired, while queue wrap/nonempty cases are ready.
    assert(native == 8 && ready == 3 && primary == 1 && queue == 2);
    assert(expired == 5 && fallback == 14);
}
#endif

#ifdef _WIN32
static void test_disabled_child(const char* executable) {
    _putenv_s("HAZE_NATIVE_JOB_WAIT", "0");
    reset_guest(0, 0);
    auto ctx = caller();
    haze_job_wait_call(&ctx);
    assert(!handled && original_syscalls == 1);
    assert(range_calls == 0 && read32_calls == 0 && read8_calls == 0);
    (void)executable;
}

static void run_disabled_child(const char* executable) {
    std::string command = "\"" + std::string(executable) + "\" --disabled-child";
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    assert(CreateProcessA(executable, mutable_command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    assert(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code = 1;
    assert(GetExitCodeProcess(process.hProcess, &code) && code == 0);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc > 1 && std::strcmp(argv[1], "--disabled-child") == 0) {
        test_disabled_child(argv[0]);
        return 0;
    }
#endif
    test_policy();
    // Exercise the production default; the child below checks explicit opt-out.
    _putenv_s("HAZE_NATIVE_JOB_WAIT", "");
    test_completion_queue();

    reset_guest(2, 0);
    auto ready_at_entry = caller();
    haze_job_wait_call(&ready_at_entry);
    assert(handled && ready_at_entry.gpr[3] == 0 && original_syscalls == 0);
    assert(range_calls == 3 && read32_calls == 2 && read8_calls == 0);

    reset_guest(1, 2);
    set_guest_be32(kQueueEa + 0x04u, 1u);
    auto compound_ready = caller();
    haze_job_wait_call(&compound_ready);
    assert(handled && compound_ready.gpr[3] == 0);
    assert(read32_calls >= 10 && read8_calls == 0 && original_syscalls == 0);
    assert(lock_calls == 1 && unlock_calls == 1 && !lock_held);

    reset_guest(0, 0);
    auto deadline = caller();
    haze_job_wait_call(&deadline);
    assert(handled && deadline.gpr[3] == 0 && read8_calls == 0);

    for (unsigned field = 0; field < 5; ++field) {
        reset_guest(2, 0);
        auto mismatch = caller();
        if (field == 0) mismatch.thread_id = 2;
        if (field == 1) mismatch.gpr[2]++;
        if (field == 2) mismatch.lr++;
        if (field == 3) mismatch.gpr[31] = 512;
        if (field == 4) syscall_duration = 331;
        haze_job_wait_call(&mismatch);
        assert(!handled && original_syscalls == 1);
        assert(range_calls == 0 && read32_calls == 0 && read8_calls == 0);
    }

    reset_guest(0, 0);
    reject_range = true;
    auto bad_range = caller();
    haze_job_wait_call(&bad_range);
    assert(!handled && original_syscalls == 1 && read8_calls == 0);

    reset_guest(0, 0);
    // An invalid/noncanonical anchor is rejected before the table read.
    anchor_value = 0x7000u;
    auto bad_pointer = caller();
    haze_job_wait_call(&bad_pointer);
    assert(!handled && original_syscalls == 1 && read32_calls == 1 && read8_calls == 0);

    reset_guest(2, 0);
    native_memory_available = false; // Runtime diagnostics force the ordinary syscall route.
    auto diagnostics_enabled = caller();
    haze_job_wait_call(&diagnostics_enabled);
    assert(!handled && original_syscalls == 1 && range_calls == 0 && read32_calls == 0 && read8_calls == 0);

    reset_guest(0, 0);
    throw_inside = true;
    auto unwound = caller();
    try { haze_job_wait_call(&unwound); assert(false); }
    catch (const std::runtime_error&) {}
    throw_inside = false;
    unwound.lr = 0x0068EFF0;
    unwound.gpr[11] = 0x8D;
    unwound.gpr[3] = 330;
    const auto before = unwound;
    assert(!haze_job_wait_usleep(&unwound));
    assert(std::memcmp(&unwound, &before, sizeof(unwound)) == 0);

#ifdef _WIN32
    run_disabled_child(argv[0]);
    test_final_counts();
#endif
    std::puts("PASS: readiness policy, queue validation/wrap/busy/fallback, non-mutation, exact delay guard, and scope unwind");
}
