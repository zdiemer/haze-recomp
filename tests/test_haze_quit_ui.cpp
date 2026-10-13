#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <crtdbg.h>
#include <fstream>
static HWND foreground;
static HWND test_foreground() { return foreground; }
static BOOL test_activate(HWND window) { foreground=window; return TRUE; }
static HWND test_focus(HWND) { return nullptr; }
static BOOL test_show(HWND,int) { return TRUE; }
#define GetForegroundWindow test_foreground
#define SetForegroundWindow test_activate
#define SetFocus test_focus
#define ShowWindow test_show
#include "../port/haze_quit_menu.cpp"
#include "../port/haze_quit.cpp"
#include "../port/haze_save_status.cpp"
static HazeQuitOffer current_offer{1,true,false};
static bool settings=false;
static unsigned returned;
static HazeInputFamily family=HazeInputFamily::Keyboard;
static std::string input_path;
const char* haze_input_config_path() { return input_path.c_str(); }
HazeInputFamily haze_input_family() { return family; }
HazeQuitOffer haze_quit_offer() { return current_offer; }
bool haze_quit_guest_ready() { return true; }
void haze_quit_resume_guest() {}
bool haze_menu_active() { return settings; }
bool haze_menu_has_focus() { return false; }
void haze_input_menu_retail() { ++returned; }
unsigned haze_input_menu_button(int key) { return key==VK_TAB ? CELL_PAD_CTRL_SELECT : key==VK_SPACE ? CELL_PAD_CTRL_CROSS : key=='X' ? CELL_PAD_CTRL_CIRCLE : 0; }
bool haze_prompts_draw_icon(void*,int,int,int,int) { return true; }
static void key(int vk) { SendMessage(quit_ui::dialog,WM_KEYDOWN,vk,0); }
static void click(HWND window,int x,int y) { SendMessage(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y)); }
static void frame(HWND owner,unsigned buttons=0) {
    HazeGamepadState pad; pad.input.buttons=(u16)buttons; haze_quit_menu_update(owner,pad);
}
static void screenshot(const char* path) {
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=quit_ui::width; info.bmiHeader.biHeight=-quit_ui::height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* pixels=nullptr; auto bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0); assert(bitmap && pixels);
    auto previous=SelectObject(dc,bitmap); SendMessage(quit_ui::dialog,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT); GdiFlush();
    BITMAPFILEHEADER header{}; header.bfType=0x4d42; header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    const auto bytes=quit_ui::width*quit_ui::height*4; header.bfSize=header.bfOffBits+bytes;
    std::ofstream file(path,std::ios::binary); file.write((char*)&header,sizeof(header)); file.write((char*)&info.bmiHeader,sizeof(info.bmiHeader)); file.write((char*)pixels,bytes); assert(file.good());
    SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr,screen);
}
int main() {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
    HWND owner=CreateWindowA("STATIC","Quit test",WS_POPUP,0,0,1920,1080,nullptr,nullptr,GetModuleHandle(nullptr),nullptr); assert(owner);
    foreground=owner; frame(owner);
    assert(quit_ui::button && quit_ui::dialog);
    assert(SendMessage(quit_ui::button,WM_MOUSEACTIVATE,0,0)==MA_NOACTIVATE);
    assert(quit_ui::action_binding=="Tab");
    family=HazeInputFamily::Xbox; frame(owner); assert(quit_ui::action_binding=="View");
    family=HazeInputFamily::PlayStation; frame(owner); assert(quit_ui::action_binding=="Share");
    family=HazeInputFamily::Switch; frame(owner); assert(quit_ui::action_binding=="-");
    family=HazeInputFamily::Keyboard; frame(owner);
    char path[MAX_PATH]{};
    const auto relative="local/quit-prompt-"+std::to_string(GetCurrentProcessId())+".ini";
    assert(GetFullPathNameA(relative.c_str(),MAX_PATH,path,nullptr)); input_path=path;
    assert(WritePrivateProfileStringA("Bindings","Select","J",path)); quit_ui::next_prompt_check=0;
    frame(owner); assert(quit_ui::action_binding=="J / Tab");
    assert(WritePrivateProfileStringA("Bindings","Select","None",path)); quit_ui::next_prompt_check=0;
    frame(owner); assert(quit_ui::action_binding=="Tab");
    assert(WritePrivateProfileStringA("ControllerBindings","Select","north",path));
    family=HazeInputFamily::Xbox; frame(owner); assert(quit_ui::action_binding=="Y");
    assert(DeleteFileA(path)); input_path.clear(); family=HazeInputFamily::Keyboard; frame(owner);
    foreground=nullptr; click(quit_ui::button,5,5); assert(!haze_quit_menu_active()); foreground=owner;
    settings=true; assert(!haze_quit_menu_key(VK_TAB,false)); settings=false;
    current_offer.available=false; assert(!haze_quit_menu_key(VK_TAB,false)); current_offer.available=true;
    assert(haze_quit_menu_key(VK_TAB,false)); frame(owner);
    assert(haze_quit_state().phase==HazeQuitPhase::Confirm && quit_ui::selected==0);
    screenshot("local/quit-main-confirm.bmp");
    key(VK_RETURN); assert(!haze_quit_menu_active() && returned==1); // Cancel is the default.
    frame(owner); foreground=quit_ui::button; click(quit_ui::button,5,5); frame(owner);
    assert(haze_quit_menu_active()); key(VK_RIGHT); key(VK_SPACE);
    assert(haze_quit_state().phase==HazeQuitPhase::Waiting);
    assert(haze_quit_cancel(haze_quit_state().generation)); foreground=owner; frame(owner);
    current_offer={2,true,true}; frame(owner); frame(owner,CELL_PAD_CTRL_SELECT);
    frame(owner,CELL_PAD_CTRL_SELECT|CELL_PAD_CTRL_CROSS); // Held opening input cannot confirm.
    assert(haze_quit_state().phase==HazeQuitPhase::Confirm);
    assert(haze_quit_state().message.find("checkpoint")!=std::string::npos);
    screenshot("local/quit-pause-confirm.bmp");
    frame(owner); frame(owner,CELL_PAD_CTRL_RIGHT); frame(owner);
    auto save=haze_save_begin(0xe71ac8); frame(owner,CELL_PAD_CTRL_CROSS);
    assert(haze_quit_state().phase==HazeQuitPhase::Waiting);
    assert(!haze_quit_poll(2,false,false)); haze_save_finished(save,-1);
    assert(!haze_quit_poll(2,false,false)); frame(owner);
    assert(haze_quit_state().phase==HazeQuitPhase::Failed); key(VK_ESCAPE);
    assert(!haze_quit_menu_active() && returned==2);
    frame(owner); assert(haze_quit_menu_key(VK_TAB,false)); frame(owner);
    foreground=nullptr; frame(owner); assert(!haze_quit_menu_active() && returned==3);
    foreground=owner; frame(owner); click(quit_ui::button,5,5); frame(owner);
    auto rect=quit_ui::choice_rect(1); click(quit_ui::dialog,(rect.left+rect.right)/2,(rect.top+rect.bottom)/2);
    assert(haze_quit_state().phase==HazeQuitPhase::Waiting);
    frame(owner); key('X'); assert(!haze_quit_menu_active() && returned==4);
    haze_quit_menu_shutdown(); DestroyWindow(owner);
    puts("Quit UI: keyboard/mouse/controller access, cancel-first confirmation, held-input gate, failed-save return and focus cancellation passed.");
}
