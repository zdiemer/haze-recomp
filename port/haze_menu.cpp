#include "haze_menu.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
std::array<HazeMenuProvider,size_t(HazeMenuPage::Count)> providers;
std::array<std::atomic<bool>,size_t(HazeMenuPage::Count)> available{};
std::atomic<int> requested{0};
std::atomic<bool> active{false},blocked{false};
std::atomic<uint64_t> presented{0};
std::mutex exchange_mutex;
std::deque<HazeMenuCommand> commands;
HazeMenuView published,current_view;
HazeMenuModal interrupt_dialog;
std::function<void(int)> interrupt_choose;
void* owner_window=nullptr;
uint64_t next_generation=0;
HazeMenuProvider& provider() { return providers[size_t(current_view.page)]; }
HazeMenuModal modal() { return interrupt_dialog.revision ? interrupt_dialog : provider().modal ? provider().modal() : HazeMenuModal{}; }
void publish() {
    if (current_view.page!=HazeMenuPage::None) {
        current_view.rows=provider().rows ? provider().rows() : std::vector<HazeMenuRow>{};
        current_view.modal=modal();
        current_view.title=provider().title;
        current_view.actions=provider().actions;
    }
    blocked=active && (presented!=current_view.generation || (current_view.applying && !interrupt_dialog.revision) || current_view.modal.capturing);
    std::lock_guard lock(exchange_mutex);
    if (published.generation!=current_view.generation || published.page!=current_view.page ||
        published.title!=current_view.title || published.notice!=current_view.notice ||
        published.rows!=current_view.rows || published.modal!=current_view.modal ||
        published.actions!=current_view.actions || published.applying!=current_view.applying ||
        published.error!=current_view.error || published.pop!=current_view.pop) {
        current_view.revision=published.revision+1;
        published=current_view;
    }
}
void close_page(bool pop) {
    if (current_view.page==HazeMenuPage::None) return;
    const bool guest_back=provider().guest_back;
    if (provider().end) provider().end(pop);
    if (provider().update) provider().update(false);
    current_view.page=HazeMenuPage::None;
    current_view.rows.clear(); current_view.modal={};
    current_view.applying=false; current_view.pop=pop && guest_back;
    active=false; blocked=false;
}
void dispatch(const HazeMenuCommand& command) {
    if (command.generation!=current_view.generation || current_view.page==HazeMenuPage::None) return;
    if (command.kind==HazeMenuCommandKind::Departed) { close_page(false); return; }
    if (current_view.applying && !interrupt_dialog.revision) return;
    const auto dialog=modal();
    if (dialog.revision) {
        if (command.modal_revision!=dialog.revision) return;
        // Copy the callback: cancelling an interrupt can clear its owner.
        const auto choose=interrupt_dialog.revision ? interrupt_choose : provider().modal_choose;
        if (command.kind==HazeMenuCommandKind::Modal && choose &&
            command.choice>=0 && command.choice<int(dialog.choices.size()))
            choose(command.choice);
        else if (command.kind==HazeMenuCommandKind::Key && dialog.capturing && provider().modal_key)
            provider().modal_key(command.choice);
        else if (command.kind==HazeMenuCommandKind::Text && dialog.text_entry && !interrupt_dialog.revision && provider().modal_text)
            provider().modal_text(uint32_t(command.choice));
        else if (command.kind==HazeMenuCommandKind::Back && choose) {
            const int cancel=dialog.cancel_choice>=0 ? dialog.cancel_choice : int(dialog.choices.size())-1;
            if (cancel>=0 && cancel<int(dialog.choices.size())) choose(cancel);
        }
        return;
    }
    switch (command.kind) {
    case HazeMenuCommandKind::Change: {
        const auto row=std::find_if(current_view.rows.begin(),current_view.rows.end(),[&](const auto& r){return r.key==command.key;});
        if (row!=current_view.rows.end() && provider().change) {
            provider().change(command.key,command.value); current_view.notice.clear(); current_view.error=false;
        }
        break;
    }
    case HazeMenuCommandKind::Activate:
        if (provider().activate && std::any_of(current_view.rows.begin(),current_view.rows.end(),[&](const auto& r){return r.key==command.key && r.action;}))
            provider().activate(command.key);
        break;
    case HazeMenuCommandKind::Apply:
        if (provider().apply) {
            const auto result=provider().apply(current_view.notice);
            current_view.applying=result==HazeMenuApply::Pending;
            current_view.error=result==HazeMenuApply::Failed;
        }
        break;
    case HazeMenuCommandKind::Defaults:
        if (provider().defaults) provider().defaults();
        current_view.notice=provider().defaults_notice; current_view.error=false;
        break;
    case HazeMenuCommandKind::Back: close_page(true); break;
    default: break;
    }
}
}
void haze_menu_register(HazeMenuPage page,HazeMenuProvider value) {
    if (page<=HazeMenuPage::None || size_t(page)>=providers.size()) return;
    providers[size_t(page)]=std::move(value); available[size_t(page)]=true;
}
bool haze_menu_request(HazeMenuPage page) {
    if (page<=HazeMenuPage::None || size_t(page)>=providers.size() || !available[size_t(page)]) return false;
    {
        std::lock_guard lock(exchange_mutex);
        if (active && (published.applying || published.modal.revision)) return false;
    }
    blocked=true; active=true; requested=int(page); return true;
}
bool haze_menu_active() { return active.load(); }
bool haze_menu_blocks_input() { return blocked.load(); }
bool haze_menu_has_focus() {
#ifdef _WIN32
    return active && owner_window && GetForegroundWindow()==owner_window;
#else
    return active;
#endif
}
void* haze_menu_owner() { return owner_window; }
HazeMenuView haze_menu_view() { std::lock_guard lock(exchange_mutex); return published; }
void haze_menu_command(HazeMenuCommand command) {
    std::lock_guard lock(exchange_mutex);
    // Only the displayed page may issue commands. Bound work from input bursts.
    if (command.generation==published.generation && commands.size()<256) commands.push_back(std::move(command));
}
void haze_menu_presented(uint64_t generation) { presented=generation; }
bool haze_menu_capture_key(int key) {
    const auto view=haze_menu_view();
    if (!active || !view.modal.capturing) return false;
    HazeMenuCommand command; command.generation=view.generation;
    command.kind=key==27 ? HazeMenuCommandKind::Back : HazeMenuCommandKind::Key; command.choice=key; command.modal_revision=view.modal.revision;
    haze_menu_command(std::move(command)); return true;
}
bool haze_menu_capture_text(uint32_t character) {
    const auto view=haze_menu_view();
    if (!active || !view.modal.text_entry) return false;
    HazeMenuCommand command; command.generation=view.generation;
    command.kind=HazeMenuCommandKind::Text; command.choice=int(character); command.modal_revision=view.modal.revision;
    haze_menu_command(std::move(command)); return true;
}
void haze_menu_update(void* owner,const HazeGamepadState&) {
    owner_window=owner;
    if (const int next=requested.exchange(0)) {
        close_page(false);
        const auto revision=current_view.revision;
        current_view={}; current_view.page=HazeMenuPage(next);
        current_view.generation=++next_generation; current_view.revision=revision;
        active=true;
        if (provider().begin) provider().begin();
        publish();
    }
    std::deque<HazeMenuCommand> pending;
    { std::lock_guard lock(exchange_mutex); pending.swap(commands); }
    for (const auto& command:pending) dispatch(command);
    if (current_view.page!=HazeMenuPage::None) {
        if (provider().update) provider().update(haze_menu_has_focus());
        if (current_view.applying) {
            const auto result=provider().poll ? provider().poll(current_view.notice) : HazeMenuApply::Failed;
            if (!provider().poll) current_view.notice="The settings provider cannot report save completion.";
            current_view.applying=result==HazeMenuApply::Pending;
            current_view.error=result==HazeMenuApply::Failed;
        }
    }
    publish();
}
void haze_menu_retail() { close_page(false); publish(); }
void haze_menu_interrupt(HazeMenuModal dialog,std::function<void(int)> choose) {
    // Provider modal revisions are local counters. Keep interrupts in a separate
    // range so an old confirmation cannot activate the overlaid Cancel button.
    if (dialog.revision) dialog.revision|=0x80000000u;
    interrupt_dialog=std::move(dialog); interrupt_choose=std::move(choose); publish();
}
void haze_menu_shutdown(bool exiting_process) {
    if (!exiting_process) close_page(false);
    active=false; blocked=false; requested=0; presented=0;
    current_view={}; interrupt_dialog={}; interrupt_choose={}; owner_window=nullptr;
    std::lock_guard lock(exchange_mutex); commands.clear(); published={};
}
