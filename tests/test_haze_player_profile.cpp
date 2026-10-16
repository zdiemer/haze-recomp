#include <cassert>
#include <cstdio>
#include <vector>
#include "../port/haze_player_profile.cpp"
#include "../port/haze_player_profile_menu.cpp"
#include "../port/haze_player_profile_guest.cpp"

HazeMenuProvider ui;
void haze_menu_register(HazeMenuPage page,HazeMenuProvider provider) { assert(page==HazeMenuPage::Profile); ui=std::move(provider); }
std::vector<uint8_t> memory(0x1000000,0xcc);
uint8_t vm_read8(uint64_t p) { return memory.at(size_t(p)); }
uint32_t vm_read32(uint64_t p) { return uint32_t(vm_read8(p))<<24 | uint32_t(vm_read8(p+1))<<16 | uint32_t(vm_read8(p+2))<<8 | vm_read8(p+3); }
void vm_write32(uint64_t p,uint32_t v) { for (unsigned i=0;i<4;++i) memory.at(size_t(p+i))=uint8_t(v>>(24-i*8)); }
void vm_write8(uint64_t p,uint8_t value) { memory.at(size_t(p))=value; }
extern "C" int32_t cellSysutilGetSystemParamString(int32_t,uint32_t address,uint32_t size) {
    if (!address || !size || uint64_t(address)+size>memory.size()) return -1;
    vm_write8(address,0); return 0;
}
int np_result=0;
extern "C" int32_t sceNpManagerGetNpId(void* address) {
    if (np_result) return np_result;
    const auto p=uintptr_t(address); assert(p && p+36<=memory.size());
    for (unsigned i=0;i<36;++i) vm_write8(p+i,0xab);
    return 0;
}
unsigned registrations=0;
extern "C" void ps3_hle_register_ctx(uint32_t,const char*,void(*)(ppu_context*)) { ++registrations; }
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),{}};
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/("haze-profile-test-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root/"BLUS30094PROFILE");
    const auto save=root/"BLUS30094PROFILE"/"_PROFILE.DAT";
    { std::ofstream file(save,std::ios::binary); file<<"progression sentinel"; }
    std::string error;
    for (const auto name:{"","ab","123name","A B","A/B","A\nB","NameIsTooLong12345","\xc3\xa9ric"})
        assert(!haze_player_name_valid(name,error) && !error.empty());
    for (const auto name:{"User","Player_2","Player-3","ABCDEFGHIJKLMNOP"}) assert(haze_player_name_valid(name,error));
    haze_player_profile_initialize(root); assert(haze_player_name()=="User");
    assert(haze_player_name_save("FirstPlayer",error));
    assert(!haze_player_name_save("bad name",error)); assert(haze_player_name()=="FirstPlayer");
    haze_player_profile_initialize(root); assert(haze_player_name()=="FirstPlayer");
    // Windows denies replacement while another process has the file open
    // without delete sharing. Neither memory nor persisted identity may change.
    HANDLE locked=CreateFileW((root/"player-name.txt").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    assert(locked!=INVALID_HANDLE_VALUE);
    assert(!haze_player_name_save("SecondPlayer",error)); assert(haze_player_name()=="FirstPlayer");
    CloseHandle(locked); assert(read(root/"player-name.txt")=="FirstPlayer\n");
    haze_player_profile_menu_register(); ui.begin();
    ui.activate("clear"); ui.change("character","A"); ui.change("character","b"); ui.change("character","c");
    assert(ui.apply(error)==HazeMenuApply::Applied && haze_player_name()=="Abc");
    ui.activate("type"); ui.modal_text(8); ui.modal_text('Z'); ui.modal_text(0x00e9);
    assert(ui.modal().message.find("Use only")!=std::string::npos);
    ui.modal_key(13); assert(!ui.modal().revision);
    assert(ui.apply(error)==HazeMenuApply::Applied && haze_player_name()=="AbZ");
    ui.activate("type"); ui.modal_text('X'); ui.modal_choose(1);
    assert(ui.apply(error)==HazeMenuApply::Applied && haze_player_name()=="AbZ");
    ui.activate("type"); ui.modal_text('X'); ui.update(false); assert(!ui.modal().revision);
    ui.begin(); assert(haze_player_name()=="AbZ");
    haze_player_profile_register(); assert(registrations==2);
    ppu_context ctx{}; ctx.gpr[3]=0x113; ctx.gpr[4]=128; ctx.gpr[5]=64;
    system_name(&ctx); assert(ctx.gpr[3]==0 && std::string(reinterpret_cast<char*>(memory.data()+128))=="AbZ");
    assert(memory[127]==0xcc && memory[192]==0xcc);
    ctx.gpr[3]=0x113; ctx.gpr[4]=128; ctx.gpr[5]=2; system_name(&ctx); assert(memory[128]=='A' && memory[129]==0);
    ctx.gpr[3]=0x131; ctx.gpr[4]=128; ctx.gpr[5]=64; system_name(&ctx); assert(memory[128]==0); // SDK key is not replaced
    ctx.gpr[3]=0x131; ctx.gpr[4]=0; ctx.gpr[5]=0; system_name(&ctx); assert(int64_t(ctx.gpr[3])==-1);
    ctx.gpr[3]=256; online_name(&ctx); assert(ctx.gpr[3]==0);
    assert(std::string(reinterpret_cast<char*>(memory.data()+256))=="AbZ" && memory[256+17]==0xab);
    np_result=-2; ctx.gpr[3]=320; online_name(&ctx); assert(int64_t(ctx.gpr[3])==-2 && memory[320]==0xcc);
    constexpr uint32_t profile=0x20000;
    vm_write32(0xf48de0,0x10000); vm_write32(0x10010,profile);
    haze_player_profile_tick(); assert(memory[profile+0x4d]==0xcc); // unsupported code
    constexpr uint32_t code[]={0x3863004d,0x7c0802a6,0x38a00040,0x78630020};
    for (unsigned i=0;i<4;++i) vm_write32(0x4d31f8+i*4,code[i]);
    const auto before=memory;
    haze_player_profile_tick();
    assert(std::string(reinterpret_cast<char*>(memory.data()+profile+0x4d))=="AbZ");
    for (size_t i=0;i<memory.size();++i) if (i<profile+0x4d || i>profile+0x50) assert(memory[i]==before[i]);
    assert(read(save)=="progression sentinel");
    haze_player_profile_initialize(root); assert(haze_player_name()=="AbZ");
    { std::ofstream file(root/"player-name.txt"); file<<"unsupported name"; }
    haze_player_profile_initialize(root); assert(haze_player_name()=="User" && !haze_player_profile_error().empty());
    assert(haze_player_name_save("Repaired",error));
    // Only this uniquely named fixture directory is removed.
    std::filesystem::remove_all(root);
    puts("Player profile: validation, atomic persistence/failure, input, guest ABI and save-file isolation passed.");
}
