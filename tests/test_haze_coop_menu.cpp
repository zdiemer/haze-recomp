#include <cassert>
#include <map>
#include "../port/haze_coop.h"
#include "../port/haze_menu.h"
#include "../port/net/invite_code.cpp"
#include "../port/haze_coop_settings.cpp"

static std::map<HazeMenuPage,HazeMenuProvider> providers;
static haze::net::Status state;
static unsigned hosts=0, joins=0, leaves=0;
static std::string joined_code;
void haze_menu_register(HazeMenuPage page,HazeMenuProvider provider) { providers[page]=std::move(provider); }
void* haze_menu_owner() { return nullptr; }
HazeMenuView haze_menu_view() { HazeMenuView view; view.generation=42; return view; }
haze::net::Status haze_coop_status() { return state; }
void haze_coop_host() { ++hosts; }
void haze_coop_join(const std::string& code) { ++joins; joined_code=code; }
void haze_coop_leave() { ++leaves; state={}; }
#include "../port/haze_coop_menu.cpp"

int main() {
    const auto root=std::filesystem::temp_directory_path()/("haze-coop-settings-test-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root/"BLUS30094PROFILE");
    { std::ofstream file(root/"BLUS30094PROFILE/_PROFILE.DAT"); file<<"progression sentinel"; }
    _putenv_s("HAZE_COOP_LOCAL_TEST","");
    // An obsolete environment variable must not silently override the UI.
    _putenv_s("HAZE_COOP_SERVICE","wss://obsolete.example/v1");
    haze_coop_settings_initialize(root); assert(haze_coop_server().empty());
    std::string endpoint, error;
    for(const auto bad:{"", "http://example.com", "ws://example.com", "host:0", "host:65536", "wss://user@host/v1", "host/#fragment", "a b", "host?query", "host\\path", "\xc3\xa9.com"})
        assert(!haze_coop_server_normalize(bad,false,endpoint,error) && endpoint.empty() && !error.empty());
    assert(haze_coop_server_normalize(" coop.example:8443 ",false,endpoint,error) && endpoint=="wss://coop.example:8443/v1");
    assert(haze_coop_server_normalize("wss://coop.example/",false,endpoint,error) && endpoint=="wss://coop.example/v1");
    assert(haze_coop_server_normalize("[::1]:443/proxy/v1",false,endpoint,error) && endpoint=="wss://[::1]:443/proxy/v1");
    assert(!haze_coop_server_normalize("ws://127.0.0.1:8765",false,endpoint,error));
    assert(haze_coop_server_normalize("ws://127.0.0.1:8765",true,endpoint,error) && endpoint=="ws://127.0.0.1:8765/v1");
    assert(!haze_coop_server_normalize("ws://127.0.0.1.evil:8765",true,endpoint,error));
    assert(!haze_coop_server_normalize(std::string(241,'a'),false,endpoint,error));
    haze_coop_menu_register();
    auto& host=providers.at(HazeMenuPage::CoopHost);
    auto& join=providers.at(HazeMenuPage::CoopJoin);
    std::string notice;
    host.begin();
    assert(host.apply(notice)==HazeMenuApply::Failed);
    host.activate("invite"); assert(hosts==0);
    host.activate("server");
    for(char c:std::string("coop.example:443")) host.modal_text(c);
    host.modal_key(13); assert(!host.modal().revision && haze_coop_server()=="wss://coop.example:443/v1");
    assert(leaves==1); leaves=0;
    haze_coop_settings_initialize(root); assert(haze_coop_server()=="wss://coop.example:443/v1");
    assert(host.rows()[1].key=="server" && host.rows()[1].value==haze_coop_server());
    host.activate("invite"); assert(hosts==1);
    state.state=haze::net::State::Waiting; state.code="ABCDEFGHJK23";
    // Saving the unchanged address and cancelling edits preserve the live room.
    host.activate("server"); host.modal_choose(0); assert(leaves==0);
    host.activate("server"); host.modal_text(1); host.modal_text('x'); host.modal_choose(1);
    assert(haze_coop_server()=="wss://coop.example:443/v1" && leaves==0);
    host.activate("server"); host.modal_text(1); host.modal_text('x'); host.update(false);
    assert(!host.modal().revision && haze_coop_server()=="wss://coop.example:443/v1" && leaves==0);
    host.activate("server"); host.modal_text(1); host.modal_choose(0);
    assert(host.modal().text_entry && host.modal().message.find("Enter a server address")!=std::string::npos);
    host.modal_choose(1);
    // Failed replacement preserves the active room and both copies of the address.
    HANDLE locked=CreateFileW((root/"co-op-server.txt").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    assert(locked!=INVALID_HANDLE_VALUE);
    host.activate("server"); host.modal_text(1);
    for(char c:std::string("second.example")) host.modal_text(c);
    host.modal_choose(0); assert(host.modal().text_entry && leaves==0 && haze_coop_server()=="wss://coop.example:443/v1");
    CloseHandle(locked); host.modal_choose(1);
    haze_coop_settings_initialize(root); assert(haze_coop_server()=="wss://coop.example:443/v1");
    assert(host.apply(notice)==HazeMenuApply::Applied);
    auto action=haze_coop_take_action(); assert(action.generation==42 && action.host);
    assert(!haze_coop_take_action().generation);
    host.end(false); assert(leaves==0); // Continue preserves the room.
    host.end(true); assert(leaves==1); // Back closes an unfinished room.

    join.begin(); join.activate("connect"); assert(joins==0);
    assert(join.rows()[1].value=="wss://coop.example:443/v1");
    join.activate("invite");
    for(char c:std::string("abcd-efgh-jk23")) join.modal_text(c);
    join.modal_choose(1); // Cancel restores the previous code.
    join.activate("connect"); assert(joins==0);
    join.activate("invite");
    for(char c:std::string("abcd-efgh-jk23")) join.modal_text(c);
    join.modal_choose(0); assert(!join.modal().text_entry);
    join.activate("connect"); assert(joins==1 && joined_code=="ABCDEFGHJK23");
    state.state=haze::net::State::Waiting;
    assert(join.apply(notice)==HazeMenuApply::Failed);
    state.state=haze::net::State::Connected;
    assert(join.apply(notice)==HazeMenuApply::Applied);
    action=haze_coop_take_action(); assert(action.generation==42 && !action.host);
    join.activate("server"); join.modal_text(1);
    for(char c:std::string("new.example")) join.modal_text(c);
    join.modal_choose(0); assert(leaves==2 && state.state==haze::net::State::Offline);
    assert(join.apply(notice)==HazeMenuApply::Failed);
    haze_coop_settings_initialize(root); assert(haze_coop_server()=="wss://new.example/v1");
    join.activate("invite"); join.modal_text(8); join.update(false);
    join.activate("connect"); assert(joined_code=="ABCDEFGHJK23");
    join.change("mode","lan"); assert(leaves==3 && join.rows().size()==1);
    assert(join.apply(notice)==HazeMenuApply::Applied);
    join.defaults(); assert(leaves==4);
    { std::ofstream file(root/"co-op-server.txt"); file<<"invalid saved address"; }
    haze_coop_settings_initialize(root); assert(haze_coop_server().empty() && !haze_coop_settings_error().empty());
    join.begin(); join.activate("connect"); assert(join.rows()[4].value=="Enter a server address first.");
    assert(haze_coop_server_save("repaired.example",error) && haze_coop_settings_error().empty());
    { std::ifstream file(root/"BLUS30094PROFILE/_PROFILE.DAT"); std::string value; std::getline(file,value); assert(value=="progression sentinel"); }
    std::filesystem::remove_all(root); // Only this process's uniquely named fixture.
}
