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
const char* haze_input_config_path() { return ""; }
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
static void frame(HWND owner,unsigned buttons=0) {
    HazeGamepadState pad; pad.input.buttons=(u16)buttons; haze_menu_update(owner,pad); haze_quit_menu_update(owner,pad);
}
static void key(int vk) { SendMessage(panel,WM_KEYDOWN,vk,0); SendMessage(panel,WM_KEYUP,vk,0); }
static void click(HWND hwnd,RECT rect) { SendMessage(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM((rect.left+rect.right)/2,(rect.top+rect.bottom)/2)); SendMessage(hwnd,WM_LBUTTONUP,0,0); }
static void action(int index) { click(panel,action_rect(index)); }
static void screenshot(const char* path) {
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen); BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; void* pixels=nullptr;
    auto bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0); assert(bitmap && pixels); auto previous=SelectObject(dc,bitmap);
    SendMessage(panel,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT); GdiFlush();
    BITMAPFILEHEADER header{}; header.bfType=0x4d42; header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER); header.bfSize=header.bfOffBits+width*height*4;
    std::ofstream output(path,std::ios::binary); output.write((char*)&header,sizeof(header)); output.write((char*)&info.bmiHeader,sizeof(info.bmiHeader)); output.write((char*)pixels,width*height*4); assert(output.good());
    SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr,screen);
}
int main() {
    const auto fixture=std::filesystem::absolute("local/steam-menu-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(fixture); const auto launcher=fixture/L"Haze Launcher \u00e9.exe";
    { std::ofstream file(launcher); file<<"fixture"; }
    SetEnvironmentVariableW(L"HAZE_LAUNCHER_PATH",launcher.c_str());
    SetEnvironmentVariableW(L"HAZE_LAUNCHER_ARGUMENTS",L"--shared-root \"C:/Haze data\" --savedata \"C:/My saves\"");
    auto owner=CreateWindowA("STATIC","Steam menu fixture",WS_POPUP,0,0,1920,1080,nullptr,nullptr,GetModuleHandle(nullptr),nullptr); assert(owner);
    foreground=owner; haze_steam_menu_register(); frame(owner);
    // A real footer click may temporarily activate its own popup window.
    foreground=quit_ui::button;
    assert(SendMessage(quit_ui::button,WM_MOUSEACTIVATE,0,0)==MA_NOACTIVATE);
    click(quit_ui::button,RECT{0,0,10,10}); frame(owner);
    assert(foreground==panel);
    assert(haze_menu_active() && !haze_quit_menu_active() && current==HazeMenuPage::Desktop);
    assert(steam_menu::selected.empty()); action(0); assert(!installs && notice_error);
    const auto accounts=steam_menu::rows()[1].choices;
    assert(accounts[1].value=="42" && accounts[1].label=="Account 42 - Zo\xc3\xab");
    assert(accounts[2].value=="7" && accounts[2].label=="Account 7 - Zo\xc3\xab");
    auto unnamed=steam_menu::profiles[0]; unnamed.persona_name.clear();
    assert(steam_menu::profile_label(unnamed)=="Account 42");
    screenshot("local/steam-desktop-options.bmp");
    key(VK_UP); key(VK_UP); key(VK_UP); key(VK_RIGHT); assert(steam_menu::selected=="42");
    action(0); frame(owner); assert(steam_menu::confirming && modal_selected==0);
    assert(steam_menu::dialog.message.find("Account 42 - Zo\xc3\xab")!=std::string::npos);
    screenshot("local/steam-add-confirm.bmp");
    key(VK_ESCAPE); assert(!installs && !steam_menu::dialog.revision && haze_menu_active());
    action(0); frame(owner); key('K'); assert(!installs && !steam_menu::dialog.revision);
    action(0); frame(owner); SendMessage(panel,WM_CLOSE,0,0); assert(!installs && !steam_menu::dialog.revision);
    action(0); frame(owner); key(VK_RETURN); assert(!installs && !steam_menu::dialog.revision); // Cancel default
    action(0); frame(owner); foreground=nullptr; frame(owner); assert(!installs && !steam_menu::dialog.revision);
    foreground=panel; frame(owner); action(0); frame(owner); key(VK_RIGHT); key(VK_RETURN);
    assert(installs==1 && installed_account=="42" && steam_menu::dialog.title=="Steam shortcut ready"); key(VK_RETURN);
    action(1); assert(steam_menu::selected.empty()); // Refresh never silently reselects.
    key(VK_ESCAPE); assert(!haze_menu_active() && guest_backs==0 && retail_returns==1);
    frame(owner); frame(owner,CELL_PAD_CTRL_SELECT); frame(owner,CELL_PAD_CTRL_SELECT|CELL_PAD_CTRL_CROSS);
    assert(haze_menu_active() && !haze_quit_menu_active()); frame(owner); // neutral after opening
    frame(owner,CELL_PAD_CTRL_DOWN); frame(owner); frame(owner,CELL_PAD_CTRL_RIGHT); frame(owner);
    assert(steam_menu::selected=="42"); action(0); frame(owner,CELL_PAD_CTRL_CROSS); assert(installs==1);
    frame(owner); frame(owner,CELL_PAD_CTRL_CIRCLE); assert(!steam_menu::dialog.revision && installs==1);
    install_success=false; action(0); frame(owner); click(panel,modal_action_rect(1,2));
    assert(installs==2 && steam_menu::dialog.title=="Could not add to Steam"); key(VK_RETURN);
    // Stale main-menu generation cannot authorize writing.
    action(0); ++offer.generation; frame(owner); key(VK_RIGHT); key(VK_RETURN); assert(installs==2); key(VK_RETURN);
    key(VK_ESCAPE); frame(owner); assert(haze_quit_menu_key(VK_TAB,false)); frame(owner);
    key(VK_RETURN); frame(owner); // Quit action goes to the normal Cancel-first confirmation.
    assert(!haze_menu_active() && haze_quit_state().phase==HazeQuitPhase::Confirm && quit_ui::selected==0);
    SendMessage(quit_ui::dialog,WM_KEYDOWN,VK_RETURN,0); frame(owner); assert(!haze_quit_menu_active());
    // Missing launcher and missing Steam are actionable, with zero installer calls.
    std::filesystem::remove(launcher); assert(haze_quit_menu_key(VK_TAB,false)); frame(owner);
    key(VK_DOWN); key(VK_RIGHT); action(0); assert(steam_menu::dialog.title=="Launcher required" && installs==2); key(VK_RETURN);
    steam_available=false; action(1); action(0); assert(notice.find("Steam was not found")!=std::string::npos && installs==2); key(VK_ESCAPE);
    offer.gameplay=true; frame(owner); assert(haze_quit_menu_key(VK_TAB,false)); frame(owner);
    assert(haze_quit_menu_active() && !haze_menu_active()); SendMessage(quit_ui::dialog,WM_KEYDOWN,VK_ESCAPE,0);
    haze_quit_menu_shutdown(); haze_menu_shutdown(); DestroyWindow(owner); std::filesystem::remove_all(fixture);
    std::puts("Steam menu: explicit account selection, keyboard/mouse/controller access, held-input gate, Cancel/default/Back/focus handling, missing dependencies, failures, stale menu and main/pause Quit routing passed.");
}
