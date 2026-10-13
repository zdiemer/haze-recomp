#include "haze_steam.h"
#include "haze_menu.h"
#include "haze_quit.h"
#include "haze_quit_menu.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>

namespace steam_menu {
std::vector<HazeSteamProfile> profiles;
HazeSteamTarget target;
std::string selected,discovery;
HazeMenuModal dialog;
unsigned revision=0;
uint64_t menu_generation=0;
bool confirming=false;
std::string utf8(const std::filesystem::path& path) { const auto u=path.u8string(); return {u.begin(),u.end()}; }
std::string utf8_text(const std::wstring& text) {
    if (text.empty()) return {};
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    if (!size) return {};
    std::string result(size_t(size),'\0'); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),size,nullptr,nullptr); return result;
}
std::wstring environment(const wchar_t* name) {
    const DWORD size=GetEnvironmentVariableW(name,nullptr,0); if (!size || size>32768) return {};
    std::wstring value(size,L'\0'); const auto length=GetEnvironmentVariableW(name,value.data(),size);
    if (!length || length>=size) return {}; value.resize(length); return value;
}
void refresh() {
    selected.clear(); dialog={}; confirming=false;
    profiles=haze_steam_profiles(discovery);
    wchar_t executable[32768]{}; const auto length=GetModuleFileNameW(nullptr,executable,32768);
    const auto configured=environment(L"HAZE_LAUNCHER_PATH");
    target={};
    if (!configured.empty()) target.launcher=configured;
    else if (length && length<32768) target.launcher=std::filesystem::path(executable).parent_path()/L"HazeLauncher.exe";
    target.working_directory=target.launcher.parent_path(); target.icon=target.launcher;
    target.arguments=utf8_text(environment(L"HAZE_LAUNCHER_ARGUMENTS"));
    const auto disc=environment(L"PS3_VFS_ROOT");
    if (!disc.empty()) target.artwork_directory=std::filesystem::path(disc)/L"PS3_GAME";
}
void message(const std::string& title,const std::string& text) {
    confirming=false; dialog={++revision,title,text,{"OK"},false};
}
bool current() { const auto offer=haze_quit_offer(); return offer.available && !offer.gameplay && offer.generation==menu_generation; }
std::string profile_label(const HazeSteamProfile& profile) {
    return "Account "+profile.account_id+(profile.persona_name.empty() ? "" : " - "+profile.persona_name);
}
std::vector<HazeMenuRow> rows() {
    HazeMenuRow quit; quit.key="quit"; quit.label="Quit to Desktop"; quit.action=true;
    quit.hint="Open the quit confirmation. Cancel keeps Haze open.";
    HazeMenuRow account; account.key="profile"; account.label="Steam profile"; account.value=selected;
    account.hint=discovery.empty() ? "Choose the local Steam account to update. No account is selected automatically." : discovery;
    account.choices.push_back({"","Choose a profile"});
    for (const auto& profile:profiles) account.choices.push_back({profile.account_id,profile_label(profile)});
    HazeMenuRow launcher; launcher.key="launcher"; launcher.label="Launcher"; launcher.value=utf8(target.launcher.filename());
    launcher.hint="Target: "+utf8(target.launcher)+". The executable launcher must be installed before adding Haze.";
    HazeMenuRow artwork; artwork.key="artwork"; artwork.label="Library artwork"; artwork.value="From local disc";
    artwork.hint="Uses local game artwork. Existing custom covers, banners and logos are retained.";
    return {quit,account,launcher,artwork};
}
HazeMenuApply apply(std::string& notice) {
    if (!current()) { notice="Return to the main menu and reopen this page."; return HazeMenuApply::Failed; }
    if (selected.empty()) { notice=discovery.empty() ? "Choose a Steam profile first." : discovery; return HazeMenuApply::Failed; }
    const auto profile=std::find_if(profiles.begin(),profiles.end(),[](const auto& p){return p.account_id==selected;});
    if (profile==profiles.end()) { notice="Refresh the Steam profiles and select an account again."; return HazeMenuApply::Failed; }
    std::error_code ec;
    if (!target.launcher.is_absolute() || !std::filesystem::is_regular_file(target.launcher,ec)) {
        notice="The executable launcher is missing. Install/build HazeLauncher.exe first, then use Refresh.";
        message("Launcher required",notice); return HazeMenuApply::Failed;
    }
    confirming=true;
    dialog={++revision,"Add to Steam","Add or repair Haze Recomp for this Steam profile?\n"+profile_label(*profile)+"\n\nLauncher: "+utf8(target.launcher)+
        "\n\nExit Steam first. Existing unrelated shortcuts and custom artwork will be retained.",{"Cancel","Add to Steam"},false,0};
    notice="Review the selected Steam account and launcher."; return HazeMenuApply::Applied;
}
void choose(int index) {
    if (!dialog.revision) return;
    if (!confirming || index!=1) { dialog={}; confirming=false; return; }
    if (!current()) { message("Main menu changed","Return to the main menu and reopen this page before adding Haze."); return; }
    const auto profile=std::find_if(profiles.begin(),profiles.end(),[](const auto& p){return p.account_id==selected;});
    if (profile==profiles.end()) { message("Select a profile","Refresh the Steam profile list and choose an account again."); return; }
    const auto result=haze_steam_install(*profile,target);
    message(result.success ? "Steam shortcut ready" : "Could not add to Steam",result.message);
}
bool open() {
    const auto offer=haze_quit_offer(); if (!offer.available || offer.gameplay || haze_quit_menu_active()) return false;
    menu_generation=offer.generation; return haze_menu_request(HazeMenuPage::Desktop);
}
}
void haze_steam_menu_register() {
    using namespace steam_menu;
    HazeMenuProvider provider;
    provider.title="DESKTOP OPTIONS"; provider.begin=steam_menu::refresh; provider.defaults=steam_menu::refresh; provider.rows=steam_menu::rows;
    provider.change=[](const std::string& key,const std::string& value) {
        if (key!="profile") return;
        if (value.empty() || std::any_of(profiles.begin(),profiles.end(),[&](const auto& p){return p.account_id==value;})) steam_menu::selected=value;
    };
    provider.apply=steam_menu::apply; provider.modal=[] {return steam_menu::dialog;}; provider.modal_choose=steam_menu::choose;
    provider.activate=[](const std::string& key) {
        if (key=="quit" && steam_menu::current()) {
            const auto offer=haze_quit_offer(); haze_menu_retail(); haze_quit_request(false,offer.generation);
        }
    };
    provider.update=[](bool focused) { if (confirming && !focused) { steam_menu::dialog={}; confirming=false; } };
    provider.actions={"Add to Steam","Refresh","Back"};
    provider.footer_hint="Exit Steam, select the intended account, then choose Add to Steam. Haze remains usable without Steam.";
    provider.defaults_notice="Steam profiles refreshed. Choose an account before adding Haze.";
    provider.guest_back=false;
    haze_menu_register(HazeMenuPage::Desktop,std::move(provider)); haze_quit_menu_main_action(steam_menu::open);
}
#else
void haze_steam_menu_register() {}
#endif
