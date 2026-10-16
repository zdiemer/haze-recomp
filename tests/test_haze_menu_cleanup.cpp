#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
uint64_t vm_read64(uint64_t);
void vm_write64(uint64_t,uint64_t);
#include "../port/haze_menu_cleanup.cpp"

std::vector<uint8_t> memory(0x100000);
uint8_t vm_read8(uint64_t p) { return memory.at(size_t(p)); }
uint32_t vm_read32(uint64_t p) {
    return uint32_t(vm_read8(p))<<24 | uint32_t(vm_read8(p+1))<<16 | uint32_t(vm_read8(p+2))<<8 | vm_read8(p+3);
}
uint64_t vm_read64(uint64_t p) { return uint64_t(vm_read32(p))<<32 | vm_read32(p+4); }
void vm_write32(uint64_t p,uint32_t v) { for (unsigned i=0;i<4;++i) memory.at(size_t(p+i))=uint8_t(v>>(24-i*8)); }
void vm_write64(uint64_t p,uint64_t v) { vm_write32(p,uint32_t(v>>32)); vm_write32(p+4,uint32_t(v)); }
int main() {
    constexpr uint32_t context=0x10000, registry=0x20000, parent=0x30000, items=0x40000;
    vm_write32(context+0x1f0,registry); vm_write32(context+0x1f4,5); vm_write32(context+0x1f8,8);
    for (unsigned i=0;i<5;++i) {
        const auto widget=parent+i*0x1000, handle=(i<<20)|1;
        vm_write32(registry+i*8,widget); vm_write32(registry+i*8+4,handle);
        vm_write32(widget+0xcc,context); vm_write32(widget+0x30,7);
        if (i) vm_write64(items+(i-1)*8,(uint64_t(handle)<<32)|i);
    }
    std::memcpy(memory.data()+parent+0x1000+0x18,"Campaign",9);
    std::memcpy(memory.data()+parent+0x2000+0x18,"fefl_DC_text7",14);
    std::memcpy(memory.data()+parent+0x3000+0x18,"fefl_KS_text45",14);
    std::memcpy(memory.data()+parent+0x4000+0x18,"Multiplayer",12);
    vm_write32(parent+0x64,items); vm_write32(parent+0x60,4); vm_write32(parent+0x5c,8);
    const auto first=vm_read64(items),last=vm_read64(items+24);
    haze_menu_cleanup(context);
    assert(vm_read32(parent+0x60)==2 && vm_read64(items)==first && vm_read64(items+8)==last);
    assert(vm_read32(parent+0x2000+0x30)==4 && vm_read32(parent+0x3000+0x30)==4);
    haze_menu_cleanup(context); assert(removed_rows.size()==2); // idempotent reactivation
    haze_menu_cleanup_restore(context+0x1000); assert(removed_rows.size()==2);
    // New native menu rows must survive ownership restoration.
    vm_write64(items+16,0x123400005678ULL); vm_write32(parent+0x60,3);
    haze_menu_cleanup_restore(context);
    assert(removed_rows.empty() && vm_read32(parent+0x60)==5);
    assert(vm_read64(items+16)==0x123400005678ULL && vm_read32(parent+0x2000+0x30)==7);
    // A stale handle must not remove a widget from a later context generation.
    vm_write32(registry+2*8+4,(2<<20)|2);
    haze_menu_cleanup(context);
    assert(removed_rows.size()==1 && removed_rows[0].widget==parent+0x3000);
    haze_menu_cleanup_restore(context);
    // A later native row can consume all spare capacity. Restoration must ask
    // the guest allocator for room, preserving the live entries after growth.
    haze_menu_cleanup(context);
    const auto live=vm_read32(parent+0x60);
    vm_write32(parent+0x5c,live);
    bool grew=false;
    auto reserve=[](void* owner,uint32_t parent,uint32_t needed) {
        *static_cast<bool*>(owner)=true;
        const auto before=vm_read32(parent+0x64),count=vm_read32(parent+0x60);
        for(uint32_t i=0;i<count;++i) vm_write64(0x50000+i*8,vm_read64(before+i*8));
        vm_write32(parent+0x64,0x50000); vm_write32(parent+0x5c,needed+4); return true;
    };
    haze_menu_cleanup_restore(context,reserve,&grew);
    assert(grew && removed_rows.empty() && vm_read32(parent+0x60)==live+1);
    assert(vm_read64(0x50000)==first && vm_read32(parent+0x3000+0x30)==7);
    puts("Online menu cleanup: compact traversal, repeat activation, ownership and handle generations passed.");
}
