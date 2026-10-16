#include "ppu_recomp.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
struct RemovedRow {
    uint32_t context, parent, widget, flags;
    uint64_t entry;
};
std::vector<RemovedRow> removed_rows;
bool valid(uint32_t p) { return p>=0x10000 && p<0xbffff000 && !(p&3); }
bool obsolete(uint32_t widget) {
    char name[17]{};
    for (unsigned i=0;i<16;++i) name[i]=char(vm_read8(widget+0x18+i));
    // BLUS30094 frontend resources: PSN sign-in action and startup status.
    return !std::strcmp(name,"fefl_DC_text7") || !std::strcmp(name,"fefl_KS_text45");
}
uint32_t resolve(uint32_t context,uint32_t handle) {
    const auto index=handle>>20, data=vm_read32(context+0x1f0), stride=vm_read32(context+0x1f8);
    if (!handle || index>=vm_read32(context+0x1f4) || !valid(data) || stride<8 || stride>0x100) return 0;
    const uint64_t slot=uint64_t(data)+uint64_t(index)*stride;
    if (slot+8>=0xc0000000 || vm_read32(slot+4)!=handle) return 0;
    const auto widget=vm_read32(slot);
    return valid(widget) ? widget : 0;
}
}

void haze_menu_cleanup(uint32_t context) {
    if (!valid(context)) return;
    const auto data=vm_read32(context+0x1f0), stride=vm_read32(context+0x1f8), count=vm_read32(context+0x1f4);
    if (!valid(data) || stride<8 || stride>0x100 || count>65536 || uint64_t(data)+uint64_t(stride)*count>=0xc0000000) return;
    // Remove the actual child-list entry, so native layout and keyboard, pad
    // and pointer traversal all see the same compact list. Keep the widget in
    // the context registry: retail callbacks may still look it up by ID.
    for (uint32_t i=0;i<count;++i) {
        const auto parent=resolve(context,vm_read32(data+i*stride+4));
        if (!parent || vm_read32(parent+0xcc)!=context) continue;
        const auto items=vm_read32(parent+0x64), size=vm_read32(parent+0x60);
        if (!size || size>512 || size>vm_read32(parent+0x5c) || !valid(items)) continue;
        uint32_t kept=0;
        for (uint32_t j=0;j<size;++j) {
            const auto entry=vm_read64(items+j*8);
            const auto widget=resolve(context,uint32_t(entry>>32));
            if (widget && obsolete(widget)) {
                removed_rows.push_back({context,parent,widget,vm_read32(widget+0x30),entry});
                vm_write32(widget+0x30,vm_read32(widget+0x30)&~3u);
            } else vm_write64(items+kept++*8,entry);
        }
        vm_write32(parent+0x60,kept);
    }
}

void haze_menu_cleanup_restore(uint32_t context,bool (*reserve)(void*,uint32_t,uint32_t)=nullptr,void* owner=nullptr) {
    // Restore ownership before 00317300 recursively deletes the GUI roots.
    // This is not a page-close operation: the rows stay absent on Back.
    for (auto it=removed_rows.begin();it!=removed_rows.end();) {
        if (it->context!=context) { ++it; continue; }
        const auto count=vm_read32(it->parent+0x60);
        if(count>=vm_read32(it->parent+0x5c) && reserve) reserve(owner,it->parent,count+1);
        const auto items=vm_read32(it->parent+0x64);
        if (valid(items) && count<vm_read32(it->parent+0x5c)) {
            vm_write64(items+count*8,it->entry);
            vm_write32(it->parent+0x60,count+1);
            vm_write32(it->widget+0x30,it->flags);
        }
        it=removed_rows.erase(it);
    }
}
