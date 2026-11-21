#include "ppu_recomp.h"
#include "haze_benchmark_clock.h"
#include "haze_benchmark_clock_logic.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Diagnostic frame-locked benchmark ("timedemo"); see docs/benchmark-clock.md.
// Normal launches set none of these variables and keep the real clocks.
//
// The game clock is sys_time_get_system_time: func_0071F028 converts it to
// seconds and the frame tick (func_0068ECD8) derives each frame's dt from it,
// spinning only while dt < 0. Real time makes the simulation depend on host
// speed: a faster build reaches scripted events in fewer frames.
//
// HAZE_BENCHMARK_CLOCK_US=<step>: virtual time advances exactly <step> us at
//   each guest frame boundary and is frozen within a frame. If no boundary
//   arrives for the escape interval of real time (loading, a wait that polls
//   the clock), time resumes at the real rate beyond it, so nothing deadlocks;
//   escaped time is recorded because it breaks frame-locking for that frame.
// HAZE_BENCHMARK_ESCAPE_US / HAZE_BENCHMARK_GAMEPLAY_ESCAPE_US: the escape
//   interval before and after gameplay frame 0 (default 100000 both).
// HAZE_BENCHMARK_TIMELINE=<settle>,<sweep>,<csv>: count gameplay frames from
//   the first local-player look update, run the camera sweep for <sweep>
//   frames after <settle>, then write the per-frame CSV and <csv>.done.
// HAZE_BENCHMARK_INPUT="<boundary>:<PAD_FILE command>;...": write PAD_FILE
//   commands at guest frame boundaries instead of wall-clock times.
// HAZE_BENCHMARK_SYNC_GPU=1: during gameplay, wait at each boundary until the
//   guest FIFO is drained, so fences/labels/reports never depend on renderer
//   progress. This serializes producer and renderer: alignment, not speed.
// HAZE_BENCHMARK_MEMHASH=<path>: hash every committed guest page at gameplay
//   boundaries 0..8, to locate the first diverging state between two runs.
// HAZE_BENCHMARK_KEEP_HISTORY=1: skip the gameplay-start frame-tick reset.
#ifdef _WIN32
extern "C" void ps3_hle_register_ctx(uint32_t,const char*,void(*)(ppu_context*));
extern "C" uint32_t cellGcm_flip_request_count(void);
extern "C" int cellGcm_fifo_has_queued_work(void);
extern "C" void haze_fingerprint_trace_from(uint32_t flips);
extern "C" uint8_t* vm_base;

namespace {
constexpr uint32_t system_time_nid=0x8461E528u;

struct Row {
    uint64_t boundary, virtual_us, escaped_us;
    int64_t gameplay;
    LONGLONG qpc;
    uint32_t flips;
};

uint64_t environment_u64(const char* name,uint64_t fallback) {
    const char* value=std::getenv(name);
    return value && *value ? std::strtoull(value,nullptr,10) : fallback;
}

struct Clock {
    uint64_t step=0;
    long long settle=-1, sweep=0;
    char csv[1024]{};
    SRWLOCK lock=SRWLOCK_INIT;
    LONGLONG frequency=0, boundary_qpc=0;
    uint64_t base=0, last=0, escaped=0, boundaries=0, escaped_frames=0;
    std::atomic<uint64_t> escape_us{100000};
    uint64_t gameplay_escape_us=100000;
    std::atomic<bool> gameplay_seen{false};
    std::atomic<uint64_t> published{0};
    std::atomic<long long> gameplay{-1};
    static constexpr size_t capacity=size_t(1)<<18;
    std::unique_ptr<Row[]> rows;
    std::atomic<bool> dumped{false};
    std::vector<haze_benchmark::Command> commands;
    size_t next_command=0;
    const char* pad_file=nullptr;
    const char* memhash=nullptr;
    bool sync_gpu=false;

