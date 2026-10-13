#include "ppu_recomp.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t title_hash=0xb3a1b9c6; // STR_PRESS_START, verified against the disc string-ID table.
constexpr uint32_t start_hash=0xfaf5863e; // STR_CONTROLLERDESCRIPTION_START = {E}.
// BLUS30094's complete eight-instruction lookup plus return. Refuse other code
// layouts even if a differently sourced ELF is supplied with this translation.
bool supported() {
    constexpr uint32_t code[]={0x7ca42a14,0x80630020,0x54a5103a,0x7ca51a14,
        0x78a50020,0x80050000,0x7c630214,0x78630020,0x4e800020};
    for (uint32_t i=0;i<9;i++) if (vm_read32(0x5451a0+i*4)!=code[i]) return false;
    return true;
}
// Hashes are sorted, with a parallel big-endian u16 row-index table. The five
// offsets in each row have already been endian-converted by the guest loader.
uint32_t row_for(uint32_t file,uint32_t count,uint32_t hash) {
    uint32_t lo=0,hi=count;
    const uint32_t hashes=vm_read32(file+8), rows=vm_read32(file+12);
    if (!hashes || !rows || uint64_t(hashes)+uint64_t(count)*4>UINT32_MAX ||
        uint64_t(rows)+uint64_t(count)*2>UINT32_MAX) return UINT32_MAX;
    while (lo<hi) {
        const auto mid=lo+(hi-lo)/2;
        if (vm_read32(hashes+mid*4)<hash) lo=mid+1; else hi=mid;
    }
    return lo<count && vm_read32(hashes+lo*4)==hash ? vm_read16(rows+lo*2) : UINT32_MAX;
}
bool read_text(uint32_t address,char* out,size_t capacity) {
    if (!address || uint64_t(address)+capacity>UINT32_MAX) return false;
    for (size_t i=0;i<capacity;i++) {
        out[i]=char(vm_read8(address+i));
        if (!out[i]) return true;
    }
    return false;
}
}

void haze_title_prepare_text(uint32_t file,uint32_t entry,uint32_t field) {
    // Column 4 is display text. Other columns contain flags and audio metadata.
    // 'entry' is five times the row index, not the localized string's hash.
    if (field!=4 || entry%5 || !file || file>UINT32_MAX-0x28 || !supported()) return;
    const uint32_t data=vm_read32(file+0x24);
    if (!data) return;
    const uint32_t count=vm_read32(data);
    if (!count || count>65535 || entry/5>=count || row_for(file,count,title_hash)!=entry/5) return;
    const uint32_t table=vm_read32(file+0x20);
    if (!table || uint64_t(table)+uint64_t(count)*20>UINT32_MAX) return;
    const uint32_t start=row_for(file,count,start_hash);
    if (start>=count) return;
    const uint64_t start_text=uint64_t(table)+vm_read32(table+start*20+16);
    char glyph[8];
    if (start_text>UINT32_MAX || !read_text(uint32_t(start_text),glyph,sizeof glyph) || std::strcmp(glyph,"{E}")) return;
    const uint64_t address=uint64_t(table)+vm_read32(table+entry*4+16);
    char original[96];
    if (address>UINT32_MAX || !read_text(uint32_t(address),original,sizeof original)) return;
    char replacement[96];
    if (!std::strcmp(original,"Press START Button")) {
        std::strcpy(replacement,"Press {E} to begin");
    } else {
        // Keep translated wording in the supplied French, Spanish, Italian,
        // German and Russian resources; only the literal button label changes.
        const char* label=std::strstr(original,"START");
        if (!label || std::strstr(label+5,"START")) return;
        const size_t prefix=size_t(label-original);
        std::memcpy(replacement,original,prefix);
        std::strcpy(replacement+prefix,"{E}");
        std::strcpy(replacement+prefix+3,label+5);
    }
    // Reuse the existing allocation, including its terminator. Never grow a
    // guest resource or retain pointers after its loader releases it.
    const size_t bytes=std::strlen(replacement)+1;
    if (bytes>std::strlen(original)+1) return;
    for (size_t i=0;i<bytes;i++) vm_write8(address+i,uint8_t(replacement[i]));
    std::fprintf(stderr,"[title] Device-aware Start prompt prepared in the localized game layout\n");
}
