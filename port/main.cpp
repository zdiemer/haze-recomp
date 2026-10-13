/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include "haze_wasm_vm.h"
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif
#include "haze_input.h"
#include "haze_camera.h"
#include "haze_graphics.h"
#include "haze_display.h"
#include "haze_pump_profile.h"

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#ifdef HAZE_REQUIRE_FMA
#include <intrin.h>
#endif
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
void     cellGame_init_from_paramsfo(const char* path);
int32_t  cellSaveData_set_root_path(const char* path);
void     cellSaveData_set_strict_load_errors(int enabled);
void     ppu_report_guest_lrs(void);
extern uint32_t g_last_hle_nid;
extern const char* g_last_hle_name;
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
uint32_t cellGcmGetCurrentDisplayBufferId(void);
void cellGcm_fifo_kick_wait(unsigned ms);
int cellGcm_fifo_has_queued_work(void);
}

void ps3_sampler_start(void); /* SDK profiler has C++ linkage. */
#ifdef __EMSCRIPTEN__
extern "C" void ppu_report_guest_lrs(void);
extern "C" EMSCRIPTEN_KEEPALIVE void haze_wasm_report(void) {
    ppu_report_guest_lrs();
}
#endif

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(__EMSCRIPTEN__) && defined(HAZE_WASM_WEBGL)
extern "C" int haze_webgl_backend_init(uint32_t,uint32_t,const char*);
extern "C" void haze_webgl_backend_present(void);
extern "C" int haze_webgl_backend_pump_messages(void);
#define rsx_backend_init haze_webgl_backend_init
#define rsx_backend_present haze_webgl_backend_present
#define rsx_backend_pump haze_webgl_backend_pump_messages
#define RSX_BACKEND_NAME "WebGL2"
#elif defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
#ifdef _WIN32
extern "C" {
int rsx_live_draw_enabled(void);
void rsx_live_draw_set_render_reference(uint32_t, uint32_t);
void rsx_live_draw_set_shadow_reference(uint32_t);
int rsx_live_draw_init(void*, uint32_t, uint32_t,
    const uint8_t* (*)(void*, uint32_t, uint32_t, uint32_t), void*);
void rsx_live_draw_present(uint32_t);
void rsx_live_draw_dump_present_samples(void);
int rsx_null_backend_init(uint32_t, uint32_t, const char*);
int rsx_null_backend_pump_messages(void);
void* rsx_null_backend_get_hwnd(void);
void rsx_null_backend_suppress_present(int);
uint32_t cellGcmResolveLocated(int, uint32_t);
uint32_t cellGcmResolveIO(uint32_t);
}
static bool s_rsx_live = false;
static const uint8_t* live_guest_ptr(void*, uint32_t location, uint32_t offset, uint32_t size)
{
    if (!vm_base) return nullptr;
    uint32_t ea = location == 0 ? cellGcmResolveLocated(1, offset) : cellGcmResolveIO(offset);
    if (!ea && location != 0) ea = cellGcmResolveLocated(0, offset);
    if (!ea || ea == UINT32_MAX || (uint64_t)ea + size > 0x100000000ull) return nullptr;
    return vm_base + ea;
}
#endif

static void present_frame(void)
{
#ifdef _WIN32
    if (s_rsx_live) { rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId()); return; }
#endif
    rsx_backend_present();
}

static int pump_messages(void)
{
#ifdef _WIN32
    if (s_rsx_live) return rsx_null_backend_pump_messages();
#endif
    return rsx_backend_pump();
}

static volatile LONG g_frames_presented = 0;
static unsigned g_fifo_drains = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    const auto profile_begin = g_pump_profile.now();
    static const bool trace = getenv("HAZE_FRAME_TRACE") != nullptr;
    if (trace) {
        static unsigned previous_flip = 0, duplicates = 0, presents = 0;
        const unsigned flip = cellGcm_flip_request_count();
        if (flip == previous_flip) ++duplicates;
        previous_flip = flip;
        if ((++presents & 127) == 0)
            fprintf(stderr, "[frame-pump] presents=%u flips=%u duplicate=%u drains=%u\n",
                    presents, flip, duplicates, g_fifo_drains);
    }
    present_frame();
    g_pump_profile.present += g_pump_profile.now()-profile_begin;
    ++g_pump_profile.presents;
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

#include "haze_frame_pump.h"

