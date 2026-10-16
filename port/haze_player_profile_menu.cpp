#include "haze_player_profile.h"
#include "haze_menu.h"

namespace {
std::string edited,typing,feedback;
HazeMenuModal dialog;
unsigned revision=0;
void refresh_text() {
    dialog.message="Name: "+typing+"_\n"+(feedback.empty() ? "3-16 characters: A-Z, 0-9, _ and -. Enter accepts; Escape cancels." : feedback);
}
void choose(int choice) {
    if (choice==0) {
        if (!haze_player_name_valid(typing,feedback)) { refresh_text(); return; }
        edited=typing;
    }
    dialog={}; feedback.clear();
}
void append(const std::string& value) {
    if (edited.size()+value.size()>16) { feedback="Names can contain at most 16 characters."; return; }
    edited+=value; feedback.clear();
}
std::vector<HazeMenuRow> rows() {
    HazeMenuRow current; current.key="current"; current.label="Current profile name"; current.value=haze_player_name();
    HazeMenuRow name; name.key="type"; name.label="Edit profile name"; name.value=edited; name.action=true;
    name.hint="Type a name with your keyboard, or use Add character and Delete last character below.";
    HazeMenuRow character; character.key="character"; character.label="Add character";
    for (const char c:std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"))
        character.choices.push_back({std::string(1,c),std::string(1,c)});
    HazeMenuRow erase; erase.key="erase"; erase.label="Delete last character"; erase.action=true;
    HazeMenuRow clear; clear.key="clear"; clear.label="Clear name"; clear.action=true;
    HazeMenuRow notice; notice.key="feedback"; notice.label="Name rules";
    notice.value=feedback.empty() ? "3-16 characters; start with A-Z" : feedback;
    return {current,name,character,erase,clear,notice};
}
}
void haze_player_profile_menu_register() {
    HazeMenuProvider p;
    p.title="PLAYER PROFILE"; p.guest_back=false;
    p.begin=[] { edited=haze_player_name(); feedback=haze_player_profile_error(); dialog={}; };
    p.rows=rows;
    p.change=[](const std::string& key,const std::string& value) {
        if (key=="character" && value.size()==1 && std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-").find(value)!=std::string::npos) append(value);
    };
    p.activate=[](const std::string& key) {
        if (key=="erase") { if (!edited.empty()) edited.pop_back(); feedback.clear(); }
        else if (key=="clear") { edited.clear(); feedback.clear(); }
        else if (key=="type") {
            typing=edited; feedback.clear();
            dialog={++revision,"Edit profile name",{}, {"Use name","Cancel"},true,1,true}; refresh_text();
        }
    };
    p.defaults=[] { edited="User"; feedback.clear(); };
    p.apply=[](std::string& notice) {
        if (!haze_player_name_save(edited,notice)) return HazeMenuApply::Failed;
        notice="Profile name saved. Saves and progression are preserved.";
        return HazeMenuApply::Applied;
    };
    p.modal=[] { return dialog; }; p.modal_choose=choose;
    p.modal_key=[](int key) { if (key==13) choose(0); };
    p.modal_text=[](uint32_t c) {
        if (!dialog.text_entry) return;
        if (c==8) { if (!typing.empty()) typing.pop_back(); feedback.clear(); }
        else if (c==13 || c==27) return;
        else if (!((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-'))
            feedback="Use only A-Z, 0-9, _ and -.";
        else if (typing.size()==16) feedback="Names can contain at most 16 characters.";
        else { typing+=char(c); feedback.clear(); }
        refresh_text();
    };
    p.update=[](bool focused) { if (!focused && dialog.revision) { dialog={}; feedback="Typing cancelled. The staged name is unchanged."; } };
    p.footer_hint="Apply saves your display name without changing saves or progression.";
    haze_menu_register(HazeMenuPage::Profile,std::move(p));
}
