// Exercise the real Win32 page and Video provider with controlled focus and
// backend results. Native input/device and GPU acceptance are separate checks.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <crtdbg.h>
static HWND foreground;
static unsigned returns,activations,retail_returns;
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
void haze_input_menu_retail() { ++retail_returns; }
unsigned haze_input_menu_button(int vk) {
    return vk=='J' ? CELL_PAD_CTRL_CROSS : vk=='K' ? CELL_PAD_CTRL_CIRCLE : vk=='L' ? CELL_PAD_CTRL_SQUARE : 0;
}
bool haze_prompts_draw_icon(void*,int,int,int,int) { return false; }
bool haze_prompts_draw_binding(void*,HazeInputFamily,const char*,int,int,int) { return false; }
HazeInputFamily haze_input_family() { return HazeInputFamily::Keyboard; }
static HazeGraphicsState graphics;
static unsigned applies;
static bool fail_apply;
static bool timed_display,fail_restore;
static HazeGraphicsState before_preview;
static haze_video::Values staged_preview;
static unsigned next_preview,keeps,reverts,confirmations;
HazeGraphicsState haze_graphics_state() { return graphics; }
bool haze_graphics_apply(const haze_video::Values& values,bool persist,std::string& message) {
    assert(persist); ++applies;
    if (fail_apply) { message="Could not save settings."; return false; }
    graphics.configured=graphics.effective=values; graphics.notice="Applied"; return true;
}
bool haze_graphics_preview(void* window,const haze_video::Values& values,bool persist,std::string& message) {
    assert(window==owner_window);
    if (!timed_display) return haze_graphics_apply(values,persist,message);
    before_preview=graphics; staged_preview=values;
    graphics.display_preview=++next_preview; graphics.display_seconds=15; graphics.display_restore_failed=false;
    graphics.notice="Previewing display"; return true;
}
bool haze_graphics_confirm(unsigned revision,bool keep,std::string& message) {
    ++confirmations;
    if (!revision || revision!=graphics.display_preview) { message="Stale confirmation"; return false; }
    if (!keep && fail_restore) { graphics.display_restore_failed=true; graphics.notice=message="Could not restore display"; return false; }
    graphics=before_preview;
    if (keep && fail_apply) { graphics.notice=message="Could not save settings. Display reverted."; return false; }
    if (keep) { ++keeps; ++applies; graphics.configured=graphics.effective=staged_preview; graphics.notice="Video settings applied"; }
    else { ++reverts; graphics.notice="Display changes reverted. Settings were not saved."; }
    return true;
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
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT|_WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
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
    timed_display=true; foreground=owner;
    auto preview_page=[&](bool neutral=true) {
        open(owner); provider().change("display_mode","windowed"); action(0);
        assert(graphics.display_preview && modal().choices==std::vector<std::string>({"Revert","Keep"}) && modal().cancel_choice==0);
        if (neutral) { haze_menu_update(owner,{}); assert(modal_selected==0); }
    };
    auto dismiss_result=[&] {
        assert(!graphics.display_preview && modal().choices==std::vector<std::string>({"OK"}));
        key(VK_RETURN); assert(!modal().revision); key(VK_ESCAPE); assert(!haze_menu_active());
    };
    preview_page(); key(VK_RETURN); assert(!keeps && reverts==1); dismiss_result();
    preview_page(); key(VK_RIGHT); assert(modal_selected==1); key(VK_ESCAPE); assert(!keeps && reverts==2); dismiss_result();
    preview_page(); key(VK_RIGHT); key('K'); assert(!keeps && reverts==3); dismiss_result();
    preview_page(); SendMessage(panel,WM_CLOSE,0,0); assert(!keeps && reverts==4); dismiss_result();
    preview_page(); graphics.display_seconds=9; haze_menu_update(owner,{});
    assert(modal().message.find("9 seconds")!=std::string::npos && modal_selected==0);
    render("local/video-display-confirm.bmp");
    auto button=modal_action_rect(1,2); click((button.left+button.right)/2,(button.top+button.bottom)/2);
    assert(keeps==1); dismiss_result();
    preview_page(); foreground=nullptr; haze_menu_update(owner,{}); assert(!graphics.display_preview && reverts==5); foreground=owner; dismiss_result();
    preview_page();
    graphics=before_preview; graphics.notice="Display preview timed out. Previous settings restored.";
    haze_menu_update(owner,{}); assert(modal().message.find("timed out")!=std::string::npos);
    // A queued Keep key after the backend timeout can only dismiss the result.
    key(VK_RIGHT); key(VK_RETURN); assert(keeps==1 && !modal().revision); key(VK_ESCAPE);
    preview_page(); fail_apply=true; key(VK_RIGHT); key(VK_RETURN);
    assert(modal().message.find("Could not save")!=std::string::npos && keeps==1); fail_apply=false; dismiss_result();
    preview_page(); fail_restore=true; key(VK_ESCAPE);
    assert(graphics.display_preview && modal().choices==std::vector<std::string>({"Retry revert"}));
    fail_restore=false; key(VK_RETURN); dismiss_result();
    preview_page(false);
    pad.input.buttons=CELL_PAD_CTRL_CROSS; haze_menu_update(owner,pad); // fresh modal still requires neutral
    assert(graphics.display_preview && keeps==1);
    pad.input.buttons=0; haze_menu_update(owner,pad);
    pad.input.buttons=CELL_PAD_CTRL_RIGHT; haze_menu_update(owner,pad);
    pad.input.buttons=0; haze_menu_update(owner,pad);
    pad.input.buttons=CELL_PAD_CTRL_CROSS; haze_menu_update(owner,pad); assert(keeps==2); dismiss_result();
    preview_page(); pad.input.buttons=0; haze_menu_update(owner,pad);
    pad.input.buttons=CELL_PAD_CTRL_CIRCLE; haze_menu_update(owner,pad); dismiss_result();
    for (const auto& size:sizes) {
        SetWindowPos(owner,nullptr,0,0,size[0],size[1],SWP_NOACTIVATE|SWP_NOZORDER);
        preview_page(); render(size[0]==960 ? "local/video-display-confirm-540p.bmp" : nullptr); key(VK_ESCAPE); dismiss_result();
    }
    timed_display=false;
    unsigned queued=0,changes=0,defaults_count=0,polls=0;
    auto completion=HazeMenuApply::Pending;
    HazeMenuProvider audio;
    audio.title="AUDIO"; audio.begin=[] {};
    audio.defaults=[&] { ++defaults_count; };
    audio.rows=[] { return std::vector<HazeMenuRow>{{"music","Music","100","Music volume",{},true,0,100,5}}; };
    audio.change=[&](const std::string&,const std::string&) { ++changes; };
    audio.apply=[&](std::string& message) { ++queued; message="Saving audio settings..."; return HazeMenuApply::Pending; };
    audio.poll=[&](std::string& message) { ++polls; message=completion==HazeMenuApply::Failed ? "Profile save failed." : "Audio settings saved."; return completion; };
    haze_menu_register(HazeMenuPage::Audio,std::move(audio));
    assert(haze_menu_request(HazeMenuPage::Audio)); haze_menu_update(owner,{});
    action(0); assert(applying && queued==1);
    auto returned=returns;
    key(VK_ESCAPE); key('L'); key(VK_RIGHT); action(0); action(1); action(2);
    assert(applying && returns==returned && queued==1 && changes==0 && defaults_count==0);
    foreground=nullptr; haze_menu_update(owner,{}); assert(applying && polls==1);
    completion=HazeMenuApply::Failed; haze_menu_update(owner,{});
    assert(!applying && notice_error && notice=="Profile save failed.");
    key(VK_ESCAPE); assert(returns==returned+1);
    assert(haze_menu_request(HazeMenuPage::Audio)); haze_menu_update(owner,{});
    action(0); completion=HazeMenuApply::Applied; haze_menu_update(owner,{});
    assert(!applying && !notice_error && notice=="Audio settings saved." && queued==2);
    key(VK_ESCAPE);
    HazeMenuModal capture_dialog;
    unsigned captured_key=0,chosen=0,control_defaults=0;
    HazeMenuProvider controls;
    controls.title="CONTROLS"; controls.begin=[] {};
    controls.rows=[] { return std::vector<HazeMenuRow>{{"binding","Bind key","W","Capture a binding",{},false,0,1,.05,true}}; };
    controls.defaults=[&] { ++control_defaults; };
    controls.activate=[&](const std::string&) { capture_dialog={1,"Capture","Press an input",{"Unbind","Cancel"},true}; };
    controls.modal=[&] { return capture_dialog; };
    controls.modal_key=[&](int vk) { captured_key=vk; capture_dialog={2,"Preview","Apply this input?",{"Use","Unbind","Cancel"},false}; };
    controls.modal_choose=[&](int choice) { chosen=choice+1; capture_dialog={}; };
    controls.update=[&](bool focused) { if (!focused) capture_dialog={}; };
    haze_menu_register(HazeMenuPage::Controls,std::move(controls));
    foreground=owner; assert(haze_menu_request(HazeMenuPage::Controls)); haze_menu_update(owner,{});
    key(VK_RETURN); assert(capture_dialog.capturing);
    key('L'); assert(captured_key=='L' && !capture_dialog.capturing && control_defaults==0);
    render(nullptr);
    key(VK_RIGHT); key(VK_RETURN); assert(chosen==2 && !capture_dialog.revision && haze_menu_active());
    key(VK_RETURN); returned=returns;
    pad.input.buttons=CELL_PAD_CTRL_CIRCLE; haze_menu_update(owner,pad);
    assert(haze_menu_active() && capture_dialog.capturing && returns==returned);
    capture_dialog={2,"Preview","Controller captured",{"Use","Unbind","Cancel"},false};
    haze_menu_update(owner,pad); // The captured held Back cannot dismiss preview.
    assert(capture_dialog.revision && haze_menu_active());
    pad.input.buttons=0; haze_menu_update(owner,pad);
    pad.input.buttons=CELL_PAD_CTRL_CIRCLE; haze_menu_update(owner,pad);
    assert(!capture_dialog.revision && haze_menu_active() && chosen==3 && returns==returned);
    key(VK_RETURN); foreground=nullptr; haze_menu_update(owner,{});
    assert(!capture_dialog.revision && haze_menu_active());
    foreground=owner; key(VK_RETURN); key(VK_ESCAPE);
    assert(!capture_dialog.revision && haze_menu_active());
    key(VK_ESCAPE);
    // Reusable menu teardown reverts; final process teardown must not enter
    // the display backend again, even after a failed restoration.
    timed_display=true; foreground=owner;
    preview_page(); const auto previous_reverts=reverts;
    haze_menu_shutdown(); assert(!graphics.display_preview && reverts==previous_reverts+1);
    for (bool failed:{false,true}) {
        foreground=owner;
        preview_page();
        fail_restore=failed;
        if (failed) { key(VK_ESCAPE); assert(graphics.display_restore_failed); }
        const auto pending=graphics.display_preview, calls=confirmations, saved=applies;
        const auto configured=graphics.configured;
        const auto closing_panel=panel;
        haze_menu_shutdown(true);
        assert(!haze_menu_active() && !IsWindow(closing_panel));
        assert(graphics.display_preview==pending && confirmations==calls && applies==saved);
        assert(graphics.configured==configured);
        graphics=before_preview; fail_restore=false; // Simulate a fresh process.
    }
    DestroyWindow(owner);
    std::puts("Native page navigation, staging, failure recovery, rendering and input tests passed.");
}