    Clock() {
        const char* value=std::getenv("HAZE_BENCHMARK_CLOCK_US");
        if (!value || !*value) return;
        char* end=nullptr;
        const unsigned long long parsed=std::strtoull(value,&end,10);
        if (*end || parsed<1000 || parsed>100000) {
            std::fprintf(stderr,"[benchmark-clock] ignoring invalid HAZE_BENCHMARK_CLOCK_US=%s\n",value);
            return;
        }
        LARGE_INTEGER clock;
        if (!QueryPerformanceFrequency(&clock) || clock.QuadPart<=0) return;
        frequency=clock.QuadPart;
        QueryPerformanceCounter(&clock); boundary_qpc=clock.QuadPart;
        step=parsed;
        if (const char* timeline=std::getenv("HAZE_BENCHMARK_TIMELINE")) {
            long long a=0,b=0; int used=0;
            if (std::sscanf(timeline,"%lld,%lld,%n",&a,&b,&used)==2 && used>0 &&
                a>=0 && b>0 && timeline[used] && std::strlen(timeline+used)<sizeof csv) {
                settle=a; sweep=b; std::strcpy(csv,timeline+used);
                rows.reset(new Row[capacity]);
            } else {
                std::fprintf(stderr,"[benchmark-clock] ignoring invalid HAZE_BENCHMARK_TIMELINE\n");
            }
        }
        escape_us=environment_u64("HAZE_BENCHMARK_ESCAPE_US",100000);
        gameplay_escape_us=environment_u64("HAZE_BENCHMARK_GAMEPLAY_ESCAPE_US",escape_us);
        pad_file=std::getenv("PAD_FILE");
        if (const char* input=std::getenv("HAZE_BENCHMARK_INPUT"); input && pad_file &&
            !haze_benchmark::parse_input(input,commands))
            std::fprintf(stderr,"[benchmark-clock] HAZE_BENCHMARK_INPUT stops after %zu commands\n",commands.size());
        memhash=std::getenv("HAZE_BENCHMARK_MEMHASH");
        sync_gpu=std::getenv("HAZE_BENCHMARK_SYNC_GPU")!=nullptr;
        std::fprintf(stderr,"[benchmark-clock] step_us=%llu escape_us=%llu gameplay_escape_us=%llu "
            "settle=%lld sweep=%lld input_commands=%zu sync_gpu=%d\n",(unsigned long long)step,
            (unsigned long long)escape_us.load(),(unsigned long long)gameplay_escape_us,
            settle,sweep,commands.size(),sync_gpu ? 1 : 0);
    }
    bool active() const { return step!=0; }
    uint64_t real_since_boundary(LONGLONG now) const {
        const auto delta=uint64_t(now>boundary_qpc ? now-boundary_qpc : 0);
        return delta/uint64_t(frequency)*1000000u+delta%uint64_t(frequency)*1000000u/uint64_t(frequency);
    }
    uint64_t read() {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        AcquireSRWLockExclusive(&lock);
        const uint64_t real=real_since_boundary(now.QuadPart);
        const uint64_t limit=escape_us.load(std::memory_order_relaxed);
        const uint64_t value=haze_benchmark::clock_value(base,last,real,limit);
        if (real>limit && real-limit>escaped) escaped=real-limit;
        last=value;
        ReleaseSRWLockExclusive(&lock);
        return value;
    }
    // Menus and loading take a variable number of frames, and the frame tick
    // (func_0068ECD8) carries that history as accumulated game time (double at
    // 0x1515318) and a frame counter (0x12B9C18). Restart both at gameplay
    // frame 0 so time- and frame-scheduled game logic agrees across runs.
    void canonicalize_frame_tick() const {
        if (!rows || std::getenv("HAZE_BENCHMARK_KEEP_HISTORY")) return;
        const uint32_t globals=vm_read32(0xE94EE0-0x7D44);
        if (globals!=0xE35F10 || vm_read32(globals-0x7FF0)!=0x1515318 ||
            vm_read32(globals-0x7FEC)!=0x12B9C18) {
            std::fprintf(stderr,"[benchmark-clock] unsupported frame tick globals; history kept\n");
            return;
        }
        uint64_t old_bits=vm_read64(0x1515318); double old_time;
        std::memcpy(&old_time,&old_bits,sizeof old_time);
        std::fprintf(stderr,"[benchmark-clock] canonical frame tick: game_time %.6f -> 100, frames %u -> 100000\n",
            old_time,vm_read32(0x12B9C18));
        const double game_time=100.0; uint64_t bits; std::memcpy(&bits,&game_time,sizeof bits);
        vm_write64(0x1515318,bits);
        vm_write32(0x12B9C18,100000);
    }
    // Other threads may be writing, so a page that differs in only one frame
    // between runs is noise; persistent differences locate diverging state.
    void hash_memory(long long frame) const {
        if (!memhash || !*memhash || frame<0 || frame>8 || !vm_base) return;
        FILE* file=std::fopen(memhash,frame==0 ? "wb" : "ab");
        if (!file) return;
        std::vector<uint64_t> records;
        for (uint64_t offset=0;offset<0x100000000ull;) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(vm_base+offset,&info,sizeof info)) break;
            const uint64_t region_end=uint64_t(static_cast<uint8_t*>(info.BaseAddress)-vm_base)+info.RegionSize;
            if (info.State==MEM_COMMIT && !(info.Protect&(PAGE_NOACCESS|PAGE_GUARD))) {
                for (uint64_t page=offset&~0xFFFull;page<region_end && page<0x100000000ull;page+=0x1000) {
                    const uint64_t* words=reinterpret_cast<const uint64_t*>(vm_base+page);
                    uint64_t hash=0x9E3779B97F4A7C15ull;
                    for (int i=0;i<512;++i) hash=(hash^words[i])*0xFF51AFD7ED558CCDull;
                    records.push_back(page); records.push_back(hash);
                }
            }
            offset=region_end;
        }
        const uint32_t header[2]={uint32_t(frame),uint32_t(records.size()/2)};
        std::fwrite(header,sizeof header,1,file);
        std::fwrite(records.data(),sizeof(uint64_t),records.size(),file);
        std::fclose(file);
    }
    void frame() {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        long long gameplay_frame=gameplay.load(std::memory_order_relaxed);
        if (gameplay_frame>=0) ++gameplay_frame;
        else if (gameplay_seen.load(std::memory_order_relaxed)) gameplay_frame=0;
        AcquireSRWLockExclusive(&lock);
        const uint64_t next=haze_benchmark::next_base(base,last,step);
        const uint64_t frame_escaped=escaped;
        if (frame_escaped) ++escaped_frames;
        base=next; boundary_qpc=now.QuadPart; escaped=0;
        published.store(next,std::memory_order_release);
        const uint64_t index=boundaries++;
        ReleaseSRWLockExclusive(&lock);
        if (frame_escaped)
            std::fprintf(stderr,"[benchmark-clock-escape] boundary=%llu escaped_us=%llu\n",
                (unsigned long long)index,(unsigned long long)frame_escaped);
        for (;next_command<commands.size() && commands[next_command].boundary<=index;++next_command) {
            if (FILE* file=std::fopen(pad_file,"wb")) {
                std::fprintf(file,"%s\n",commands[next_command].text); std::fclose(file);
            }
            std::fprintf(stderr,"[benchmark-input] boundary=%llu command=%s\n",
                (unsigned long long)index,commands[next_command].text);
        }
        gameplay.store(gameplay_frame,std::memory_order_relaxed);
        hash_memory(gameplay_frame);
        if (sync_gpu && gameplay_frame>=0) {
            const ULONGLONG deadline=GetTickCount64()+1000;
            while (cellGcm_fifo_has_queued_work() && GetTickCount64()<deadline) SwitchToThread();
        }
        if (gameplay_frame==0) {
            // The frame submitted after this boundary carries the next flip count.
            haze_fingerprint_trace_from(cellGcm_flip_request_count()+1);
            canonicalize_frame_tick();
            escape_us.store(gameplay_escape_us,std::memory_order_relaxed);
        }
        if (rows && !dumped.load(std::memory_order_relaxed))
            rows[index%capacity]={index,next,frame_escaped,gameplay_frame,now.QuadPart,
                cellGcm_flip_request_count()};
        if (rows && gameplay_frame>=0 && gameplay_frame==settle+sweep) dump();
    }
    bool sweep_state(unsigned& token,double& seconds) const {
        if (!rows) return false;
        const auto state=haze_benchmark::sweep_at(gameplay.load(std::memory_order_relaxed),settle,sweep,step);
        token=state.token; seconds=state.seconds;
        return true;
    }
    void dump() {
        // The timeline dumps itself on the game thread; shutdown is a fallback.
        if (!rows || dumped.exchange(true)) return;
        FILE* file=std::fopen(csv,"wb");
        if (!file) { std::fprintf(stderr,"[benchmark-clock-dump-error] cannot open timeline CSV\n"); return; }
        bool error=std::fprintf(file,"# qpc_frequency=%lld step_us=%llu settle=%lld sweep=%lld\n"
            "boundary,gameplay,virtual_us,qpc,escaped_us,flips\n",frequency,
            (unsigned long long)step,settle,sweep)<0;
        const uint64_t total=boundaries;
        const uint64_t first=total>capacity ? total-capacity : 0;
        for (uint64_t i=first;i<total;++i) {
            const Row& r=rows[i%capacity];
            if (std::fprintf(file,"%llu,%lld,%llu,%lld,%llu,%u\n",(unsigned long long)r.boundary,
                (long long)r.gameplay,(unsigned long long)r.virtual_us,r.qpc,
                (unsigned long long)r.escaped_us,r.flips)<0) error=true;
        }
        if (std::fclose(file)!=0) error=true;
        std::fprintf(stderr,"[benchmark-clock-dump%s] boundaries=%llu escaped_frames=%llu\n",
            error ? "-error" : "",(unsigned long long)total,(unsigned long long)escaped_frames);
        if (!error) {
            char done[1100];
            std::snprintf(done,sizeof done,"%s.done",csv);
            if (FILE* marker=std::fopen(done,"wb")) std::fclose(marker);
        }
    }
};
Clock& benchmark_clock() { static Clock value; return value; }
void system_time(ppu_context* ctx) { ctx->gpr[3]=benchmark_clock().read(); }
}

