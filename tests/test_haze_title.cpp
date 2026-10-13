#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "../port/haze_title.cpp"

namespace {
std::vector<uint8_t> memory(0x550000);
unsigned writes;
void word(uint32_t addr,uint32_t value) {
    for (int i=0;i<4;i++) memory.at(addr+i)=uint8_t(value>>(24-8*i));
}
void text(uint32_t addr,const char* value) {
    std::memcpy(memory.data()+addr,value,std::strlen(value)+1);
}
void prepare(const char* value="Press START Button") {
    std::fill(memory.begin(),memory.end(),0xcc);
    constexpr uint32_t instructions[]={0x7ca42a14,0x80630020,0x54a5103a,0x7ca51a14,
        0x78a50020,0x80050000,0x7c630214,0x78630020,0x4e800020};
    for (uint32_t i=0;i<9;i++) word(0x5451a0+i*4,instructions[i]);
    word(0x1024,0x2000); word(0x2000,3);
    word(0x1008,0x2004); word(0x100c,0x2020); word(0x1020,0x3000);
    word(0x2004,1); word(0x2008,0xb3a1b9c6); word(0x200c,0xfaf5863e);
    memory[0x2020]=0; memory[0x2021]=2;
    memory[0x2022]=0; memory[0x2023]=0;
    memory[0x2024]=0; memory[0x2025]=1;
    word(0x3010,0x100); word(0x3024,0x180); word(0x3038,0x200);
    text(0x3100,value); text(0x3180,"{E}"); text(0x3200,"Press START Button");
    writes=0;
}
void unchanged(uint32_t file=0x1000,uint32_t row=0,uint32_t field=4) {
    auto before=memory;
    haze_title_prepare_text(file,row,field);
    assert(memory==before && writes==0);
}
}
uint8_t vm_read8(uint64_t address) { return memory.at(size_t(address)); }
uint16_t vm_read16(uint64_t address) { return uint16_t(vm_read8(address)<<8)|vm_read8(address+1); }
uint32_t vm_read32(uint64_t address) { return uint32_t(vm_read16(address))<<16|vm_read16(address+2); }
void vm_write8(uint64_t address,uint8_t value) { memory.at(size_t(address))=value; ++writes; }

int main() {
    prepare();
    auto before=memory;
    haze_title_prepare_text(0x1000,0,4);
    assert(!std::strcmp((char*)memory.data()+0x3100,"Press {E} to begin"));
    assert(writes==19);
    for (size_t i=0;i<memory.size();i++)
        if (i<0x3100 || i>=0x3100+19) assert(memory[i]==before[i]);
    writes=0; unchanged(); // Repeated lookup does not rewrite.
    prepare(); unchanged(0x1000,10,4); // Same text in another string ID is untouched.
    prepare(); unchanged(0x1000,0,2); // Audio/flags column is untouched.
    prepare(); unchanged(0x1000,1,4); // Misaligned row.
    prepare(); unchanged(0x1000,15,4); // Past the last row.
    prepare(); word(0x5451ac,0); unchanged(); // Different ELF.
    prepare(); word(0x2008,0xb3a1b9c7); unchanged(); // Wrong localization identity.
    prepare(); text(0x3180,"{X}"); unchanged(); // Do not assume the Start markup.
    prepare(); word(0x2000,65536); unchanged();
    prepare(); word(0x1008,0); unchanged();
    prepare(); word(0x1020,UINT32_MAX-4); unchanged();
    prepare(); word(0x3010,UINT32_MAX); unchanged();
    prepare(); std::fill_n(memory.begin()+0x3100,96,'A'); unchanged();
    prepare("Hold something else"); unchanged();
    prepare("START START"); unchanged();
    for (const auto& pair : {std::pair{"Appuyez sur la touche START","Appuyez sur la touche {E}"},
                           std::pair{"Pulsa el bot\xc3\xb3n START","Pulsa el bot\xc3\xb3n {E}"},
                           std::pair{"Premi il tasto START","Premi il tasto {E}"},
                           std::pair{"START-Taste dr\xc3\xbc" "cken","{E}-Taste dr\xc3\xbc" "cken"}}) {
        prepare(pair.first); const size_t extent=std::strlen(pair.first)+1;
        haze_title_prepare_text(0x1000,0,4);
        assert(!std::strcmp((char*)memory.data()+0x3100,pair.second));
        assert(writes<=extent && memory[0x3100+extent]==0xcc);
    }
    puts("Title localization identity, Start markup, bounds, language and repeat-lookup tests passed.");
}
