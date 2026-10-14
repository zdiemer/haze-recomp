// Feasibility probe only: this does not change Haze's allocator or cache.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "rsx_texture_hash.h"
#include "rsx_texture_write_watch.h"

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "write-watch probe: %s (error %lu)\n", message, GetLastError()); std::exit(1); }
}
static double now_ms() {
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    return double(q.QuadPart) * 1000 / double(f.QuadPart);
}

// One reader owns epochs and queries. Producers write ordinary bytes. Compare
// whole-arena snapshots against per-texture snapshots; both retain dirty-page
// generations so reset queries cannot hide writes from overlapping textures.
struct Watch {
    unsigned char* base;
    size_t size, page;
    uint64_t epoch = 0;
    bool healthy = true;
    bool regional;
    std::vector<void*> dirty;
    std::vector<uint64_t> generations;
    rsx_texture_write_watch native{};
    Watch(unsigned char* b, size_t n, size_t p, bool r = false) : base(b), size(n), page(p), regional(r), dirty(n/p), generations(n/p) {
        require(rsx_texture_write_watch_init(&native,b,n)!=0,"native tracker initialization");
    }
    ~Watch() { rsx_texture_write_watch_destroy(&native); }
    bool begin() {
        if (regional) return healthy;
        ULONG_PTR count = dirty.size(); DWORD granularity = 0;
        if (!healthy || GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, size, dirty.data(), &count, &granularity)
                || granularity != page || count > dirty.size() || ++epoch == 0) {
            healthy = false; return false;
        }
        for (size_t i = 0; i < count; ++i) {
            const uintptr_t address = reinterpret_cast<uintptr_t>(dirty[i]);
            const uintptr_t origin = reinterpret_cast<uintptr_t>(base);
            if (address < origin || address-origin >= size || (address-origin)%page) {
                healthy = false; return false;
            }
            generations[(address-origin)/page] = epoch;
        }
        return true;
    }
    bool content_stamp(size_t offset, size_t length, uint64_t& stamp) {
        if (!healthy || !length || offset >= size || length > size-offset) return false;
        if (regional) {
            const bool ok = rsx_texture_write_watch_stamp(&native,base+offset,length,&stamp,nullptr)!=0;
            healthy = native.healthy!=0; epoch = native.epoch;
            return ok;
        }
        const size_t first = offset/page, last = (offset+length-1)/page;
        ULONG_PTR count = last-first+1; DWORD granularity = 0;
        if (GetWriteWatch(regional ? WRITE_WATCH_FLAG_RESET : 0, base+first*page, count*page, dirty.data(), &count, &granularity)
                || granularity != page || count > last-first+1) { healthy = false; return false; }
        if (count && !regional) return false;
        if (count) {
            if (++epoch == 0) { healthy = false; return false; }
            const uintptr_t origin = reinterpret_cast<uintptr_t>(base);
            for (size_t i=0; i<count; ++i) {
                const uintptr_t address = reinterpret_cast<uintptr_t>(dirty[i]);
                if (address < origin+first*page || address > origin+last*page || (address-origin)%page) {
                    healthy = false; return false;
                }
                generations[(address-origin)/page] = epoch;
            }
        }
        stamp = 0;
        for (size_t i = first; i <= last; ++i) stamp = std::max(stamp, generations[i]);
        return true;
    }
};

struct Cached {
    bool valid = false;
    uint64_t stamp = 0, hash = 0;
};
static bool bind(Watch& w, size_t offset, size_t length, Cached& c) {
    uint64_t stamp = 0;
    const bool clean = w.content_stamp(offset, length, stamp);
    const bool skip = clean && c.valid && stamp == c.stamp;
    const uint64_t actual = rsx_texture_hash(w.base+offset, uint32_t(length));
    require(!skip || actual == c.hash, "dirty contents incorrectly classified unchanged");
    if (!skip) c.hash = actual;
    c.valid = clean; c.stamp = stamp;
    return skip;
}

struct Producer {
    unsigned char* destination;
    HANDLE request, done;
};
static DWORD WINAPI produce(void* argument) {
    auto& p = *static_cast<Producer*>(argument);
    for (unsigned i=0; i<128; ++i) {
        require(WaitForSingleObject(p.request,5000)==WAIT_OBJECT_0,"producer request timeout");
        *p.destination ^= 1;
        require(SetEvent(p.done)!=0,"producer completion event");
    }
    return 0;
}

