#include "ppu_recomp.h"
#include "ppu_native_memory.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

bool haze_vp_upload_native(ppu_context*);
extern "C" {
uint8_t* vm_base=nullptr;
uint32_t ppu_vm_size=0x40000,ppu_hle_inject_base=0x30000;
}
static bool available=true,context_ok=true,locked=false;
static uint32_t reserved=UINT32_MAX;
static unsigned lock_count,unlock_count;
extern "C" int ppu_native_memory_available() { return available; }
extern "C" int cellGcm_native_context_matches(uint32_t a) { return context_ok && a==0x11000; }
extern "C" void spu_lockline_lock() { assert(!locked); locked=true; ++lock_count; }
extern "C" void spu_lockline_unlock() { assert(locked); locked=false; ++unlock_count; }
extern "C" int spu_coh_is_reserved(uint32_t a) { assert(locked); return a==reserved; }

static uint32_t vm_read32(uint64_t a) {
    assert(a+4<=ppu_vm_size);
    return uint32_t(vm_base[a])<<24 | uint32_t(vm_base[a+1])<<16 |
        uint32_t(vm_base[a+2])<<8 | vm_base[a+3];
}
static void vm_write32(uint64_t a,uint32_t v) {
    assert(a+4<=ppu_vm_size);
    for(unsigned i=0;i<4;++i)vm_base[a+i]=uint8_t(v>>(24-i*8));
}
static uint64_t ppc_rldicl(uint64_t v,unsigned shift,unsigned mask) {
    assert(shift==0 && mask==32); return uint32_t(v);
}
#include "vp_packet_reference.inc"

static ppu_context prepare(std::vector<uint8_t>& memory,unsigned n,unsigned sa=0,unsigned da=0) {
    memory.assign(ppu_vm_size,0xCD);vm_base=memory.data();
    ppu_context c;std::memset(&c,0x5A,sizeof c);
    c.thread_id=1;c.gpr[2]=0xE94EE0;c.gpr[3]=0x00800B80;c.gpr[4]=c.gpr[23]=0x13000+sa;
    c.gpr[29]=0x11000;c.gpr[31]=c.ctr=n;
    vm_write32(0x11000,0x18000);vm_write32(0x11004,0x18000+da+n*132);
    vm_write32(0x11008,0x18000+da);
    for(unsigned i=0;i<n*128;++i)memory[0x13000+sa+i]=uint8_t(i*37+i/17+n+sa);
    return c;
}
int main() {
    std::vector<uint8_t> memory;
    unsigned cases=0;
    for(unsigned n=1;n<=64;++n)for(unsigned sa=0;sa<16;sa+=4)for(unsigned da=0;da<16;da+=4) {
        auto native=prepare(memory,n,sa,da), reference=native;
        const auto original=memory;
        reference_vp_blocks(&reference);const auto expected=memory;
        memory=original;vm_base=memory.data();
        assert(haze_vp_upload_native(&native));
        assert(!locked && lock_count==unlock_count);
        assert(memory==expected && !std::memcmp(&native,&reference,sizeof native));
        ++cases;
    }
    for(unsigned mutation=0;mutation<19;++mutation) {
        auto c=prepare(memory,2);available=context_ok=true;reserved=UINT32_MAX;
        switch(mutation) {
        case 0:c.thread_id=2;break;
        case 1:c.gpr[2]++;break;
        case 2:c.ctr=0;break;
        case 3:c.ctr=c.gpr[31]=65;break;
        case 4:c.gpr[31]++;break;
        case 5:c.gpr[3]++;break;
        case 6:c.gpr[4]++;break;
        case 7:c.gpr[4]=c.gpr[23]=UINT64_C(0x100013000);break;
        case 8:c.gpr[29]=UINT64_C(0x100011000);break;
        case 9:c.gpr[29]++;break;
        case 10:c.gpr[4]=c.gpr[23]=0x13001;break;
        case 11:available=false;break;
        case 12:context_ok=false;break;
        case 13:reserved=0x13080;break; // second source line
        case 14:reserved=0x18100;break; // final destination line
        case 15:reserved=0x11000;break;
        case 16:vm_write32(0x11004,0x18000+263);break; // one byte short
        case 17:vm_write32(0x11008,0x13000);break; // source overlap
        case 18:c.gpr[4]=c.gpr[23]=ppu_hle_inject_base;break;
        }
        auto before=c;const auto bytes=memory;
        assert(!haze_vp_upload_native(&c));
        assert(!locked && lock_count==unlock_count);
        assert(memory==bytes && !std::memcmp(&c,&before,sizeof c));
    }
    available=true;reserved=UINT32_MAX;
    assert(!ppu_native_memory_begin(0x13000,0,0x18000,132,0x11000,16));
    assert(!ppu_native_memory_begin(0x13000,128,0x18000,132,0x18004,16));
    assert(!ppu_native_ram_range(0xFFFF,2,0,0x3000000));
    assert(!ppu_native_ram_range(0xFFFFFFFC,8,0,0x3000000));
    assert(!ppu_native_ram_range(0xBFFFFFFC,8,0,0x3000000));
    assert(!ppu_native_ram_range(0x2FFFFFF,2,0,0x3000000));
    assert(!ppu_native_ram_range(0x3007FFF,2,0,0x3000000));
    assert(ppu_native_ram_range(0x3008000,4,0,0x3000000));
    assert(!ppu_native_ram_range(0x13000,128,0x1307F,0x3000000));
    assert(ppu_native_ram_range(0x13000,128,0x13080,0x3000000));
    std::printf("Native VP adapter: %u full-context/memory comparisons; guards and locked reservations passed\n",cases);
}
