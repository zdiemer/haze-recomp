#include "ppu_recomp.h"
#include "ppu_native_memory.h"
#include "haze_vp_packets.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
extern "C" int cellGcm_native_context_matches(uint32_t);
extern "C" uint8_t* vm_base;

namespace {
uint32_t raw_read(uint32_t a) {
    const uint8_t* p=vm_base+a;
    return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
void raw_write(uint32_t a,uint32_t v) {
    for(unsigned i=0;i<4;++i) vm_base[a+i]=uint8_t(v>>(24-i*8));
}
struct MemoryScope { ~MemoryScope() { ppu_native_memory_end(); } };
#ifdef _WIN32
struct Mode {
    const char* control=std::getenv("HAZE_VP_PACKET_FILE");
    bool profile=std::getenv("HAZE_VP_PACKET_PROFILE")!=nullptr;
    int enabled=[] { const char* e=std::getenv("HAZE_NATIVE_VP_PACKETS");
        return e && !std::strcmp(e,"1") ? 1:0; }();
    std::atomic<int> reported{-1};
    ULONGLONG next_poll=0;
    std::atomic<uint64_t> original{0},native{0},rejected{0},blocks{0},ticks{0};
    LONGLONG start=0;
    void poll() {
        if (!control && !enabled && !profile) return;
        const auto now=GetTickCount64();
        if (control && now>=next_poll) {
            next_poll=now+100;
            if (FILE* f=std::fopen(control,"r")) {
                unsigned v; if(std::fscanf(f,"%u",&v)==1 && v<=1) enabled=int(v);
                std::fclose(f);
            }
        }
        if(reported!=enabled) {
            LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
            std::fprintf(stderr,"[haze-vp-packet-counts] qpc=%lld original=%llu native=%llu rejected=%llu blocks=%llu ticks=%llu\n",
                qpc.QuadPart,original.load(),native.load(),rejected.load(),blocks.load(),ticks.load());
            std::fprintf(stderr,"[haze-vp-packet-mode] enabled=%d qpc=%lld\n",enabled,qpc.QuadPart);
            original=native=rejected=blocks=ticks=0; reported=enabled;
        }
    }
};
Mode& mode() { static Mode value; return value; }
#endif
}

// The function hash guards the only installed call site, before 0072C4F8.
// This implementation preserves even the loop's scratch registers so the
// unchanged continuation has the same complete architectural boundary state.
bool haze_vp_upload_native(ppu_context* ctx) {
    if(ctx->thread_id!=1 || ctx->gpr[2]!=0xE94EE0 || !ctx->ctr || ctx->ctr>64 ||
       ctx->ctr!=ctx->gpr[31] || ctx->gpr[3]!=0x00800B80 ||
       ctx->gpr[4]!=ctx->gpr[23] || ctx->gpr[4]>UINT32_MAX || ctx->gpr[29]>UINT32_MAX ||
       !ppu_native_memory_available()) return false;
    const uint32_t meta=uint32_t(ctx->gpr[29]), src=uint32_t(ctx->gpr[4]);
    const uint32_t n=ctx->ctr, in=n*128, out=n*132;
    if((meta&3) || (src&3) || !ppu_native_memory_range(meta,16)) return false;
    // Read the real, already-reserved context; leave allocation callbacks and
    // later FIFO publication in the original code.
    const uint32_t begin=raw_read(meta), end=raw_read(meta+4), dst=raw_read(meta+8);
    if((dst&3) || begin>dst || uint64_t(dst)+out>end ||
       !cellGcm_native_context_matches(meta) ||
       !ppu_native_memory_begin(src,in,dst,out,meta,16)) return false;
    MemoryScope scope;
    if(raw_read(meta)!=begin || raw_read(meta+4)!=end || raw_read(meta+8)!=dst) return false;
    // All three disjoint spans are plain RAM and unreserved while the scope
    // holds the reservation lock. No services, allocation or logging here.
    const uint32_t last_src=src+in-128, last_dst=dst+out-132;
    const uint32_t r0=raw_read(last_src+124), r5=raw_read(last_src+60);
    const uint32_t r6=raw_read(last_src+44), r7=raw_read(last_src+120), r8=raw_read(last_src+116);
    if(!haze::pack_vp_blocks(vm_base+src,in,vm_base+dst,out,n)) return false;
    std::atomic_thread_fence(std::memory_order_release);
    raw_write(meta+8,dst+out);
    ctx->gpr[0]=r0; ctx->gpr[4]+=in; ctx->gpr[5]=r5; ctx->gpr[6]=r6;
    ctx->gpr[7]=r7; ctx->gpr[8]=r8; ctx->gpr[9]=dst+out;
    ctx->gpr[10]=last_src+64; ctx->gpr[11]=last_dst+68; ctx->ctr=0;
    return true;
}

bool haze_vp_upload_begin(ppu_context* ctx) {
#ifdef _WIN32
    // Only the main-thread producer participates in the comparison/counters.
    if(ctx->thread_id!=1) return false;
    auto& m=mode(); m.poll();
    if(m.profile) { LARGE_INTEGER t; QueryPerformanceCounter(&t); m.start=t.QuadPart; }
    if(!m.enabled) { if(m.control || m.profile) ++m.original; return false; }
    const uint32_t count=ctx->ctr;
    if(haze_vp_upload_native(ctx)) { ++m.native; m.blocks+=count; return true; }
    ++m.rejected;
#endif
    return false;
}
void haze_vp_upload_end(ppu_context* ctx) {
#ifdef _WIN32
    if(ctx->thread_id!=1) return;
    auto& m=mode();
    if(m.profile && m.start) {
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        m.ticks+=uint64_t(t.QuadPart-m.start); m.start=0;
    }
#endif
}
extern "C" void haze_vp_upload_summary() {
#ifdef _WIN32
    auto& m=mode();
    if(m.reported<0) return;
    LARGE_INTEGER t,f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    std::fprintf(stderr,"[haze-vp-packet-counts] qpc=%lld original=%llu native=%llu rejected=%llu blocks=%llu ticks=%llu frequency=%lld\n",
        t.QuadPart,m.original.load(),m.native.load(),m.rejected.load(),m.blocks.load(),m.ticks.load(),f.QuadPart);
#endif
}