static void profile_fifo_drain(void)
{
    const auto begin=g_pump_profile.now();
    cellGcm_rsx_process_fifo();
    g_pump_profile.fifo+=g_pump_profile.now()-begin;
    ++g_pump_profile.drains;
}
static void drain_and_present(void)
{
    ++g_fifo_drains;
    haze_drain_frames(profile_fifo_drain, cellGcm_take_flip_pending,
                      present_guest_frame);
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "Haze Recomp";

    int rsx_ok = 0;
#ifdef _WIN32
    if (rsx_live_draw_enabled()) {
        haze_display_prepare();
        // Haze renders the main scene at 576p before its 720p scanout pass.
        rsx_live_draw_set_render_reference(1024, 576);
        rsx_live_draw_set_shadow_reference(2048);
        rsx_ok = rsx_null_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0;
        if (rsx_ok && !haze_display_apply(rsx_null_backend_get_hwnd())) {
            fprintf(stderr,"[display] Cannot apply display settings; stopping startup\n");
            fflush(stderr);
            ExitProcess(2);
        }
        if (rsx_ok && rsx_live_draw_init(rsx_null_backend_get_hwnd(), WINDOW_WIDTH,
                                       WINDOW_HEIGHT, live_guest_ptr, nullptr) == 0) {
            s_rsx_live = true;
            rsx_null_backend_suppress_present(1);
            haze_input_attach(rsx_null_backend_get_hwnd());
            fprintf(stderr, "[rsx] native live D3D12 renderer selected\n");
        } else {
            fprintf(stderr, "[rsx] native live renderer failed; stopping graphics\n");
            rsx_ok = 0;
        }
    } else
#endif
    { rsx_ok = rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0; }
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

#ifdef __EMSCRIPTEN__
    static ULONGLONG next_tick;
    static int browser_rsx_ok;
    next_tick = GetTickCount64();
    browser_rsx_ok = rsx_ok;
    // OffscreenCanvas presents when its worker returns to JavaScript. Modern
    // browsers removed commit(), so a blocking native loop never shows frames.
    emscripten_set_main_loop([] {
        const int rsx_ok = browser_rsx_ok;
        static bool scheduled = false;
        if (!scheduled) {
            emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 1);
            scheduled = true;
        }
#else
    ULONGLONG next_tick = GetTickCount64();
    for (;;) {
        /* Haze also waits on RSX labels, which do not signal the REF kick.
         * Keep those render-pass dependencies responsive instead of adding
         * a four-millisecond host delay to every producer/consumer round trip. */
        const auto wait_begin=g_pump_profile.now();
        if (g_pump_profile.enabled) {
            ++g_pump_profile.waits;
            g_pump_profile.queued_waits+=cellGcm_fifo_has_queued_work()!=0;
        }
        cellGcm_fifo_kick_wait(g_pump_profile.wait_ms);
        g_pump_profile.wait+=g_pump_profile.now()-wait_begin;
#endif
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            const auto tick_begin=g_pump_profile.now();
            cellGcmTickVBlank();
            cellGcmTickFlip();
            g_pump_profile.tick+=g_pump_profile.now()-tick_begin;
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) drain_and_present();
            next_tick += 16;             /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */
        g_pump_profile.report();

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            drain_and_present();

            if (pump_messages() != 0) {
                haze_input_shutdown();
#ifdef _WIN32
                if (s_rsx_live) rsx_live_draw_dump_present_samples();
#endif
                // Closing the native game window ends the host, including its
                // guest workers, rather than leaving an invisible game running.
                exit(0);
            }
            haze_input_update();
            haze_graphics_update();
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            if (cellGcm_flip_request_count() == 0
#ifdef __EMSCRIPTEN__
                && fired
#endif
            ) {
                present_frame();
            }
        }
#ifdef __EMSCRIPTEN__
    }, 1000, true);
#else
    }
#endif
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef __EMSCRIPTEN__
    // Preserve GUEST_EA's low-32-bit conversion. Advance the program break
    // past an aligned guest arena so later host allocations cannot overlap it.
    vm_base = haze_wasm_reserve_guest_vm();
    if (!vm_base) return false;
    ppu_vm_size = 0;
    fprintf(stderr, "[wasm-vm] aligned guest arena at 0x%llX, heap break 0x%llX\n",
            (unsigned long long)(uintptr_t)vm_base, (unsigned long long)(uintptr_t)sbrk(0));
