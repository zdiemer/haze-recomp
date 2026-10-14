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
HazeQuitOffer haze_quit_offer() { return current_offer; }
bool haze_quit_guest_ready() { return true; }
void haze_quit_resume_guest() {}
#include "../port/haze_menu.cpp"
static HWND owner=reinterpret_cast<HWND>(uintptr_t(1));
static void frame() { haze_menu_update(owner,{}); }
static void send(HazeMenuCommandKind kind,int choice=0) {
    const auto view=haze_menu_view();
    haze_menu_command({view.generation,kind,{}, {},choice,view.modal.revision}); frame();
}
static void open() { assert(haze_menu_request(HazeMenuPage::Quit)); frame(); }
int main() {
    foreground=owner; haze_quit_menu_register();
    using K=HazeMenuCommandKind;
    open(); assert(haze_quit_state().phase==HazeQuitPhase::Confirm);
    assert(haze_menu_view().modal.cancel_choice==0 && haze_menu_view().modal.choices[0]=="Cancel");
    send(K::Modal,0); assert(!haze_menu_active() && !haze_quit_menu_active());
    open(); const auto stale=haze_menu_view(); send(K::Back);
    open(); haze_menu_command({stale.generation,K::Modal,{}, {},1,stale.modal.revision}); frame();
    assert(haze_quit_state().phase==HazeQuitPhase::Confirm);
    send(K::Modal,1); assert(haze_quit_state().phase==HazeQuitPhase::Waiting);
    send(K::Back); assert(!haze_menu_active() && !haze_quit_menu_active());
    current_offer={2,true,true}; open();
    assert(haze_menu_view().modal.message.find("checkpoint")!=std::string::npos);
    auto save=haze_save_begin(0xe71ac8); send(K::Modal,1);
    assert(!haze_quit_poll(2,false,false)); haze_save_finished(save,-1);
    assert(!haze_quit_poll(2,false,false)); frame();
    assert(haze_quit_state().phase==HazeQuitPhase::Failed && haze_menu_view().modal.choices.size()==1);
    send(K::Back); assert(!haze_quit_menu_active() && !haze_menu_active());
    open(); foreground=nullptr; frame(); frame();
    assert(!haze_quit_menu_active() && !haze_menu_active());
    // A window close while Apply is pending must keep polling the transaction,
    // show save failures, and let Cancel return to the same provider generation.
    foreground=owner;
    unsigned polls=0,begins=0;
    HazeMenuProvider pending;
    pending.title="AUDIO"; pending.begin=[&] { ++begins; };
    pending.apply=[](std::string&) { return HazeMenuApply::Pending; };
    pending.poll=[&](std::string&) { ++polls; return HazeMenuApply::Pending; };
    haze_menu_register(HazeMenuPage::Audio,pending);
    assert(haze_menu_request(HazeMenuPage::Audio)); frame();
    haze_menu_presented(haze_menu_view().generation); frame();
    send(K::Apply);
    const auto saving=haze_menu_view();
    assert(saving.applying);
    current_offer={3,false,true}; save=haze_save_begin(0xe71ac8);
    assert(haze_quit_window_close()); haze_quit_menu_update(owner,{}); frame();
    assert(haze_menu_view().page==HazeMenuPage::Audio && haze_menu_view().modal.revision);
    assert(!haze_menu_blocks_input() && polls>1);
    haze_save_finished(save,-1); assert(!haze_quit_poll(0,false,true));
    haze_quit_menu_update(owner,{}); frame();
    assert(haze_menu_view().modal.message.find("failed")!=std::string::npos);
    send(K::Back);
    assert(!haze_quit_menu_active() && haze_menu_view().generation==saving.generation);
    assert(haze_menu_view().applying && !haze_menu_view().modal.revision && begins==1);
    haze_menu_retail();
    // Loading/no offer uses the same native provider. Before a page is ready,
    // Escape still cancels instead of leaving input blocked indefinitely.
    assert(haze_quit_window_close()); haze_quit_menu_update(owner,{}); frame();
    assert(haze_menu_view().page==HazeMenuPage::Quit);
    assert(haze_quit_menu_key(27,false)); frame(); frame();
    assert(!haze_quit_menu_active() && !haze_menu_active());
    current_offer={4,true,false}; open(); send(K::Modal,1);
    assert(haze_quit_poll(4,false,false));
    assert(haze_quit_take_shutdown(true));
    haze_menu_shutdown(true); haze_quit_menu_shutdown();
    std::puts("Native Quit provider: Cancel-first, stale events, save wait/failure, focus cancellation and shutdown passed.");
}
