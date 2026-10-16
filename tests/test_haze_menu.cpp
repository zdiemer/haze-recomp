// Exercise the native menu command bridge with the real Video provider.
// Rendering and guest input acceptance are separate runtime checks.
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
static HWND owner=reinterpret_cast<HWND>(uintptr_t(1));
static void tick() { haze_menu_update(owner,{}); }
static void send(HazeMenuCommandKind kind,const std::string& key={},const std::string& value={},int choice=0) {
    const auto view=haze_menu_view();
    haze_menu_command({view.generation,kind,key,value,choice,view.modal.revision}); tick();
}
static void open(HazeMenuPage page=HazeMenuPage::Video) {
    assert(haze_menu_request(page)); tick();
    assert(haze_menu_blocks_input());
    haze_menu_presented(haze_menu_view().generation); tick();
}
int main() {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT|_WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    graphics.configured=graphics.effective=haze_video::defaults();
    foreground=owner; haze_video_menu_register();
    using K=HazeMenuCommandKind;
    assert(!haze_menu_request(HazeMenuPage::Audio));
    assert(!haze_menu_request(HazeMenuPage(99)));
    open(); assert(haze_menu_active() && !haze_menu_blocks_input() && !activations);
    send(K::Change,"quality","Medium");
    assert(edited.at("anisotropic_filtering")=="4" && graphics.configured.at("anisotropic_filtering")=="1");
    send(K::Apply); assert(applies==1 && graphics.configured.at("anisotropic_filtering")=="4");
    send(K::Change,"quality","High");
    const auto old=haze_menu_view();
    send(K::Back); assert(!haze_menu_active() && haze_menu_view().pop);
    open(); assert(haze_video::preset_name(edited)=="Medium");
    send(K::Change,"performance_overlay","expanded");
    send(K::Change,"overlay_position","bottom-right");
    assert(edited.at("performance_overlay")=="expanded" && edited.at("overlay_position")=="bottom-right");
    haze_menu_command({old.generation,K::Defaults}); tick();
    assert(haze_video::preset_name(edited)=="Medium"); // A destroyed widget cannot edit a new page.
    send(K::Change,"quality","High"); fail_apply=true; send(K::Apply);
    assert(haze_menu_view().error && haze_menu_view().notice=="Could not save settings.");
    assert(graphics.configured.at("anisotropic_filtering")=="4");
    send(K::Defaults); assert(!haze_menu_view().error && edited==haze_video::defaults());
    fail_apply=false; send(K::Apply); assert(graphics.configured==haze_video::defaults());
    send(K::Back);
    timed_display=true;
    auto preview=[&] {
        open(); send(K::Change,"display_mode","windowed"); send(K::Apply);
        assert(graphics.display_preview);
        assert(haze_menu_view().modal.choices==std::vector<std::string>({"Revert","Keep"}));
        assert(haze_menu_view().modal.cancel_choice==0);
    };
    auto dismiss=[&] {
        assert(!graphics.display_preview && haze_menu_view().modal.choices==std::vector<std::string>({"OK"}));
        send(K::Modal,{}, {},0); assert(!haze_menu_view().modal.revision);
        send(K::Back); assert(!haze_menu_active());
    };
    preview(); send(K::Back); assert(!keeps && reverts==1); dismiss();
    preview(); graphics.display_seconds=9; tick();
    assert(haze_menu_view().modal.message.find("9 seconds")!=std::string::npos);
    send(K::Modal,{}, {},1); assert(keeps==1); dismiss();
    preview(); foreground=nullptr; tick();
    assert(!graphics.display_preview && reverts==2); foreground=owner; dismiss();
    preview(); const auto expired=haze_menu_view();
    graphics=before_preview; graphics.notice="Display preview timed out. Previous settings restored."; tick();
    haze_menu_command({expired.generation,K::Modal,{}, {},1,expired.modal.revision}); tick();
    assert(keeps==1 && haze_menu_view().modal.choices==std::vector<std::string>({"OK"})); dismiss();
    preview(); fail_apply=true; send(K::Modal,{}, {},1);
    assert(haze_menu_view().modal.message.find("Could not save")!=std::string::npos && keeps==1);
    fail_apply=false; dismiss();
    preview(); fail_restore=true; send(K::Back);
    assert(graphics.display_preview && haze_menu_view().modal.choices==std::vector<std::string>({"Retry revert"}));
    fail_restore=false; send(K::Modal,{}, {},0); dismiss();
    preview(); const auto revert_count=reverts; haze_menu_shutdown();
    assert(!graphics.display_preview && reverts==revert_count+1);
    preview(); const auto confirmations_before=confirmations;
    haze_menu_shutdown(true); assert(!haze_menu_active() && graphics.display_preview && confirmations==confirmations_before);
    graphics=before_preview; timed_display=false;

    unsigned queued=0,changes=0,defaults_count=0;
    auto completion=HazeMenuApply::Pending;
    HazeMenuProvider audio;
    audio.title="AUDIO";
    audio.rows=[] { return std::vector<HazeMenuRow>{{"music","Music","100","Music volume",{},true,0,100,5}}; };
    audio.change=[&](const std::string&,const std::string&) { ++changes; };
    audio.defaults=[&] { ++defaults_count; };
    audio.apply=[&](std::string&) { ++queued; return HazeMenuApply::Pending; };
    audio.poll=[&](std::string& message) { message=completion==HazeMenuApply::Failed ? "Profile save failed." : "Audio settings saved."; return completion; };
    haze_menu_register(HazeMenuPage::Audio,std::move(audio));
    open(HazeMenuPage::Audio); send(K::Apply);
    assert(haze_menu_view().applying && haze_menu_blocks_input());
    assert(!haze_menu_request(HazeMenuPage::Video));
    send(K::Back); send(K::Defaults); send(K::Change,"music","50"); send(K::Apply);
    assert(haze_menu_active() && queued==1 && !changes && !defaults_count);
    foreground=nullptr; completion=HazeMenuApply::Failed; tick();
    assert(!haze_menu_view().applying && haze_menu_view().error && haze_menu_view().notice=="Profile save failed.");
    send(K::Back); assert(!haze_menu_active()); foreground=owner;

    HazeMenuModal capture_dialog;
    unsigned captured=0,chosen=0;
    HazeMenuProvider controls;
    controls.title="CONTROLS";
    controls.rows=[] { return std::vector<HazeMenuRow>{{"binding","Bind key","W","Capture a binding",{},false,0,1,.05,true}}; };
    controls.activate=[&](const std::string&) { capture_dialog={1,"Capture","Press an input",{"Unbind","Cancel"},true}; };
    controls.modal=[&] { return capture_dialog; };
    controls.modal_key=[&](int vk) { captured=vk; capture_dialog={2,"Preview","Apply input?",{"Use","Unbind","Cancel"},false}; };
    controls.modal_choose=[&](int choice) { chosen=choice+1; capture_dialog={}; };
    controls.update=[&](bool focused) { if (!focused) capture_dialog={}; };
    haze_menu_register(HazeMenuPage::Controls,std::move(controls));
    open(HazeMenuPage::Controls); send(K::Activate,"binding");
    assert(haze_menu_blocks_input() && haze_menu_capture_key('L')); tick();
    assert(!haze_menu_request(HazeMenuPage::Video));
    assert(captured=='L' && !haze_menu_view().modal.capturing && !haze_menu_blocks_input());
    assert(!haze_menu_capture_key('K'));
    send(K::Back); assert(chosen==3 && haze_menu_active());
    send(K::Activate,"binding"); foreground=nullptr; tick();
    assert(!haze_menu_view().modal.revision && haze_menu_active());
    send(K::Departed); assert(!haze_menu_active() && !haze_menu_view().pop);
    foreground=owner;
    HazeMenuModal text_dialog{7,"Name","Type a name",{"Use name","Cancel"},true,1,true};
    std::vector<uint32_t> text;
    HazeMenuProvider profile;
    profile.modal=[&] { return text_dialog; };
    profile.modal_text=[&](uint32_t c) { text.push_back(c); };
    profile.modal_choose=[&](int) { text_dialog={}; };
    haze_menu_register(HazeMenuPage::Profile,std::move(profile));
    open(HazeMenuPage::Profile);
    assert(haze_menu_capture_text('a')); tick(); assert(text==std::vector<uint32_t>{'a'});
    HazeMenuCommand stale; stale.generation=haze_menu_view().generation; stale.kind=K::Text; stale.choice='b'; stale.modal_revision=6;
    haze_menu_command(stale); tick(); assert(text.size()==1);
    haze_menu_interrupt({1,"Interrupt","Keep typing private",{"OK"},false});
    assert(!haze_menu_capture_text('c'));
    haze_menu_interrupt({}); send(K::Back); assert(!haze_menu_capture_text('d'));
    haze_menu_shutdown();
    std::puts("Native menu commands, staging, preview rollback, stale events and pending saves passed.");
}