#elif defined(_WIN32)
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is committed up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left. */
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static LONG WINAPI haze_crash_filter(EXCEPTION_POINTERS* ep)
{
    const uintptr_t module_base=(uintptr_t)GetModuleHandleW(nullptr);
    fprintf(stderr,"[haze-crash-context] tid=%lu module_base=%p rip_offset=0x%llX rsp=%p rcx=%p rdx=%p r8=%p r9=%p\n",
        GetCurrentThreadId(),(void*)module_base,
        (unsigned long long)(ep->ContextRecord->Rip-module_base),
        (void*)ep->ContextRecord->Rsp,(void*)ep->ContextRecord->Rcx,
        (void*)ep->ContextRecord->Rdx,(void*)ep->ContextRecord->R8,(void*)ep->ContextRecord->R9);
    if (ep->ExceptionRecord->NumberParameters>=2)
        fprintf(stderr,"[haze-crash-access] operation=%llu address=%p\n",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
            (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    fprintf(stderr, "[haze-crash] exception=0x%08lX address=%p last-hle=0x%08X (%s)\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    ppu_report_guest_lrs();
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI haze_boot_reporter(LPVOID)
{
    for (;;) {
        Sleep(5000);
        fprintf(stderr, "[haze-boot] last-hle=0x%08X (%s)\n",
                g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
        ppu_report_guest_lrs();
        if (getenv("HAZE_QUEUE_REPORT")) {
            /* BLUS30094 boot queues; read-only diagnostics, not portable symbols. */
            const uint32_t queues[] = {0x40400180u, 0x40400280u,
                                      0x014E3000u, 0x014E3980u, 0x014E6900u};
            for (uint32_t q : queues) {
                fprintf(stderr, "[haze-queue] ea=%08X head=%08X tail=%08X depth=%u"
                        " size=%u buf=%08X waiters=%08X\n", q,
                        vm_read32(q), vm_read32(q + 4), vm_read32(q + 12),
                        vm_read32(q + 8), (uint32_t)vm_read64(q + 16), vm_read32(q + 32));
            }
        }
        if (getenv("HAZE_PHYSICS_REPORT")) {
            /* BLUS30094 allocator callbacks and their original OPDs. */
            uint32_t alloc = vm_read32(0x014E150Cu);
            fprintf(stderr, "[haze-physics] alloc-opd=%08X code=%08X toc=%08X"
                    " original=%08X,%08X config=%08X,%08X\n", alloc,
                    alloc ? vm_read32(alloc) : 0,
                    alloc ? vm_read32(alloc + 4) : 0,
                    vm_read32(0x00E79E20u), vm_read32(0x00E79E24u),
                    vm_read32(0x00E8D6D4u), vm_read32(0x00E8D6D8u));
        }
        /* Windows unbuffered stdio can issue a pipe write for every character.
         * Keep bounded-run logs current without slowing down the guest. */
        fflush(stdout);
        fflush(stderr);
    }
}
#endif

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

int main(int argc, char** argv)
{
    if (!haze_graphics_initialize()) return 2;
#if defined(_WIN32) && defined(HAZE_REQUIRE_FMA)
    int cpu[4];
    __cpuid(cpu, 1);
    if (!(cpu[2] & (1 << 12)) || !IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE)) {
        fprintf(stderr, "This SPU build requires AVX/FMA3. Rebuild without -SpuFma for this CPU.\n");
        return 2;
    }
    fprintf(stderr, "[boot] Lifted SPU SIMD: SSE4.1 + AVX/FMA3\n");
#endif
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    /* Haze's full frame pipeline renders its splash screens through this
     * native D3D12 path. RSX_LIVE_DRAW=0 retains the legacy renderer for probes. */
    if (!getenv("RSX_LIVE_DRAW")) _putenv_s("RSX_LIVE_DRAW", "1");
    /* Match the requested 30 FPS ceiling. Zero is an explicit profiling
     * override; the SDK itself remains unrestricted for other titles. */
    if (!getenv("RSX_MAX_FPS")) _putenv_s("RSX_MAX_FPS", "30");
    /* High-volume SDK logging contends on FILE locks across guest threads. */
    if (!getenv("PS3_VERBOSE")) _putenv_s("PS3_VERBOSE", "0");
    static char stdout_buffer[64 * 1024], stderr_buffer[64 * 1024];
    setvbuf(stdout, stdout_buffer, _IOFBF, sizeof stdout_buffer);
    setvbuf(stderr, stderr_buffer, _IOFBF, sizeof stderr_buffer);
    SetUnhandledExceptionFilter(haze_crash_filter);
#endif

    printf("=== ps3recomp game runner ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);
    const char* save_root = getenv("PS3_SAVEDATA_ROOT");
    char default_save_root[1200];
    if (!save_root || !*save_root) {
        snprintf(default_save_root, sizeof(default_save_root), "%s/savedata", ppu_vfs_root);
        save_root = default_save_root;
    }
    if (cellSaveData_set_root_path(save_root) != 0) {
        fprintf(stderr, "ERROR: invalid save-data root\n");
        return 1;
    }
    cellSaveData_set_strict_load_errors(1);
    printf("[boot] Save-data root: %s\n", save_root);
    char sfo_path[1200];
    snprintf(sfo_path, sizeof sfo_path, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
    cellGame_init_from_paramsfo(sfo_path);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */
#ifdef _WIN32
    if (getenv("PS3_SAMPLE")) ps3_sampler_start();
#endif

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    haze_input_initialize();
    haze_camera_initialize();
#if defined(__EMSCRIPTEN__) && defined(HAZE_WASM_WEBGL)
    pthread_attr_t frame_attr;
    pthread_attr_init(&frame_attr);
    pthread_attr_setstacksize(&frame_attr, 4u * 1024 * 1024);
    emscripten_pthread_attr_settransferredcanvases(&frame_attr, "#canvas");
    pthread_t frame_thread;
    const int frame_result = pthread_create(&frame_thread, &frame_attr,
        [](void*) -> void* { frame_clock(nullptr); return nullptr; }, nullptr);
    pthread_attr_destroy(&frame_attr);
    if (frame_result) { fprintf(stderr, "[wasm] render thread failed: %d\n", frame_result); return 1; }
#else
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
#endif
#ifdef _WIN32
    if (getenv("HAZE_BOOT_REPORT")) {
        HANDLE reporter = CreateThread(NULL, 0, haze_boot_reporter, NULL, 0, NULL);
        if (reporter) CloseHandle(reporter);
    }
#endif

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
