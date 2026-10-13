// Exercise the real Win32 page and Video provider with controlled focus and
// backend results. Native input/device and GPU acceptance are separate checks.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <cstdio>
static HWND foreground;
static unsigned returns,activations;
static HWND test_foreground() { return foreground; }
static BOOL test_activate(HWND hwnd) { ++activations; foreground=hwnd; return TRUE; }
static HWND test_focus(HWND) { return nullptr; }
static BOOL test_show(HWND,int) { return TRUE; }
#define GetForegroundWindow test_foreground
#define SetForegroundWindow test_activate
#define SetFocus test_focus
#define ShowWindow test_show
#include "../port/haze_menu.cpp"
#include "../port/haze_video_menu.cpp"
void haze_input_menu_return() { ++returns; }
unsigned haze_input_menu_button(int vk) {
    return vk=='J' ? CELL_PAD_CTRL_CROSS : vk=='K' ? CELL_PAD_CTRL_CIRCLE : vk=='L' ? CELL_PAD_CTRL_SQUARE : 0;
}
bool haze_prompts_draw_icon(void*,int,int,int,int) { return false; }
static HazeGraphicsState graphics;
static unsigned applies;
static bool fail_apply;
HazeGraphicsState haze_graphics_state() { return graphics; }
bool haze_graphics_apply(const haze_video::Values& values,bool persist,std::string& message) {
    assert(persist); ++applies;
    if (fail_apply) { message="Could not save settings."; return false; }
    graphics.configured=graphics.effective=values; graphics.notice="Applied"; return true;
}
static void key(int vk) { SendMessage(panel,WM_KEYDOWN,vk,0); SendMessage(panel,WM_KEYUP,vk,0); }
static void click(int x,int y) {
    SendMessage(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    SendMessage(panel,WM_LBUTTONUP,0,MAKELPARAM(x,y));
}
static void action(int index) {
    const auto rect=action_rect(index); click((rect.left+rect.right)/2,(rect.top+rect.bottom)/2);
}
static void open(HWND owner) { assert(haze_menu_request(HazeMenuPage::Video)); haze_menu_update(owner,{}); }
static void render(const char* path) {
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    assert(bitmap && pixels); auto old=SelectObject(dc,bitmap);
    SendMessage(panel,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT); GdiFlush();
    assert(GetPixel(dc,0,0)==RGB(242,221,0));
    assert(GetPixel(dc,0,10)==RGB(18,20,20));
    if (path) {
        BITMAPFILEHEADER header{}; header.bfType=0x4d42;
        header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
        header.bfSize=header.bfOffBits+width*height*4;
        FILE* file=std::fopen(path,"wb"); assert(file);
        std::fwrite(&header,sizeof(header),1,file);
        std::fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,file);
        std::fwrite(pixels,width*height*4,1,file); std::fclose(file);
    }
    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr,screen);
}
int main(int argc,char** argv) {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT);
    HWND owner=CreateWindowA("STATIC","Menu test owner",WS_POPUP,0,0,1920,1080,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    assert(owner);
    graphics.configured=graphics.effective=haze_video::defaults();
    haze_video_menu_register();
    assert(!haze_menu_request(HazeMenuPage::Audio));
    assert(!haze_menu_request(HazeMenuPage(99)));
    open(owner); assert(haze_menu_active() && activations==0); // Background event never steals focus.
    render(argc>1 ? argv[1] : nullptr);
    key('J'); key(VK_DOWN); key(VK_DOWN); key('J'); // Custom -> Low -> Medium, using rebound Confirm.
    assert(edited.at("anisotropic_filtering")=="4" && graphics.configured.at("anisotropic_filtering")=="1");
    action(0); assert(applies==1 && graphics.configured.at("anisotropic_filtering")=="4");
    key('K'); assert(!haze_menu_active() && returns==1);
    open(owner); key(VK_RIGHT); key(VK_ESCAPE); // Stage High, cancel, reopen: still Medium.
    open(owner); assert(haze_video::preset_name(edited)=="Medium");
    key(VK_RIGHT); fail_apply=true; action(0);
    assert(notice_error && notice=="Could not save settings." && graphics.configured.at("anisotropic_filtering")=="4");
    key('L'); assert(!notice_error && edited==haze_video::defaults());
    fail_apply=false; action(0); assert(graphics.configured==haze_video::defaults());
    key(VK_ESCAPE); open(owner);
    key('J'); key(VK_ESCAPE); assert(haze_menu_active()); // Escape dismisses an open dropdown first.
    key(VK_ESCAPE); assert(!haze_menu_active());
    open(owner);
    // A long dropdown scrolls its selected option into view and selects it.
    key(VK_DOWN); key(VK_DOWN); key('J'); key(VK_UP); key('J');
    assert(edited.at("monitor")=="16");
    key(VK_ESCAPE); open(owner);
    for (int i=0;i<12;++i) key(VK_DOWN); // Brightness slider, scrolled into view.
    auto rect=row_rect(12); click(width-unit*5,(rect.top+rect.bottom)/2);
    assert(edited.at("exposure")=="2");
    // Closing during a slider drag releases capture before returning to the game.
    SendMessage(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(width/2,(rect.top+rect.bottom)/2));
    assert(GetCapture()==panel); key(VK_ESCAPE); assert(GetCapture()!=panel);
    // Controller directions/confirm must see neutral after opening or focus return.
    foreground=owner; assert(haze_menu_request(HazeMenuPage::Video)); HazeGamepadState pad;
    pad.input.buttons=CELL_PAD_CTRL_DOWN; haze_menu_update(owner,pad); assert(selected==0);
    pad.input.buttons=0; haze_menu_update(owner,pad);
    pad.input.buttons=CELL_PAD_CTRL_DOWN; haze_menu_update(owner,pad); assert(selected==1);
    foreground=nullptr; haze_menu_update(owner,pad); foreground=owner;
    haze_menu_update(owner,pad); assert(selected==1);
    pad.input.buttons=0; haze_menu_update(owner,pad);
    pad.input.buttons=CELL_PAD_CTRL_DOWN; haze_menu_update(owner,pad); assert(selected==2);
    key(VK_ESCAPE);
    const int sizes[][2]={{960,540},{1280,720},{1920,1080},{2560,1440},{3440,1440},{3840,2160}};
    for (const auto& size:sizes) {
        SetWindowPos(owner,nullptr,0,0,size[0],size[1],SWP_NOACTIVATE|SWP_NOZORDER);
        open(owner); render(nullptr); assert(visible_rows>0);
        for (int i=0;i<13;++i) key(VK_DOWN);
        assert(selected==13); render(nullptr); key(VK_ESCAPE);
    }
    haze_menu_shutdown(); DestroyWindow(owner);
    std::puts("Native page navigation, staging, failure recovery, rendering and input tests passed.");
}
