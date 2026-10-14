#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <fstream>
#include <cstdio>
static HWND foreground;
static HWND test_foreground() { return foreground; }
static BOOL test_activate(HWND hwnd) { foreground=hwnd; return TRUE; }
static HWND test_focus(HWND) { return nullptr; }
static BOOL test_show(HWND,int) { return TRUE; }
#define GetForegroundWindow test_foreground
#define SetForegroundWindow test_activate
#define SetFocus test_focus
#define ShowWindow test_show
#include "../port/haze_quit_menu.cpp"
#include "../port/haze_quit.cpp"
#include "../port/haze_save_status.cpp"
#include "../port/haze_menu.cpp"
#include "../port/haze_steam_menu.cpp"
static HazeQuitOffer offer{1,true,false};
static unsigned guest_backs=0,retail_returns=0,installs=0;
static bool steam_available=true,install_success=true;
static std::string installed_account;
void haze_input_menu_return() { ++guest_backs; }
void haze_input_menu_retail() { ++retail_returns; }
unsigned haze_input_menu_button(int vk) { return vk==VK_TAB ? CELL_PAD_CTRL_SELECT : vk=='J' ? CELL_PAD_CTRL_CROSS : vk=='K' ? CELL_PAD_CTRL_CIRCLE : vk=='L' ? CELL_PAD_CTRL_SQUARE : 0; }
bool haze_prompts_draw_icon(void*,int,int,int,int) { return true; }
bool haze_prompts_draw_binding(void*,HazeInputFamily,const char*,int,int,int) { return true; }
HazeInputFamily haze_input_family() { return HazeInputFamily::Keyboard; }
HazeQuitOffer haze_quit_offer() { return offer; }
bool haze_quit_guest_ready() { return true; }
void haze_quit_resume_guest() {}
std::vector<HazeSteamProfile> haze_steam_profiles(std::string& message) {
    message=steam_available ? "" : "Steam was not found. Install Steam and sign in once, then retry.";
    if (!steam_available) return {};
    return {{L"C:/fixture",L"C:/fixture/userdata/42/config","42","Zo\xc3\xab"},{L"C:/fixture",L"C:/fixture/userdata/7/config","7","Zo\xc3\xab"}};
}
HazeSteamResult haze_steam_install(const HazeSteamProfile& profile,const HazeSteamTarget& target) {
    ++installs; installed_account=profile.account_id; assert(target.launcher==target.icon);
    assert(target.arguments=="--shared-root \"C:/Haze data\" --savedata \"C:/My saves\"");
    return {install_success,install_success,0x80000042u,{},install_success ? "Haze Recomp was added. Start Steam to see it in the selected library." : "Exit Steam using Steam > Exit, wait for it to close, then retry."};
}
static HWND owner;
static void frame() { haze_menu_update(owner,{}); }
static void send(HazeMenuCommandKind kind,const std::string& key={},const std::string& value={},int choice=0) {
    const auto view=haze_menu_view();
    haze_menu_command({view.generation,kind,key,value,choice,view.modal.revision}); frame();
}
static void open() { assert(haze_menu_request(HazeMenuPage::Desktop)); frame(); }
int main() {
    using K=HazeMenuCommandKind;
    const auto fixture=std::filesystem::absolute("local/steam-menu-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(fixture); const auto launcher=fixture/L"Haze Launcher \u00e9.exe";
    { std::ofstream file(launcher); file<<"fixture"; }
    SetEnvironmentVariableW(L"HAZE_LAUNCHER_PATH",launcher.c_str());
    SetEnvironmentVariableW(L"HAZE_LAUNCHER_ARGUMENTS",L"--shared-root \"C:/Haze data\" --savedata \"C:/My saves\"");
    owner=reinterpret_cast<HWND>(uintptr_t(1)); foreground=owner;
    haze_steam_menu_register(); open();
    assert(haze_menu_view().rows.size()==3 && haze_menu_view().title=="ADD TO STEAM");
    assert(steam_menu::selected.empty()); send(K::Apply); assert(!installs && haze_menu_view().error);
    const auto accounts=steam_menu::rows()[0].choices;
    assert(accounts[1].value=="42" && accounts[1].label=="Account 42 - Zo\xc3\xab");
    assert(accounts[2].value=="7" && accounts[2].label=="Account 7 - Zo\xc3\xab");
    send(K::Change,"profile","42"); send(K::Apply);
    assert(steam_menu::confirming && haze_menu_view().modal.cancel_choice==0);
    send(K::Back); assert(!installs && !haze_menu_view().modal.revision && haze_menu_active());
    send(K::Apply); foreground=nullptr; frame(); assert(!installs && !haze_menu_view().modal.revision);
    foreground=owner; send(K::Apply); send(K::Modal,{}, {},1);
    assert(installs==1 && installed_account=="42" && haze_menu_view().modal.title=="Steam shortcut ready");
    send(K::Modal); send(K::Defaults); assert(steam_menu::selected.empty());
    send(K::Back); assert(!haze_menu_active() && !haze_menu_view().pop);
    open(); send(K::Change,"profile","42"); install_success=false; send(K::Apply); send(K::Modal,{}, {},1);
    assert(installs==2 && haze_menu_view().modal.title=="Could not add to Steam"); send(K::Modal);
    send(K::Apply); ++offer.generation; send(K::Modal,{}, {},1); assert(installs==2); send(K::Modal); send(K::Back);
    std::filesystem::remove(launcher); open(); send(K::Change,"profile","42"); send(K::Apply);
    assert(haze_menu_view().modal.title=="Launcher required" && installs==2); send(K::Modal);
    steam_available=false; send(K::Defaults); send(K::Apply);
    assert(haze_menu_view().notice.find("Steam was not found")!=std::string::npos && installs==2);
    send(K::Back); haze_menu_shutdown(); std::filesystem::remove_all(fixture);
    std::puts("Native Steam provider: account selection, confirmation/cancel, focus loss, refresh, failures, stale menu and missing dependencies passed.");
}