static void correctness(Watch& w) {
    const size_t p = w.page;
    Cached a, b;
    require(w.begin(), "initial dirty snapshot");
    bind(w, p-1, p+2, a); bind(w, 2*p, p, b);
    require(bind(w, p-1, p+2, a), "clean repeat did not skip");
    w.base[2*p] ^= 1; // Both textures share this page, including a one-byte tail.
    require(!bind(w, p-1, p+2, a), "late write missed by first overlapping texture");
    require(!bind(w, 2*p, p, b), "first query reset a page shared with another texture");
    require(w.begin(), "snapshot following overlapping write");
    require(bind(w, 2*p, p, b) == w.regional, "baseline after snapshot has wrong lifetime");
    require(bind(w, 2*p, p, b), "clean baseline after revalidation");
    w.base[2*p+1] ^= 1;
    require(w.begin(), "write between last bind and next frame");
    require(!bind(w, 2*p, p, b), "reset discarded an unbound write");
    require(bind(w, 2*p, p, b), "revalidated epoch");
    uint64_t stamp;
    require(!w.content_stamp(w.size, 1, stamp) && !w.content_stamp(w.size-1, 2, stamp)
            && !w.content_stamp(0, 0, stamp), "invalid ranges must fail closed");
    // Demand commitment near the end of the 4-GB reservation, not just its prefix.
    require(VirtualAlloc(w.base+w.size-65536, 65536, MEM_COMMIT, PAGE_READWRITE) != nullptr, "late demand commit");
    w.base[w.size-1] = 91;
    const uint64_t before_tail = w.epoch;
    require(w.content_stamp(w.size-1, 1, stamp) == w.regional, "newly committed tail state incorrect");
    if (w.regional) require(stamp > before_tail, "newly committed tail write not tracked");
    require(w.begin(), "snapshot including newly committed tail");
    require(w.content_stamp(w.size-1, 1, stamp) && stamp == w.epoch, "tail epoch not retained");
    uint32_t random = 23;
    Cached entries[8];
    for (unsigned i = 0; i < 4000; ++i) {
        random = random*1664525u+1013904223u;
        if ((random&7) == 0) require(w.begin(), "random snapshot");
        else if ((random&3) == 1) w.base[(random>>8)%(12*p)] ^= uint8_t(random>>24);
        else {
            const unsigned index = (random>>16)&7;
            bind(w, index*p+p/2, 3*p+3, entries[index]);
        }
    }
    w.healthy = false;
    require(!w.content_stamp(0, 1, stamp), "failed tracking must not claim clean");
    w.healthy = true;
    Producer producer{w.base+2*p,CreateEventW(nullptr,FALSE,FALSE,nullptr),CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    require(producer.request && producer.done,"producer event allocation");
    HANDLE thread=CreateThread(nullptr,0,produce,&producer,0,nullptr);
    require(thread!=nullptr,"producer thread allocation");
    a={}; b={};
    for(unsigned i=0;i<128;++i) {
        require(w.begin(),"producer snapshot");
        bind(w,p-1,p+2,a); bind(w,2*p,p,b);
        require(SetEvent(producer.request)!=0,"request cross-thread write");
        require(WaitForSingleObject(producer.done,5000)==WAIT_OBJECT_0,"producer completion timeout");
        require(!bind(w,p-1,p+2,a) && !bind(w,2*p,p,b),"cross-thread shared-page write missed");
    }
    require(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0,"producer exit timeout");
    CloseHandle(thread); CloseHandle(producer.request); CloseHandle(producer.done);
    std::fprintf(stderr, "write-watch mutation oracle passed (%s: 4000 random operations, 128 cross-thread writes, overlap, late writes, demand commit, bounds)\n",w.regional?"regional":"whole arena");
}

static volatile uint64_t sink;
static void benchmark(unsigned char* base, Watch* watch, unsigned percent, unsigned background_mb, unsigned round) {
    constexpr size_t texture_size = 1024*1024, textures = 96, frames = 8;
    SYSTEM_INFO info; GetSystemInfo(&info);
    const size_t page = info.dwPageSize, source_pages = texture_size*textures/page;
    std::vector<Cached> cache(textures);
    if (watch) require(watch->begin(), "benchmark initial snapshot");
    for (size_t i = 0; i < textures; ++i) {
        cache[i].hash = rsx_texture_hash(base+i*texture_size, uint32_t(texture_size));
        cache[i].valid = watch && watch->content_stamp(i*texture_size, texture_size, cache[i].stamp);
    }
    double reset_ms = 0, writes_ms = 0, checks_ms = 0;
    size_t hashes = 0, skipped = 0;
    ULONG64 cycles_begin, cycles_end;
    require(QueryThreadCycleTime(GetCurrentThread(),&cycles_begin)!=0,"begin thread CPU clock");
    for (size_t frame = 0; frame < frames; ++frame) {
        double start = now_ms();
        if (watch) require(watch->begin(), "benchmark frame snapshot");
        reset_ms += now_ms()-start;
        start = now_ms();
        for (size_t i = 0; i < source_pages*percent/100; ++i) base[((i*67)%source_pages)*page] ^= 1;
        for (size_t i = 0; i < size_t(background_mb)*1024*1024/page; ++i) base[192*1024*1024+i*page] ^= 1;
        writes_ms += now_ms()-start;
        start = now_ms();
        for (size_t i = 0; i < textures; ++i) {
            Cached& c = cache[i]; uint64_t stamp = 0;
            const bool clean = watch && watch->content_stamp(i*texture_size, texture_size, stamp);
            if (clean && c.valid && stamp == c.stamp) ++skipped;
            else { c.hash = rsx_texture_hash(base+i*texture_size, uint32_t(texture_size)); ++hashes; }
            c.valid = clean; c.stamp = stamp; sink = c.hash;
        }
        checks_ms += now_ms()-start;
    }
    require(QueryThreadCycleTime(GetCurrentThread(),&cycles_end)!=0,"end thread CPU clock");
    std::printf("%u,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%zu,%zu,%.3f\n",round,watch?(watch->regional?2:1):0,percent,background_mb,
        reset_ms/frames,writes_ms/frames,checks_ms/frames,(reset_ms+writes_ms+checks_ms)/frames,hashes,skipped,double(cycles_end-cycles_begin)/frames);
}

int main(int argc, char** argv) {
    const bool test_only=argc==2 && std::strcmp(argv[1],"--test-only")==0;
    require(argc==1 || test_only,"usage: benchmark_write_watch [--test-only]");
    SYSTEM_INFO info; GetSystemInfo(&info);
    const size_t size = size_t(1)<<32;
    auto* tracked = static_cast<unsigned char*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_WRITE_WATCH,PAGE_READWRITE));
    auto* plain = static_cast<unsigned char*>(VirtualAlloc(nullptr,size,MEM_RESERVE,PAGE_READWRITE));
    require(tracked && plain, "4-GB reservations");
    for (auto* base : {tracked,plain}) {
        require(VirtualAlloc(base,256*1024*1024,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"prefix commitment");
        for (size_t i=0;i<96*1024*1024;++i) base[i]=uint8_t(i*13+7);
    }
    Watch watch(tracked,size,info.dwPageSize);
    correctness(watch);
    rsx_texture_write_watch invalid{};
    uint64_t stamp=0;
    require(rsx_texture_write_watch_init(&invalid,plain,size)!=0,"untracked query fixture");
    require(!rsx_texture_write_watch_stamp(&invalid,plain,1,&stamp,nullptr) && !invalid.healthy,"native query failure must disable tracking");
    rsx_texture_write_watch_destroy(&invalid);
    require(rsx_texture_write_watch_init(&invalid,tracked,size)!=0,"overflow fixture");
    invalid.epoch=UINT64_MAX;
    require(!rsx_texture_write_watch_stamp(&invalid,tracked,1,&stamp,nullptr) && !invalid.healthy,"generation overflow must disable tracking");
    rsx_texture_write_watch_destroy(&invalid);
    watch.regional = true;
    correctness(watch);
    if (test_only) {
        VirtualFree(tracked,0,MEM_RELEASE); VirtualFree(plain,0,MEM_RELEASE);
        return 0;
    }
    std::printf("round,tracked,texture_dirty_percent,background_write_mb,reset_ms,writes_ms,checks_ms,total_ms,hashes,skipped,thread_cycles_per_frame\n");
    for (unsigned round=0;round<4;++round)
        for (unsigned background : {0u,16u})
            for (unsigned percent : {0u,1u,10u,100u}) {
                watch.regional = false;
                if (round&1) benchmark(tracked,&watch,percent,background,round);
                benchmark(plain,nullptr,percent,background,round);
                if (!(round&1)) benchmark(tracked,&watch,percent,background,round);
                watch.regional = true;
                benchmark(tracked,&watch,percent,background,round);
            }
    VirtualFree(tracked,0,MEM_RELEASE); VirtualFree(plain,0,MEM_RELEASE);
}