// mftb/mftbu. Engine code times its own work with the timebase (budgets,
// profiling); real time would make the amount of work per frame depend on host
// speed. Under the benchmark clock each thread sees the frame's virtual time
// plus 1 us per read, so a budgeted loop does the same iterations on any host
// and a spin on the timebase still terminates.
extern "C" uint64_t ppu_timebase_real(void);
extern "C" uint64_t ppu_timebase_now(void) {
    static const bool active=benchmark_clock().active();
    if (!active) return ppu_timebase_real();
    thread_local haze_benchmark::ThreadTimebase timebase;
    return timebase.read(benchmark_clock().published.load(std::memory_order_acquire));
}

bool haze_benchmark_clock_active() { return benchmark_clock().active(); }
void haze_benchmark_clock_register() {
    // Must precede ppu_sysprx_register: the first registered handler wins.
    if (benchmark_clock().active()) ps3_hle_register_ctx(system_time_nid,"Haze benchmark clock",system_time);
}
void haze_benchmark_clock_frame(ppu_context*) { if (benchmark_clock().active()) benchmark_clock().frame(); }
void haze_benchmark_clock_note_gameplay() {
    if (benchmark_clock().active()) benchmark_clock().gameplay_seen.store(true,std::memory_order_relaxed);
}
bool haze_benchmark_clock_sweep(unsigned& token,double& seconds) {
    return benchmark_clock().active() && benchmark_clock().sweep_state(token,seconds);
}
void haze_benchmark_clock_dump() { if (benchmark_clock().active()) benchmark_clock().dump(); }
#else
bool haze_benchmark_clock_active() { return false; }
void haze_benchmark_clock_register() {}
void haze_benchmark_clock_frame(ppu_context*) {}
void haze_benchmark_clock_note_gameplay() {}
bool haze_benchmark_clock_sweep(unsigned&,double&) { return false; }
void haze_benchmark_clock_dump() {}
#endif
