#include "haze_quit_menu.h"
#include "haze_quit.h"
#include "haze_input.h"
#include "haze_menu.h"
#include "haze_prompts.h"
#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include "haze_input_key.h"

namespace quit_ui {
HWND owner=nullptr,button=nullptr,dialog=nullptr;
HFONT font=nullptr;
int unit=24,width=720,height=240,selected=0;
unsigned previous=0;
bool neutral=false;
uint64_t generation=0;
HazeQuitPhase phase=HazeQuitPhase::Idle;
std::string action_binding="Tab";
HazeInputFamily prompt_family=HazeInputFamily::Keyboard;
ULONGLONG next_prompt_check=0;
void refresh_binding() {
    const auto now=GetTickCount64(); const auto family=haze_input_family();
    if (now<next_prompt_check && family==prompt_family) return;
    next_prompt_check=now+500; prompt_family=family;
    const bool keyboard=family==HazeInputFamily::Keyboard;
    char value[128]{};
    GetPrivateProfileStringA(keyboard ? "Bindings" : "ControllerBindings","Select",
                           keyboard ? "Tab" : "back",value,sizeof(value),haze_input_config_path());
    action_binding=value;
    if (!*value || !_stricmp(value,"none")) { action_binding="Tab"; return; }
    if (keyboard) {
        const auto key=haze_key_code(value);
        action_binding=key>0 ? haze_key_name(key) : "Tab";
        if (action_binding!="Tab") action_binding+=" / Tab";
        return;
    }
    const char* inputs[]={"south","east","west","north","start","back","leftshoulder","rightshoulder","leftstick","rightstick"};
    const char* xbox[]={"A","B","X","Y","Menu","View","LB","RB","LS","RS"};
    const char* ps[]={"Cross","Circle","Square","Triangle","Options","Share","L1","R1","L3","R3"};
    const char* sw[]={"B","A","Y","X","+","-","L","R","LS","RS"};
    for (int i=0;i<10;++i) if (!_stricmp(value,inputs[i]))
        action_binding=family==HazeInputFamily::Xbox ? xbox[i] : family==HazeInputFamily::Switch ? sw[i] : ps[i];
}
std::string action_label() { return action_binding+" - Quit to Desktop"; }
RECT choice_rect(int index) { const int span=(width-unit*3)/2; return {unit+index*(span+unit),height-unit*3,unit+index*(span+unit)+span,height-unit}; }
void fill(HDC dc,RECT rect,COLORREF color) { auto brush=CreateSolidBrush(color); FillRect(dc,&rect,brush); DeleteObject(brush); }
void text(HDC dc,const std::string& value,RECT rect,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE) {
    DrawTextA(dc,value.c_str(),-1,&rect,flags|DT_NOPREFIX);
}
bool focused() { const auto active=GetForegroundWindow(); return active && (active==owner || active==button || active==dialog || haze_menu_has_focus()); }
void cancel() {
    const auto snapshot=haze_quit_state();
    if (!haze_quit_cancel(snapshot.generation)) return;
    ShowWindow(dialog,SW_HIDE); haze_input_menu_retail();
    if (focused()) { SetForegroundWindow(owner); SetFocus(owner); }
    neutral=false;
}
void choose() {
    const auto snapshot=haze_quit_state();
    if (snapshot.phase==HazeQuitPhase::Confirm && selected==1) haze_quit_confirm(snapshot.generation);
    else cancel();
}
bool open() {
    const auto offer=haze_quit_offer();
    if (!owner || !focused() || haze_menu_active() || !offer.available) return false;
    // A mouse activation may foreground the footer itself. Hand focus back
    // before opening the confirmation or a dependent native settings page.
    if (GetForegroundWindow()==button) { SetForegroundWindow(owner); SetFocus(owner); }
    if (!haze_quit_request(offer.gameplay,offer.generation)) return false;
    selected=0; neutral=false; previous=0;
    return true;
}
void paint(HWND window,HDC dc) {
    RECT bounds{}; GetClientRect(window,&bounds);
    fill(dc,bounds,RGB(25,28,25)); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(245,225,0));
    auto old=SelectObject(dc,font);
    if (window==button) {
        haze_prompts_draw_icon(dc,7,unit/2,unit/4,unit);
        text(dc,action_label(),{unit*2,0,bounds.right-unit/2,bounds.bottom},DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    } else {
        const auto snapshot=haze_quit_state();
        text(dc,"QUIT TO DESKTOP",{unit,unit/2,width-unit,unit*2});
        SetTextColor(dc,RGB(235,235,225));
        text(dc,snapshot.message,{unit,unit*2,width-unit,height-unit*4},DT_LEFT|DT_WORDBREAK);
        const int count=snapshot.phase==HazeQuitPhase::Confirm ? 2 : 1;
        for (int i=0;i<count;++i) {
            auto rect=choice_rect(i); fill(dc,rect,i==selected ? RGB(102,96,24) : RGB(48,53,45));
            haze_prompts_draw_icon(dc,i ? 12 : 14,rect.left+unit/3,rect.top+unit/2,unit);
            rect.left+=unit;
            text(dc,i ? "Quit to Desktop" : snapshot.phase==HazeQuitPhase::Failed ? "Return to game" : "Cancel",rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
    }
    SelectObject(dc,old);
}
LRESULT CALLBACK procedure(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        if (window==button) return MA_NOACTIVATE;
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; auto dc=BeginPaint(window,&ps); paint(window,dc); EndPaint(window,&ps); return 0; }
    case WM_PRINTCLIENT: paint(window,(HDC)wp); return 0;
    case WM_CLOSE: if (window==dialog) cancel(); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (window!=dialog || !haze_quit_menu_active() || (lp&(1ll<<30))) return 0;
        {
            const unsigned input=haze_input_menu_button(int(wp));
            if (wp==VK_ESCAPE || (input&CELL_PAD_CTRL_CIRCLE)) cancel();
            else if (haze_quit_state().phase==HazeQuitPhase::Confirm &&
                (wp==VK_LEFT || wp==VK_RIGHT || wp==VK_UP || wp==VK_DOWN || wp==VK_TAB ||
                (input&(CELL_PAD_CTRL_LEFT|CELL_PAD_CTRL_RIGHT|CELL_PAD_CTRL_UP|CELL_PAD_CTRL_DOWN)))) selected=1-selected;
            else if (wp==VK_RETURN || (input&CELL_PAD_CTRL_CROSS)) choose();
            InvalidateRect(window,nullptr,FALSE); return 0;
        }
    case WM_LBUTTONDOWN:
        haze_input_menu_button(0);
        if (window==button) { open(); return 0; }
        if (!haze_quit_menu_active()) return 0;
        {
            POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            const int count=haze_quit_state().phase==HazeQuitPhase::Confirm ? 2 : 1;
            for (int i=0;i<count;++i) { auto rect=choice_rect(i); if (PtInRect(&rect,point)) { selected=i; choose(); break; } }
            return 0;
        }
    }
    return DefWindowProc(window,msg,wp,lp);
}
}
bool haze_quit_menu_active() { return haze_quit_state().phase!=HazeQuitPhase::Idle; }
bool haze_quit_menu_has_focus() { return quit_ui::dialog && GetForegroundWindow()==quit_ui::dialog; }
bool haze_quit_menu_key(int key,bool repeated) {
    const auto offer=haze_quit_offer();
    if (!offer.available || haze_menu_active()) return false;
    if (key!=VK_TAB && !(haze_input_menu_button(key)&CELL_PAD_CTRL_SELECT)) return false;
    if (!repeated) quit_ui::open();
    return true;
}
void haze_quit_menu_update(void* window,const HazeGamepadState& pad) {
    using namespace quit_ui;
    owner=(HWND)window; if (!owner) return;
    const auto snapshot=haze_quit_state(); const auto offer=haze_quit_offer();
    if (!button && (offer.available || snapshot.phase!=HazeQuitPhase::Idle)) {
        WNDCLASSA cls{}; cls.lpszClassName="HazeQuit"; cls.lpfnWndProc=procedure;
        cls.hInstance=GetModuleHandle(nullptr); cls.hCursor=LoadCursor(nullptr,IDC_ARROW); RegisterClassA(&cls);
        button=CreateWindowExA(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,"Haze Quit action",WS_POPUP,0,0,300,50,owner,nullptr,cls.hInstance,nullptr);
        dialog=CreateWindowExA(WS_EX_TOOLWINDOW,cls.lpszClassName,"Quit to Desktop",WS_POPUP,0,0,width,height,owner,nullptr,cls.hInstance,nullptr);
    }
    if (!button || !dialog) return;
    RECT bounds{}; GetClientRect(owner,&bounds); POINT origin{}; ClientToScreen(owner,&origin);
    const int scale=std::clamp(int(bounds.bottom)/40,16,40);
    if (!font || scale!=unit) {
        unit=scale; if (font) DeleteObject(font);
        font=CreateFontA(-unit,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
    }
    width=std::min(int(bounds.right)-unit*2,unit*30); height=unit*11;
    refresh_binding();
    HDC dc=GetDC(button); auto old_font=SelectObject(dc,font); SIZE extent{};
    const auto label=action_label(); GetTextExtentPoint32A(dc,label.c_str(),int(label.size()),&extent);
    SelectObject(dc,old_font); ReleaseDC(button,dc);
    const int action_width=std::min(int(bounds.right)-unit*2,int(extent.cx)+unit*3);
    SetWindowPos(button,HWND_TOP,origin.x+bounds.right-action_width-unit,origin.y+bounds.bottom-unit*3,action_width,unit*2,SWP_NOACTIVATE);
    SetWindowPos(dialog,HWND_TOP,origin.x+(bounds.right-width)/2,origin.y+(bounds.bottom-height)/2,width,height,SWP_NOACTIVATE);
    const bool is_focused=focused() && !IsIconic(owner);
    if (snapshot.generation!=generation || snapshot.phase!=phase) {
        generation=snapshot.generation; phase=snapshot.phase; selected=0; neutral=false; previous=0;
        if (snapshot.phase!=HazeQuitPhase::Idle && is_focused) {
            ShowWindow(dialog,SW_SHOW); SetForegroundWindow(dialog); SetFocus(dialog);
        }
    }
    if (!is_focused) {
        if (snapshot.phase==HazeQuitPhase::Confirm) cancel();
        ShowWindow(button,SW_HIDE); ShowWindow(dialog,SW_HIDE); neutral=false; return;
    }
    const bool modal=snapshot.phase!=HazeQuitPhase::Idle;
    ShowWindow(button,!modal && offer.available && !haze_menu_active() ? SW_SHOWNOACTIVATE : SW_HIDE);
    ShowWindow(dialog,modal ? SW_SHOWNOACTIVATE : SW_HIDE);
    InvalidateRect(button,nullptr,FALSE); InvalidateRect(dialog,nullptr,FALSE);
    unsigned buttons=pad.input.buttons;
    if (modal) {
        const auto& navigation=pad.menu_input_valid ? pad.menu_input : pad.input;
        buttons=navigation.buttons;
        if (navigation.lx<64 || navigation.ly<64) buttons|=CELL_PAD_CTRL_LEFT;
        if (navigation.lx>192 || navigation.ly>192) buttons|=CELL_PAD_CTRL_RIGHT;
    }
    if (!neutral) { if (!buttons) neutral=true; previous=buttons; return; }
    const unsigned pressed=buttons&~previous; previous=buttons;
    if (!modal) { if ((pressed&CELL_PAD_CTRL_SELECT) && offer.available && !haze_menu_active()) open(); return; }
    if (pressed&CELL_PAD_CTRL_CIRCLE) { cancel(); return; }
    if (snapshot.phase==HazeQuitPhase::Confirm && (pressed&(CELL_PAD_CTRL_LEFT|CELL_PAD_CTRL_RIGHT|CELL_PAD_CTRL_UP|CELL_PAD_CTRL_DOWN))) selected=1-selected;
    if (pressed&CELL_PAD_CTRL_CROSS) choose();
}
void haze_quit_menu_shutdown() {
    using namespace quit_ui;
    if (dialog) DestroyWindow(dialog); if (button) DestroyWindow(button);
    dialog=button=owner=nullptr; if (font) DeleteObject(font); font=nullptr;
}
#else
void haze_quit_menu_update(void*,const HazeGamepadState&) {}
bool haze_quit_menu_key(int,bool) { return false; }
bool haze_quit_menu_active() { return false; }
bool haze_quit_menu_has_focus() { return false; }
void haze_quit_menu_shutdown() {}
#endif
