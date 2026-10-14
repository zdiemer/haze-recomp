// Compare native packet bytes with the exact privately extracted game loop.
// This validates the packing kernel, not guest ownership/publication or a hook.
#include "haze_vp_packets.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

struct ppu_context { uint64_t gpr[32]{}; uint32_t ctr = 0; };
static std::vector<uint8_t> memory;
static uint32_t vm_read32(uint64_t address) {
    const auto a = static_cast<uint32_t>(address);
    assert(uint64_t(a) + 4 <= memory.size());
    return uint32_t(memory[a]) << 24 | uint32_t(memory[a+1]) << 16 |
           uint32_t(memory[a+2]) << 8 | memory[a+3];
}
static void vm_write32(uint64_t address, uint32_t value) {
    const auto a = static_cast<uint32_t>(address);
    assert(uint64_t(a) + 4 <= memory.size());
    for (unsigned i=0; i<4; ++i) memory[a+i] = uint8_t(value >> (24-i*8));
}
static uint64_t ppc_rldicl(uint64_t value, unsigned shift, unsigned mask) {
    // The audited loop only zero-extends addresses; refuse a changed operation.
    assert(shift == 0 && mask == 32);
    return static_cast<uint32_t>(value);
}
#include "vp_packet_reference.inc"

int main() {
    size_t cases = 0;
    for (size_t count=1; count<=64; ++count)
        for (size_t sa=0; sa<16; ++sa)
            for (size_t da=0; da<16; ++da) {
                memory.assign(0x10000, 0xA5);
                const uint32_t source = 0x1000 + uint32_t(sa);
                const uint32_t output = 0x5000 + uint32_t(da);
                for (size_t i=0; i<count*128; ++i)
                    memory[source+i] = uint8_t((i*37 + i/17 + sa*13 + count) & 255);
                vm_write32(0x108, output);
                auto expected = memory;
                ppu_context ctx;
                ctx.gpr[3] = 0x00800B80; ctx.gpr[4] = source;
                ctx.gpr[29] = 0x100; ctx.ctr = uint32_t(count);
                reference_vp_blocks(&ctx);
                assert(vm_read32(0x108) == output + count*132 && ctx.ctr == 0);
                // Mirror the caller-owned cursor update separately. Native
                // packer must write only the output span, with no scratch state.
                std::memcpy(expected.data()+0x108, memory.data()+0x108, 4);
                assert(haze::pack_vp_blocks(expected.data()+source, count*128,
                    expected.data()+output, count*132, count));
                assert(expected == memory);
                ++cases;
            }
    assert(haze::pack_vp_blocks(nullptr, 0, nullptr, 0, 0));
    std::array<uint8_t,512> bytes{};
    const auto unchanged = bytes;
    auto rejected = [&](const uint8_t* src, size_t sn, uint8_t* dst, size_t dn, size_t n) {
        assert(!haze::pack_vp_blocks(src,sn,dst,dn,n));
        assert(bytes == unchanged);
    };
    rejected(nullptr,128,bytes.data(),132,1);
    rejected(bytes.data(),128,nullptr,132,1);
    rejected(bytes.data(),127,bytes.data()+256,132,1);
    rejected(bytes.data(),128,bytes.data()+256,131,1);
    rejected(bytes.data(),128,bytes.data(),132,1);
    rejected(bytes.data(),128,bytes.data()+127,132,1);
    rejected(bytes.data()+128,128,bytes.data(),132,1);
    rejected(bytes.data(),SIZE_MAX,bytes.data()+256,SIZE_MAX,SIZE_MAX);
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si);
    auto guarded = [&]() {
        auto p=static_cast<uint8_t*>(VirtualAlloc(nullptr,si.dwPageSize*2,
            MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE)); assert(p);
        DWORD previous; assert(VirtualProtect(p+si.dwPageSize,si.dwPageSize,PAGE_NOACCESS,&previous));
        return p;
    };
    auto src=guarded(), dst=guarded();
    for (size_t n=1; n<=si.dwPageSize/132; ++n) {
        auto s=src+si.dwPageSize-n*128, d=dst+si.dwPageSize-n*132;
        std::memset(s,0x3B,n*128);
        assert(haze::pack_vp_blocks(s,n*128,d,n*132,n));
        assert(d[n*132-1] == 0x3B);
    }
    assert(VirtualFree(src,0,MEM_RELEASE)); assert(VirtualFree(dst,0,MEM_RELEASE));
#endif
    std::printf("VP packet kernel: %zu extracted-loop comparisons; rejection and guard-page checks passed\n",cases);
}
