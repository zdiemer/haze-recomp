#include "haze_menu.h"
#include "haze_input.h"
#include "haze_prompts.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

namespace {
std::array<HazeMenuProvider,4> providers;
std::array<std::atomic<bool>,4> available{};
std::atomic<int> requested{0};
std::atomic<bool> active{false};
HWND panel,owner_window;
HFONT font,heading;
HazeMenuPage current=HazeMenuPage::None;
std::vector<HazeMenuRow> items;
std::string notice;
int selected=0,scroll=0,dropdown=-1,drop_scroll=0,drop_selected=0,dragging=-1;
int width=0,height=0,unit=24,row_height=48,top=100,visible_rows=8;
unsigned previous_buttons=0;
ULONGLONG repeat_at=0;
bool pad_neutral=false,notice_error=false;
bool applying=false;
void clear_notice() { notice.clear(); notice_error=false; }
HazeMenuProvider& provider() { return providers[size_t(current)]; }
RECT row_rect(int index) { return {unit*2,top+(index-scroll)*row_height,width-unit*2,top+(index-scroll+1)*row_height}; }
RECT action_rect(int action) { const int span=(width-unit*4)/3; return {unit*2+span*action,height-unit*5,unit*2+span*(action+1)-unit/2,height-unit*3}; }
void refresh() {
    items=provider().rows(); selected=std::clamp(selected,0,int(items.size())+2);
    if (selected<int(items.size())) {
        if (selected<scroll) scroll=selected;
        if (selected>=scroll+visible_rows) scroll=selected-visible_rows+1;
    }
    scroll=std::clamp(scroll,0,std::max(0,int(items.size())-visible_rows));
    InvalidateRect(panel,nullptr,FALSE);
}
void close_page() {
    if (!panel || applying) return;
    if (GetCapture()==panel) ReleaseCapture();
    ShowWindow(panel,SW_HIDE); dropdown=dragging=-1;
    current=HazeMenuPage::None; active=false;
    haze_input_menu_return();
    SetForegroundWindow(owner_window); SetFocus(owner_window);
}
void activate() {
    if (applying) return;
    if (dropdown>=0) {
        const auto row=items[dropdown]; provider().change(row.key,row.choices[drop_selected].value);
        dropdown=-1; clear_notice(); refresh(); return;
    }
    if (selected<int(items.size())) {
        if (!items[selected].slider) {
            dropdown=selected; drop_scroll=drop_selected=0;
            const auto& choices=items[selected].choices;
            for (size_t i=0;i<choices.size();++i) if (choices[i].value==items[selected].value)
                { drop_selected=int(i); drop_scroll=std::max(0,int(i)-3); }
        }
    } else switch (selected-int(items.size())) {
        case 0: {
            const auto result=provider().apply(notice);
            applying=result==HazeMenuApply::Pending;
            notice_error=result==HazeMenuApply::Failed;
            break;
        }
        case 1: provider().defaults(); notice="Defaults selected. Apply to save."; notice_error=false; break;
        case 2: close_page(); return;
    }
    refresh();
}
void change(int direction) {
    if (selected>=int(items.size())) return;
    const auto row=items[selected];
    if (row.slider) {
        const double value=std::clamp(std::strtod(row.value.c_str(),nullptr)+direction*row.step,row.minimum,row.maximum);
        char text[40]; snprintf(text,sizeof text,"%.3g",value);
        provider().change(row.key,text);
    } else if (!row.choices.empty()) {
        int index=0;
        for (size_t i=0;i<row.choices.size();++i) if (row.choices[i].value==row.value) index=int(i);
        index=(index+direction+int(row.choices.size()))%int(row.choices.size());
        provider().change(row.key,row.choices[index].value);
    }
    clear_notice(); dropdown=-1; refresh();
}
void move(int direction) {
    if (dropdown>=0) {
        const int count=int(items[dropdown].choices.size());
        drop_selected=(drop_selected+direction+count)%count;
        if (drop_selected<drop_scroll) drop_scroll=drop_selected;
        if (drop_selected>=drop_scroll+8) drop_scroll=drop_selected-7;
        refresh(); return;
    }
    selected=(selected+direction+int(items.size())+3)%(int(items.size())+3); refresh();
}
void slider_at(int x) {
    if (dragging<0 || dragging>=int(items.size())) return;
    const auto row=items[dragging];
    const double fraction=std::clamp(double(x-width/2)/std::max(1,width/2-unit*5),0.,1.);
    double value=row.minimum+std::round(fraction*(row.maximum-row.minimum)/row.step)*row.step;
    char text[40]; snprintf(text,sizeof text,"%.3g",std::clamp(value,row.minimum,row.maximum));
    provider().change(row.key,text); clear_notice(); refresh();
}
void fill(HDC dc,RECT rect,COLORREF color) { HBRUSH brush=CreateSolidBrush(color); FillRect(dc,&rect,brush); DeleteObject(brush); }
void label(HDC dc,const std::string& text,RECT rect,COLORREF color,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE) {
    SetTextColor(dc,color); DrawTextA(dc,text.c_str(),-1,&rect,flags|DT_NOPREFIX|DT_END_ELLIPSIS);
}
RECT dropdown_rect() {
    const int count=std::min(8,int(items[dropdown].choices.size())-drop_scroll);
    int y=row_rect(dropdown).bottom;
    y=std::min(y,height-unit*6-count*row_height);
    return {width/2,std::max(top,y),width-unit*2,std::max(top,y)+count*row_height};
}
void paint(HDC target) {
    HDC dc=CreateCompatibleDC(target); HBITMAP bitmap=CreateCompatibleBitmap(target,width,height);
    auto old_bitmap=SelectObject(dc,bitmap); auto old_font=SelectObject(dc,font);
    SetBkMode(dc,TRANSPARENT);
    fill(dc,{0,0,width,height},RGB(18,20,20)); fill(dc,{0,0,width,4},RGB(242,221,0));
    SelectObject(dc,heading); label(dc,provider().title,{unit*2,unit,width-unit*2,top-unit/2},RGB(245,225,0)); SelectObject(dc,font);
    if (int(items.size())>visible_rows)
        label(dc,scroll+visible_rows<int(items.size()) ? "Scroll for more settings" : "More settings above",
            {width/2,unit,width-unit*2,top-unit/2},RGB(195,200,190),DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    for (int i=scroll;i<std::min(int(items.size()),scroll+visible_rows);++i) {
        auto rect=row_rect(i); const auto& row=items[i];
        if (selected==i) fill(dc,rect,RGB(55,57,40));
        RECT left=rect; left.left+=unit/2; left.right=width/2-unit;
        label(dc,row.label,left,RGB(235,235,225));
        RECT right=rect; right.left=width/2; right.right-=unit;
        std::string value=row.value;
        for (const auto& choice:row.choices) if (choice.value==row.value) value=choice.label;
        if (row.slider) {
            const int y=(rect.top+rect.bottom)/2;
            const int end=width-unit*5;
            fill(dc,{width/2,y-2,end,y+2},RGB(95,97,88));
            const double fraction=std::clamp((std::strtod(row.value.c_str(),nullptr)-row.minimum)/(row.maximum-row.minimum),0.,1.);
            const int x=width/2+int((end-width/2)*fraction);
            fill(dc,{x-4,y-unit/2,x+4,y+unit/2},RGB(242,221,0)); right.left=end+unit/2;
        } else value+="  v";
        label(dc,value,right,RGB(242,221,0),DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    }
    const std::string hint=!notice.empty() ? notice : selected<int(items.size()) ? items[selected].hint :
        "Apply saves changes. Back discards edits made since the last Apply.";
    label(dc,hint,{unit*2,height-unit*8,width-unit*2,height-unit*5-unit/3},
        notice_error ? RGB(255,155,135) : RGB(195,200,190),DT_LEFT|DT_WORDBREAK);
    const char* actions[]={"Apply","Defaults","Back"};
    for (int i=0;i<3;++i) {
        RECT rect=action_rect(i); fill(dc,rect,!applying && selected==int(items.size())+i ? RGB(105,100,20) : RGB(45,48,43));
        label(dc,applying && i==0 ? "Saving..." : actions[i],rect,applying ? RGB(140,145,130) : RGB(245,240,210),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    const int footer=height-unit*3,icon_size=unit*2;
    const int slots[]={12,14,13}; const char* hints[]={"Select","Back","Defaults"};
    for (int i=0;i<3;++i) {
        const int x=unit*2+i*(width-unit*4)/3;
        haze_prompts_draw_icon(dc,slots[i],x,footer,icon_size);
        label(dc,hints[i],{x+icon_size,footer,x+(width-unit*4)/3,footer+icon_size},RGB(190,195,180));
    }
    if (dropdown>=0) {
        auto rect=dropdown_rect(); fill(dc,rect,RGB(64,68,53));
        for (int i=drop_scroll;i<std::min(int(items[dropdown].choices.size()),drop_scroll+8);++i) {
            RECT item=rect; item.top+=(i-drop_scroll)*row_height; item.bottom=item.top+row_height;
            if (i==drop_selected) fill(dc,item,RGB(100,100,30));
            const auto& choice=items[dropdown].choices[i];
            label(dc,choice.label,item,choice.value==items[dropdown].value ? RGB(255,240,0) : RGB(240,240,230));
        }
    }
    BitBlt(target,0,0,width,height,dc,0,0,SRCCOPY);
    SelectObject(dc,old_font); SelectObject(dc,old_bitmap); DeleteObject(bitmap); DeleteDC(dc);
}
LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); if (current!=HazeMenuPage::None) paint(dc); EndPaint(hwnd,&ps); return 0; }
    case WM_PRINTCLIENT: if (current!=HazeMenuPage::None) paint((HDC)wp); return 0;
    case WM_CLOSE: close_page(); return 0;
    case WM_KEYDOWN: {
        if (applying) return 0;
        const unsigned button=haze_input_menu_button(int(wp));
        if (wp==VK_ESCAPE || (button&CELL_PAD_CTRL_CIRCLE)) { if (dropdown>=0) { dropdown=-1; refresh(); } else close_page(); }
        else if (wp==VK_UP || (button&CELL_PAD_CTRL_UP)) move(-1);
        else if (wp==VK_DOWN || wp==VK_TAB || (button&CELL_PAD_CTRL_DOWN)) move(1);
        else if (wp==VK_LEFT || (button&CELL_PAD_CTRL_LEFT)) change(-1);
        else if (wp==VK_RIGHT || (button&CELL_PAD_CTRL_RIGHT)) change(1);
        else if ((wp==VK_RETURN || (button&CELL_PAD_CTRL_CROSS)) && !(lp&(1ll<<30))) activate();
        else if ((button&CELL_PAD_CTRL_SQUARE) && !(lp&(1ll<<30))) { provider().defaults(); dropdown=-1; notice="Defaults selected. Apply to save."; notice_error=false; refresh(); }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (applying) return 0;
        if (dropdown>=0) drop_scroll=std::clamp(drop_scroll+(GET_WHEEL_DELTA_WPARAM(wp)>0 ? -1 : 1),0,std::max(0,int(items[dropdown].choices.size())-8));
        else scroll=std::clamp(scroll+(GET_WHEEL_DELTA_WPARAM(wp)>0 ? -2 : 2),0,std::max(0,int(items.size())-visible_rows));
        InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_LBUTTONDOWN: {
        if (applying) return 0;
        haze_input_menu_button(0);
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (dropdown>=0) {
            const auto rect=dropdown_rect();
            if (PtInRect(&rect,p)) {
                const int index=drop_scroll+(p.y-rect.top)/row_height;
                provider().change(items[dropdown].key,items[dropdown].choices[index].value);
                clear_notice();
            }
            dropdown=-1; refresh(); return 0;
        }
        for (int i=0;i<3;++i) { auto rect=action_rect(i); if (PtInRect(&rect,p)) { selected=int(items.size())+i; activate(); return 0; } }
        for (int i=scroll;i<std::min(int(items.size()),scroll+visible_rows);++i) {
            auto rect=row_rect(i); if (!PtInRect(&rect,p)) continue;
            selected=i;
            if (items[i].slider) { dragging=i; SetCapture(hwnd); slider_at(p.x); }
            else activate();
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE: if (!applying && dragging>=0) slider_at(GET_X_LPARAM(lp)); return 0;
    case WM_LBUTTONUP: dragging=-1; if (GetCapture()==hwnd) ReleaseCapture(); return 0;
    case WM_KILLFOCUS: case WM_CANCELMODE:
        dragging=-1; dropdown=-1; pad_neutral=false; previous_buttons=0;
        if (GetCapture()==hwnd) ReleaseCapture(); return 0;
    }
    return DefWindowProc(hwnd,message,wp,lp);
}
void position() {
    RECT rect; GetClientRect(owner_window,&rect); POINT origin{}; ClientToScreen(owner_window,&origin);
    const int w=std::min(int(rect.right)*9/10,1800),h=int(rect.bottom)*9/10;
    if (width!=w || height!=h) {
        width=w; height=h; unit=std::clamp(h/32,14,38); row_height=unit*2; top=unit*4;
        visible_rows=std::max(1,(height-unit*12)/row_height);
        if (font) DeleteObject(font); if (heading) DeleteObject(heading);
        font=CreateFontA(-unit,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
        heading=CreateFontA(-unit*2,0,0,0,FW_LIGHT,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
        refresh();
    }
    SetWindowPos(panel,HWND_TOP,origin.x+(rect.right-w)/2,origin.y+(rect.bottom-h)/2,w,h,SWP_NOACTIVATE);
}
}
void haze_menu_register(HazeMenuPage page,HazeMenuProvider value) {
    if (page<=HazeMenuPage::None || size_t(page)>=providers.size()) return;
    providers[size_t(page)]=std::move(value); available[size_t(page)]=true;
}
bool haze_menu_request(HazeMenuPage page) {
    if (page<=HazeMenuPage::None || size_t(page)>=providers.size() || !available[size_t(page)]) return false;
    active=true; requested=int(page); return true;
}
bool haze_menu_active() { return active.load(); }
bool haze_menu_has_focus() { return panel && active && GetForegroundWindow()==panel; }
void haze_menu_update(void* owner,const HazeGamepadState& pad) {
    owner_window=(HWND)owner;
    const int next=applying ? 0 : requested.exchange(0);
    if (next) {
        current=HazeMenuPage(next); provider().begin(); selected=scroll=0; dropdown=dragging=-1;
        clear_notice(); applying=false; pad_neutral=false; previous_buttons=0;
        if (!panel) {
            WNDCLASSA cls{}; cls.lpfnWndProc=procedure; cls.hInstance=GetModuleHandle(nullptr);
            cls.lpszClassName="HazePCSettings"; cls.hCursor=LoadCursor(nullptr,IDC_ARROW); RegisterClassA(&cls);
            panel=CreateWindowExA(WS_EX_TOOLWINDOW,cls.lpszClassName,"Haze Settings",WS_POPUP,0,0,800,600,owner_window,nullptr,cls.hInstance,nullptr);
        }
        if (!panel) { active=false; current=HazeMenuPage::None; return; }
        refresh(); position();
        // A delayed guest event must not activate Haze over another app.
        const HWND foreground=GetForegroundWindow();
        if (foreground==owner_window || foreground==panel) {
            ShowWindow(panel,SW_SHOW); SetForegroundWindow(panel); SetFocus(panel);
        }
        std::fprintf(stderr,"[menu] Opened PC %s page\n",provider().title.c_str());
    }
    if (!active || !panel || current==HazeMenuPage::None) return;
    if (applying) {
        // Saving continues across focus loss. Keep the exact staged request
        // locked until its owning thread reports a terminal result.
        const auto result=provider().poll ? provider().poll(notice) : HazeMenuApply::Failed;
        if (!provider().poll) notice="The settings provider cannot report save completion.";
        applying=result==HazeMenuApply::Pending;
        notice_error=result==HazeMenuApply::Failed;
        if (!applying) { pad_neutral=false; previous_buttons=0; refresh(); }
    }
    position();
    const HWND foreground=GetForegroundWindow();
    if (IsIconic(owner_window) || (foreground!=owner_window && foreground!=panel)) {
        ShowWindow(panel,SW_HIDE); pad_neutral=false; return;
    }
    ShowWindow(panel,SW_SHOWNOACTIVATE);
    if (foreground==owner_window) SetFocus(panel);
    if (applying) { pad_neutral=false; return; }
    unsigned buttons=pad.input.buttons;
    if (pad.input.ly<64) buttons|=CELL_PAD_CTRL_UP; if (pad.input.ly>192) buttons|=CELL_PAD_CTRL_DOWN;
    if (pad.input.lx<64) buttons|=CELL_PAD_CTRL_LEFT; if (pad.input.lx>192) buttons|=CELL_PAD_CTRL_RIGHT;
    if (!pad_neutral) { if (!buttons) pad_neutral=true; previous_buttons=buttons; return; }
    const auto now=GetTickCount64(); unsigned presses=buttons&~previous_buttons;
    if (presses) repeat_at=now+350;
    else if (buttons && now>=repeat_at) { presses=buttons&(CELL_PAD_CTRL_UP|CELL_PAD_CTRL_DOWN|CELL_PAD_CTRL_LEFT|CELL_PAD_CTRL_RIGHT); repeat_at=now+100; }
    previous_buttons=buttons;
    if (presses&CELL_PAD_CTRL_CIRCLE) {
        if (dropdown>=0) { dropdown=-1; refresh(); } else close_page(); return;
    }
    if (presses&CELL_PAD_CTRL_SQUARE) { provider().defaults(); dropdown=-1; notice="Defaults selected. Apply to save."; notice_error=false; refresh(); }
    if (presses&CELL_PAD_CTRL_UP) move(-1); if (presses&CELL_PAD_CTRL_DOWN) move(1);
    if (presses&CELL_PAD_CTRL_LEFT) change(-1); if (presses&CELL_PAD_CTRL_RIGHT) change(1);
    if (presses&CELL_PAD_CTRL_CROSS) activate();
}
void haze_menu_shutdown() {
    if (panel) DestroyWindow(panel); panel=nullptr;
    if (font) DeleteObject(font); if (heading) DeleteObject(heading); font=heading=nullptr;
    active=false; requested=0; current=HazeMenuPage::None; applying=false; width=height=0;
}
#else
void haze_menu_register(HazeMenuPage,HazeMenuProvider) {}
bool haze_menu_request(HazeMenuPage) { return false; }
bool haze_menu_active() { return false; }
bool haze_menu_has_focus() { return false; }
void haze_menu_update(void*,const HazeGamepadState&) {}
void haze_menu_shutdown() {}
#endif
