#include "ppu_recomp.h"
#include "haze_menu.h"
#include "haze_quit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void ppu_register_function(uint64_t,void (*)(ppu_context*));
namespace {
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
constexpr uint32_t gateway=0xfffe7300;
uint32_t serial=0,registered_manager=0,callback_storage=0;
uint32_t target_page=0,target_context=0;
uint32_t return_widget=0;
struct Row { uint32_t widget=0; std::string key,label,model_value; int last_value=0; bool slider=false,toggle=false; };
struct Page {
    uint32_t page=0,context=0,container=0,notice=0,message=0;
    std::string status;
    uint64_t generation=0,revision=0;
    unsigned modal=0;
    std::vector<Row> rows;
    std::vector<uint32_t> columns,original_events;
    std::vector<std::pair<uint32_t,uint32_t>> original_children;
};
Page displayed;
struct Action {
    uint64_t generation=0;
    HazeMenuCommandKind kind=HazeMenuCommandKind::Activate;
    std::string key;
    int choice=0;
    unsigned modal=0;
    int local=0;
    std::string value;
};
struct Cache {
    uint32_t context=0,handle=0,title_mode=0,title_id=0,title_flags=0;
    std::string name,title;
    std::unordered_map<std::string,uint32_t> widgets;
    std::unordered_set<uint32_t> used;
    std::vector<std::pair<uint32_t,uint32_t>> originals;
    std::vector<uint32_t> events;
    std::vector<uint64_t> root_children;
};
std::unordered_map<uint32_t,Cache> caches;
std::unordered_map<uint32_t,uint32_t> bindings,main_roots,main_handles;
size_t section=0,choice_section=0,modal_section=0;
std::string choice_key;
bool refresh=false;
std::unordered_map<uint32_t,Action> actions;
std::unordered_set<uint32_t> popup_panels;
bool valid(uint32_t p) { return p>=0x10000 && p<0xbffff000 && !(p&3); }
uint32_t call(ppu_context* ctx,void(*fn)(ppu_context*),uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0) {
    const auto saved=*ctx; auto trampoline=g_trampoline_fn; g_trampoline_fn=nullptr;
    ctx->gpr[1]-=0x10000; ctx->gpr[2]=0xe94ee0;
    ctx->gpr[3]=a; ctx->gpr[4]=b; ctx->gpr[5]=c; ctx->gpr[6]=d; ctx->gpr[7]=e;
    fn(ctx);
    while (g_trampoline_fn) { auto next=g_trampoline_fn; g_trampoline_fn=nullptr; next(ctx); }
    const auto result=uint32_t(ctx->gpr[3]); *ctx=saved; g_trampoline_fn=trampoline; return result;
}
void put(uint32_t address,const std::string& text) {
    for (size_t i=0;i<text.size();++i) vm_write8(address+uint32_t(i),uint8_t(text[i]));
    vm_write8(address+uint32_t(text.size()),0);
}
std::string id(uint32_t widget) {
    char name[17]{};
    if (valid(widget)) for (unsigned i=0;i<16;++i) name[i]=char(vm_read8(widget+0x18+i));
    return name;
}
std::string next_id() { char name[16]; std::snprintf(name,sizeof(name),"pc_%08x",++serial); return name; }
std::string wrap(const std::string& text,size_t columns) {
    std::istringstream lines(text); std::string result,line;
    while (std::getline(lines,line)) {
        if (!result.empty()) result+='\n';
        std::istringstream words(line); std::string word; size_t used=0;
        while (words>>word) {
            if (used && used+word.size()+1>columns) { result+='\n'; used=0; }
            if (used) { result+=' '; ++used; }
            // Paths and binding names may have no whitespace. Keep every
            // character readable instead of allowing one word off the panel.
            while (word.size()>columns) {
                result+=word.substr(0,columns); result+='\n';
                word.erase(0,columns); used=0;
            }
            result+=word; used+=word.size();
        }
    }
    return result;
}
std::vector<std::string> modal_pages(const std::string& message) {
    std::istringstream lines(wrap(message,56));
    std::vector<std::string> pages(1); std::string line; unsigned count=0;
    while (std::getline(lines,line)) {
        if (count==4) { pages.emplace_back(); count=0; }
        if (count++) pages.back()+='\n';
        pages.back()+=line;
    }
    return pages;
}
void label(ppu_context* ctx,uint32_t widget,const std::string& text) {
    if (!valid(widget) || text.size()>4095) return;
    const auto component=vm_read32(widget+0x14c);
    if (!valid(component)) return;
    const auto scratch=uint32_t(ctx->gpr[1])-0x5000;
    put(scratch,text); call(ctx,func_003210F8,component,scratch);
}
uint32_t create(ppu_context* ctx,const std::string& type,const std::string& name,const std::string& properties) {
    std::string definition=properties;
    uint32_t release=0;
    if (type=="text") {
        const auto event_name=next_id();
        release=create(ctx,"menuevent",event_name,"owner = \""+name+
            "\"\nevent = \"onRelease\"\ncommand = \"callbackWithMenu\"\nrepeat = \"true\"\nplatforms = \"k_ps3\"\ncmd-string1 = \"pcNativeMenuAction\"\n");
        if (!release) return 0;
        // Declaring the event during widget loading allocates its native event
        // array. Attaching later to a zero-capacity array silently does nothing.
        definition+="string events [] { \""+event_name+"\" }\n";
    }
    const std::string source=type+" "+name+" {\n"+definition+"\n}\n";
    if (source.size()>0x6000 || ctx->gpr[1]<0x20000) return 0;
    const auto stack=uint32_t(ctx->gpr[1]);
    const auto parser=stack-0xf000,dictionary=stack-0xd800,text=stack-0xc000,key=stack-0x5800;
    put(text,source); vm_write8(text+uint32_t(source.size())+1,0); put(key,name);
    call(ctx,func_007AD8D0,dictionary,UINT32_MAX);
    call(ctx,func_0078D878,parser,dictionary,dictionary,0,0);
    uint32_t root=0,widget=0;
    if (call(ctx,func_0078CE58,parser,text,source.size()+2,key,0)) {
        call(ctx,func_0078C6C8,parser,0,0); root=call(ctx,func_0078D6B0,parser);
        if (root) {
            const auto node=call(ctx,func_0078AB58,root,key);
            if (node) widget=call(ctx,func_002BC7B0,node,key,0,0,0);
        }
    }
    vm_write32(parser+0x20,root);
    call(ctx,func_0078CE28,parser); call(ctx,func_007AD9A0,dictionary);
    if (widget && release) bindings[widget]=release;
    return widget;
}
bool attach(ppu_context* ctx,uint32_t widget) {
    if (!valid(widget)) return false;
    const auto parent=call(ctx,func_003141D0,widget+0x40,vm_read32(widget+0xcc));
    if (!valid(parent) || vm_read32(parent+0x60)>=vm_read32(parent+0x5c)) return false;
    call(ctx,func_002E6C20,parent,widget); return true;
}
void action_callback(ppu_context* ctx) {
    const auto owner=uint32_t(ctx->gpr[3]);
    std::fprintf(stderr,"[native-menu] Activate %s\n",id(owner).c_str());
    const auto found=actions.find(owner);
    if (found!=actions.end()) {
        const auto& a=found->second;
        HazeMenuCommand command; command.generation=a.generation; command.kind=a.kind;
        command.key=a.key; command.choice=a.choice; command.modal_revision=a.modal;
        command.value=a.value;
        if (a.local==6 || a.local==7) {
            const auto view=haze_menu_view();
            if (view.generation==a.generation && view.modal.revision==a.modal) {
                if (a.local==7) ++modal_section;
                else if (modal_section) --modal_section;
                refresh=true;
            }
            ctx->gpr[3]=1; return;
        }
        if (a.local==4 || a.local==5) {
            const auto offer=haze_quit_offer();
            const auto root=main_roots.find(owner);
            if (root!=main_roots.end() && offer.available && !haze_menu_active() &&
                (a.local==4 || !offer.gameplay)) {
                target_page=root->second; target_context=vm_read32(owner+0xcc);
                return_widget=owner;
                haze_menu_request(a.local==4 ? HazeMenuPage::Quit : HazeMenuPage::Desktop);
            }
            ctx->gpr[3]=1; return;
        }
        if (a.local) {
            const auto view=haze_menu_view();
            if (a.generation!=view.generation || view.applying || view.modal.revision) { ctx->gpr[3]=1; return; }
            if (a.local==1) ++(choice_key.empty() ? section : choice_section);
            else if (a.local==-1) { auto& index=choice_key.empty() ? section : choice_section; if (index) --index; }
            else if (a.local==2) { choice_key=a.key; choice_section=0; }
            else if (a.local==3) { choice_key.clear(); choice_section=0; }
            refresh=true; ctx->gpr[3]=1; return;
        }
        if (a.kind==HazeMenuCommandKind::Change && !choice_key.empty()) { choice_key.clear(); refresh=true; }
        haze_menu_command(std::move(command));
    }
    ctx->gpr[3]=1;
}
bool register_callback(ppu_context* ctx) {
    const auto manager=vm_read32(0xf48dd4);
    if (!valid(manager)) return false;
    if (registered_manager==manager) return true;
    const auto registry=vm_read32(manager+0x1530);
    if (!valid(registry) || vm_read32(registry+0x26c)>=25) return false;
    if (!callback_storage) callback_storage=call(ctx,func_00455B50,64,0);
    if (!valid(callback_storage)) return false;
    vm_write32(callback_storage,gateway); vm_write32(callback_storage+4,0xe94ee0);
    put(callback_storage+8,"pcNativeMenuAction");
    ppu_register_function(gateway,action_callback);
    call(ctx,func_0030E688,manager,callback_storage+8,callback_storage);
    registered_manager=manager; return true;
}
void bind(ppu_context* ctx,uint32_t widget,Action action,const char* event="onRelease") {
    actions[widget]=std::move(action);
    if (!bindings.count(widget)) {
        bindings[widget]=create(ctx,"menuevent",next_id(),"owner = \""+id(widget)+"\"\nevent = \""+event+
            "\"\ncommand = \"callbackWithMenu\"\nrepeat = \"true\"\nplatforms = \"k_ps3\"\ncmd-string1 = \"pcNativeMenuAction\"\n");
    }
    if (valid(bindings[widget]) && !vm_read32(bindings[widget]+0x348)) call(ctx,func_002E8200,bindings[widget]);
    // A page's original event list is restored when the PC view closes.
    // Reuse the existing event object when the page is opened again.
    if (std::strcmp(event,"onPageBack")==0 && valid(bindings[widget])) {
        if (!vm_read32(widget+0x74) && !vm_read32(widget+0x7c)) {
            // Use the native array allocator and ownership bit. Its destructor
            // (002F6120 -> 00692010) then frees the slot with the matching heap.
            call(ctx,func_00692068,widget+0x74,UINT32_MAX,1,4);
        }
        const auto list=vm_read32(widget+0x7c);
        if (valid(list) && vm_read32(widget+0x74)) {
            vm_write32(list,vm_read32(bindings[widget]+0x250)); vm_write32(widget+0x78,1);
        }
    }
}
std::string props(const std::string& parent,int order,const std::string& pos="0.0, 0.0, 100.0, 7.5",const std::string& flags="k_visible|k_enabled|k_canHighlight") {
    return "parent = \""+parent+"\"\nsortorder = "+std::to_string(100+order)+"\npos [] { "+pos+
        " }\nflags = \""+flags+"\"\nplatforms = \"k_ps3\"\n"
        "bgColour [] {0.0,0.0,0.0,0.0}\nstyl_DC1 textcomp {stringid = \"STR_NULL\" fontsize = 0.65 text-v-align = \"k_middle\"}\n";
}
void hide_children(ppu_context* ctx,Page& page) {
    // Preserve original widgets for the existing Audio/profile transaction.
    // Only their visibility and input eligibility change while this page owns them.
    const auto stack=uint32_t(ctx->gpr[1])-0x5300;
    vm_write32(stack,0); vm_write32(stack+4,page.page+0x58);
    vm_write32(stack+8,vm_read32(page.page+0x60)); vm_write32(stack+12,page.context);
    for (;;) {
        const auto child=call(ctx,func_00311060,stack);
        if (!valid(child)) break;
        page.original_children.push_back({child,vm_read32(child+0x30)});
        vm_write32(child+0x30,vm_read32(child+0x30)&~3u);
    }
}
void restore_root_children(uint32_t page,const Cache& cache) {
    auto items=cache.root_children;
    for (const auto key:{"host","prompts"}) if (const auto found=cache.widgets.find(key);found!=cache.widgets.end())
        items.push_back(uint64_t(vm_read32(found->second+0x28))<<32);
    const auto list=vm_read32(page+0x64);
    if (!valid(list) || items.size()>vm_read32(page+0x5c)) return;
    for (size_t i=0;i<items.size();++i) vm_write64(list+uint32_t(i)*8,items[i]);
    vm_write32(page+0x60,uint32_t(items.size()));
}
void restore_children(ppu_context* ctx,bool hide_custom=true) {
    if (const auto found=caches.find(displayed.page);found!=caches.end()) {
        restore_root_children(displayed.page,found->second);
        if (hide_custom) if (const auto host=found->second.widgets.find("host");host!=found->second.widgets.end())
            vm_write32(host->second+0x30,vm_read32(host->second+0x30)&~3u);
        if (hide_custom) if (const auto prompts=found->second.widgets.find("prompts");prompts!=found->second.widgets.end())
            vm_write32(prompts->second+0x30,vm_read32(prompts->second+0x30)&~3u);
        const auto title=vm_read32(displayed.page+0x14c);
        if (valid(title)) {
            if (found->second.title_mode==1) label(ctx,displayed.page,found->second.title);
            else {
                if (vm_read32(title+0x1f8)) call(ctx,func_006C9518,vm_read32(title+0x1f4));
                vm_write32(title+0x1f4,found->second.title_id);
                vm_write32(title+0x1f8,0);
            }
            vm_write32(title+0x1fc,found->second.title_mode);
            vm_write32(title+0x14,(vm_read32(title+0x14)&~1u)|(found->second.title_flags&1u));
        }
    }
    for (const auto& [widget,flags]:displayed.original_children)
        if (valid(widget) && vm_read32(widget+0xcc)==displayed.context) vm_write32(widget+0x30,flags);
    if (valid(displayed.page) && displayed.original_events.size()<=vm_read32(displayed.page+0x74)) {
        const auto items=vm_read32(displayed.page+0x7c);
        if (valid(items)) {
            for (size_t i=0;i<displayed.original_events.size();++i) vm_write32(items+uint32_t(i)*4,displayed.original_events[i]);
            vm_write32(displayed.page+0x78,uint32_t(displayed.original_events.size()));
        }
    }
}
uint32_t cached(ppu_context* ctx,Cache& cache,const std::string& key,const std::string& type,const std::string& definition,bool root=false) {
    if (const auto found=cache.widgets.find(key);found!=cache.widgets.end()) { cache.used.insert(found->second); return found->second; }
    const auto widget=create(ctx,type,next_id(),definition);
    if (!valid(widget) || (root && !attach(ctx,widget))) return 0;
    cache.widgets[key]=widget;
    cache.used.insert(widget);
    return widget;
}
bool children(uint32_t parent,const std::vector<uint32_t>& rows) {
    const auto items=vm_read32(parent+0x64),capacity=vm_read32(parent+0x5c);
    if (!valid(items) || rows.size()>capacity) return false;
    // The native list walks its child handles from the end toward the start.
    for (size_t i=0;i<rows.size();++i) {
        vm_write32(items+uint32_t(i)*8,vm_read32(rows[rows.size()-1-i]+0x28));
        vm_write32(items+uint32_t(i)*8+4,0);
    }
    vm_write32(parent+0x60,uint32_t(rows.size()));
    return true;
}
bool build(ppu_context* ctx,const HazeMenuView& view) {
    if (!valid(target_page) || !valid(target_context) || !register_callback(ctx)) return false;
    auto& cache=caches[target_page];
    if (cache.context!=target_context || cache.handle!=vm_read32(target_page+0x28) || cache.name!=id(target_page)) {
        for (const auto& [key,widget]:cache.widgets) { bindings.erase(widget); actions.erase(widget); popup_panels.erase(widget); }
        cache={}; cache.context=target_context; cache.handle=vm_read32(target_page+0x28); cache.name=id(target_page);
        const auto child_count=vm_read32(target_page+0x60),child_items=vm_read32(target_page+0x64);
        if (child_count>512 || child_count>vm_read32(target_page+0x5c) || (child_count && !valid(child_items))) return false;
        for (uint32_t i=0;i<child_count;++i) cache.root_children.push_back(vm_read64(child_items+i*8));
        const auto title=vm_read32(target_page+0x14c);
        if (valid(title)) {
            cache.title_mode=vm_read32(title+0x1fc);
            cache.title_flags=vm_read32(title+0x14);
            const auto raw=vm_read32(title+0x1f4);
            cache.title_id=raw;
            if (cache.title_mode==1 && valid(raw)) for (unsigned i=0;i<4095;++i) {
                const auto ch=char(vm_read8(raw+i)); if (!ch) break; cache.title+=ch;
            }
        }
        Page original; original.page=target_page; original.context=target_context; hide_children(ctx,original);
        cache.originals=std::move(original.original_children);
        const auto count=vm_read32(target_page+0x78),items=vm_read32(target_page+0x7c);
        if (count>128 || (count && !valid(items))) return false;
        for (uint32_t i=0;i<count;++i) cache.events.push_back(vm_read32(items+i*4));
        bindings.erase(target_page);
    }
    if (view.generation!=displayed.generation) { section=0; choice_section=0; choice_key.clear(); }
    if (view.modal.revision) choice_key.clear();
    if (view.generation!=displayed.generation || view.modal.revision!=displayed.modal) modal_section=0;
    for (const auto& [widget,flags]:cache.originals) vm_write32(widget+0x30,flags&~3u);
    cache.used.clear();
    Page next; next.page=target_page; next.context=target_context;
    next.generation=view.generation; next.modal=view.modal.revision;
    next.original_children=cache.originals; next.original_events=cache.events;
    vm_write32(target_page+0x78,0);
    Action back{view.generation,HazeMenuCommandKind::Back,{},0,view.modal.revision};
    if (!choice_key.empty()) back.local=3;
    bind(ctx,target_page,back,"onPageBack");
    const auto host=cached(ctx,cache,"host","container",
        props(id(target_page),50,"0.0, 0.0, 100.0, 100.0","k_visible|k_enabled")+
        "style = \"\"\ntexture-colour [] {0.0,0.0,0.0,0.0}\n",true);
    if (!host) return false;
    vm_write32(host+0x30,vm_read32(host+0x30)|3u);
    for (unsigned i=0;i<2;++i) {
        const auto column=cached(ctx,cache,"column"+std::to_string(i),"listcontainer",
            props(id(host),50-int(i),i ? "53.0, 0.0, 47.0, 60.0" : "0.0, 0.0, 47.0, 60.0","k_visible|k_enabled")+
            "texture-colour [] {0.0,0.0,0.0,0.0}\n",true);
        if (!column) return false;
        vm_write32(column+0x30,vm_read32(column+0x30)|3u);
        next.columns.push_back(column);
    }
    next.container=next.columns[0];
    std::vector<uint32_t> lists[2];
    unsigned slot=0; uint32_t first=0;
    auto text=[&](const std::string& caption,Action action) {
        if (slot>=16) return uint32_t(0);
        const auto column=slot/8;
        const auto widget=cached(ctx,cache,"text"+std::to_string(slot),"text",props(id(next.columns[column]),int(slot)));
        if (!widget) return uint32_t(0);
        vm_write32(widget+0x30,(vm_read32(widget+0x30)|3u)&~0x800u);
        label(ctx,widget,caption); bind(ctx,widget,std::move(action)); lists[column].push_back(widget);
        ++slot; if (!first) first=widget; return widget;
    };
    if (view.modal.revision) {
        label(ctx,target_page,view.title);
        const auto panel=cached(ctx,cache,"popup-panel","container",
            props(id(host),32,"15.0, 12.0, 70.0, 46.0","k_visible|k_enabled")+
            "texture-colour [] {0.0,0.0,0.0,0.96}\n",true);
        if (!panel) return false;
        popup_panels.insert(panel);
        vm_write32(panel+0x30,vm_read32(panel+0x30)|3u);
        const auto title=cached(ctx,cache,"popup-title","text",props(id(panel),31,"5.0, 3.0, 90.0, 15.0","k_visible|k_enabled"),true);
        if (!title) return false;
        vm_write32(title+0x30,vm_read32(title+0x30)|3u);
        const auto pages=modal_pages(view.modal.message);
        modal_section=std::min(modal_section,pages.size()-1);
        label(ctx,title,view.modal.title+(pages.size()>1 ? "  "+std::to_string(modal_section+1)+" / "+std::to_string(pages.size()) : ""));
        next.message=cached(ctx,cache,"message","text",props(id(panel),30,"5.0, 20.0, 90.0, 32.0","k_visible|k_enabled"),true);
        if (!next.message) return false;
        vm_write32(next.message+0x30,vm_read32(next.message+0x30)|3u);
        label(ctx,next.message,pages[modal_section]);
        // Modal options use a dedicated list below the message.
        const auto options=cached(ctx,cache,"modal-options","listcontainer",
            props(id(panel),29,"5.0, 61.0, 90.0, 35.0","k_visible|k_enabled")+
            "texture-colour [] {0.0,0.0,0.0,0.0}\n",true);
        if (!options || view.modal.choices.size()>8) return false;
        std::vector<uint32_t> choices;
        auto option=[&](const std::string& caption,Action action,bool selected=false) {
            const auto index=choices.size();
            const auto widget=cached(ctx,cache,"modal-choice"+std::to_string(index),"text",props(id(options),int(index),"0.0, 0.0, 100.0, 25.0"));
            if (!widget) return false;
            vm_write32(widget+0x30,(vm_read32(widget+0x30)|3u)&~0x800u);
            label(ctx,widget,caption); bind(ctx,widget,std::move(action));
            choices.push_back(widget); if (!first || selected) first=widget;
            return true;
        };
        const int cancel=view.modal.cancel_choice>=0 ? view.modal.cancel_choice:int(view.modal.choices.size())-1;
        for (size_t i=0;i<view.modal.choices.size();++i) {
            // Confirmation stays on the final message page. Cancel is always
            // available and selected, including during paginated save failures.
            if (modal_section+1<pages.size() && int(i)!=cancel) continue;
            if (!option(view.modal.choices[i],{view.generation,HazeMenuCommandKind::Modal,{},int(i),view.modal.revision},int(i)==cancel)) return false;
        }
        if (modal_section && !option("Previous",{view.generation,HazeMenuCommandKind::Modal,{},0,view.modal.revision,6})) return false;
        if (modal_section+1<pages.size() && !option("Next",{view.generation,HazeMenuCommandKind::Modal,{},0,view.modal.revision,7})) return false;
        if (choices.size()>4) return false;
        if (!children(options,choices)) return false;
        vm_write32(options+0x30,vm_read32(options+0x30)|3u);
        next.columns.push_back(options); next.container=options;
    } else if (!choice_key.empty()) {
        const auto row=std::find_if(view.rows.begin(),view.rows.end(),[](const auto& r){return r.key==choice_key;});
        if (row==view.rows.end() || row->choices.empty()) { choice_key.clear(); refresh=true; return false; }
        label(ctx,target_page,row->label);
        const size_t pages=(row->choices.size()+12)/13;
        choice_section=std::min(choice_section,pages-1);
        const auto end=std::min(row->choices.size(),(choice_section+1)*13);
        for (size_t i=choice_section*13;i<end;++i) {
            Action action{view.generation,HazeMenuCommandKind::Change,row->key}; action.value=row->choices[i].value;
            const auto widget=text(row->choices[i].label,action);
            if (row->choices[i].value==row->value) first=widget;
        }
        if (choice_section) text("Previous",{view.generation,HazeMenuCommandKind::Back,{},0,0,-1});
        if (choice_section+1<pages) text("Next",{view.generation,HazeMenuCommandKind::Back,{},0,0,1});
        text("Back",back);
    } else {
        const bool paged=view.rows.size()>13;
        const size_t page_size=paged ? 10:13,pages=std::max(size_t(1),(view.rows.size()+page_size-1)/page_size);
        section=std::min(section,pages-1);
        label(ctx,target_page,view.title+(paged ? "  "+std::to_string(section+1)+" / "+std::to_string(pages) : ""));
        const auto end=std::min(view.rows.size(),(section+1)*page_size);
        for (size_t index=section*page_size;index<end;++index) {
            const auto& row=view.rows[index];
            Row native; native.key=row.key; native.slider=row.slider;
            native.toggle=row.choices.size()==2 && row.choices[0].label=="Off" && row.choices[1].label=="On";
            const auto column=slot/8;
            auto definition=props(id(next.columns[column]),int(slot));
            std::string type="text";
            if (native.slider) {
                type="simplesliderforleft";
                const int steps=row.step>0 ? std::max(1,int(std::lround((row.maximum-row.minimum)/row.step))) : 100;
                definition+="captionWidth = 0.7\nvalueWidth = 0.0\nmaxValue = "+std::to_string(steps)+"\nstepValue = 1\ndefaultValue = 0\n";
            } else if (native.toggle) {
                type="togglewithforleft";
                definition+="fixedDataWidth = \"true\"\ndataWidth = 0.4\ntoggleOptions {"
                    "item_0 {stringID = \"STR_OFF\" toggleValue = 0 associatedString = \"\"}"
                    "item_1 {stringID = \"STR_ON\" toggleValue = 1 associatedString = \"\"}}\n";
            }
            native.widget=cached(ctx,cache,type+std::to_string(slot)+(native.slider ? row.key:""),type,definition);
            if (!native.widget) return false;
            vm_write32(native.widget+0x30,(vm_read32(native.widget+0x30)|3u)&~0x800u);
            if (!first) first=native.widget;
            if (!native.slider && !native.toggle)
                bind(ctx,native.widget,{view.generation,HazeMenuCommandKind::Activate,row.key,0,0,row.choices.empty() ? 0:2});
            lists[column].push_back(native.widget); ++slot;
            next.rows.push_back(std::move(native));
        }
        if (section) text("Previous",{view.generation,HazeMenuCommandKind::Back,{},0,0,-1});
        if (section+1<pages) text("Next",{view.generation,HazeMenuCommandKind::Back,{},0,0,1});
        const HazeMenuCommandKind kinds[]={HazeMenuCommandKind::Apply,HazeMenuCommandKind::Defaults,HazeMenuCommandKind::Back};
        for (unsigned i=0;i<3;++i) text(view.actions[i],{view.generation,kinds[i]});
    }
    for (unsigned i=0;i<2;++i) if (!children(next.columns[i],lists[i])) return false;
    next.notice=cached(ctx,cache,"notice","text",props(id(host),20,"0.0, 40.0, 100.0, 20.0","k_visible|k_enabled"),true);
    if (!next.notice) return false;
    vm_write32(next.notice+0x30,(vm_read32(next.notice+0x30)&~3u)|(view.modal.revision ? 0u:3u));
    const auto prompts=cached(ctx,cache,"prompts","buttonpromptfeflg5",
        "parent = \""+id(target_page)+"\"\nflags = \"k_visible|k_enabled\"\n"
        "bgColour [] {1.0,1.0,1.0,0.0}\nbuttons {}\nshowFriendPrompt = \"false\"\n",true);
    if (!prompts) return false;
    vm_write32(prompts+0x30,vm_read32(prompts+0x30)|3u);
    // Hidden retail children still update: their prompt bar can replace ours
    // and their animated panels can become visible again. Temporarily detach
    // them from traversal, retaining registered widgets for Audio transactions.
    if (!children(target_page,{host,prompts})) return false;
    for (const auto& [key,widget]:cache.widgets) {
        if (!cache.used.count(widget)) vm_write32(widget+0x30,vm_read32(widget+0x30)&~3u);
        const auto found=bindings.find(widget);
        if (found!=bindings.end() && valid(found->second) && !vm_read32(found->second+0x348))
            call(ctx,func_002E8200,found->second);
    }
    call(ctx,func_00319840,target_context,next.container);
    if (first) call(ctx,func_002EF780,first);
    displayed=std::move(next); refresh=false;
    haze_menu_presented(view.generation);
    std::fprintf(stderr,"[native-menu] View %s: section=%zu widgets=%zu modal=%u\n",view.title.c_str(),section,cache.widgets.size(),view.modal.revision);
    return true;
}
void sync(ppu_context* ctx,const HazeMenuView& view) {
    for (auto& native:displayed.rows) {
        const auto row=std::find_if(view.rows.begin(),view.rows.end(),[&](const auto& r){return r.key==native.key;});
        if (row==view.rows.end()) continue;
        if (native.slider || native.toggle) {
            int expected=0;
            const int steps=row->step>0 ? std::max(1,int(std::lround((row->maximum-row->minimum)/row->step))) : 100;
            if (native.slider && row->maximum>row->minimum)
                expected=int(std::lround((std::strtod(row->value.c_str(),nullptr)-row->minimum)*steps/(row->maximum-row->minimum)));
            if (native.toggle) expected=row->value==row->choices[1].value ? 1:0;
            const auto field=native.widget+(native.slider ? 0x154:0x170);
            const int actual=int(vm_read32(field));
            if (displayed.revision && actual!=native.last_value) {
                std::string value;
                if (native.toggle && actual>=0 && actual<2) value=row->choices[size_t(actual)].value;
                else if (native.slider) {
                    double amount=row->minimum+std::clamp(actual,0,steps)*(row->maximum-row->minimum)/steps;
                    if (row->step>0) amount=row->minimum+std::round((amount-row->minimum)/row->step)*row->step;
                    char number[48]; std::snprintf(number,sizeof(number),"%.8g",std::clamp(amount,row->minimum,row->maximum)); value=number;
                }
                if (!value.empty()) haze_menu_command({view.generation,HazeMenuCommandKind::Change,row->key,value});
                native.last_value=actual;
            } else if (row->value!=native.model_value || !displayed.revision) {
                vm_write32(field,uint32_t(expected)); native.last_value=expected;
            }
        }
        native.model_value=row->value;
        std::string caption=row->label;
        if (native.slider) caption+="  "+row->value;
        if (!native.slider && !native.toggle && !row->value.empty()) {
            std::string value=row->value;
            for (const auto& choice:row->choices) if (choice.value==row->value) value=choice.label;
            caption+="    "+value;
        }
        if (caption!=native.label) { label(ctx,native.widget,caption); native.label=caption; }
    }
    displayed.revision=view.revision;
}
}
void haze_native_popup_material(ppu_context* ctx,uint32_t widget) {
    // The original nine-slice owns corner coverage. Fill its interior using
    // Haze's own textured-quad renderer, including the widget's live opacity.
    // Procedural horizontal grain needs no copied game asset or host overlay.
    if (!valid(widget) || (vm_read32(widget)!=0xde5ca8 && !popup_panels.count(widget))) return;
    const auto style=vm_read32(widget+0xdc);
    if (!valid(style) || ctx->gpr[1]<0x20000) return;
    const auto saved=*ctx;
    const auto scratch=uint32_t(ctx->gpr[1])-0x5400;
    call(ctx,func_002F2978,widget,scratch);
    auto number=[](uint32_t address) {
        const auto bits=vm_read32(address); float value; std::memcpy(&value,&bits,4); return value;
    };
    const float left=number(scratch),top=number(scratch+4),right=number(scratch+8),bottom=number(scratch+12);
    const float alpha=number(widget+0x100)*number(widget+0xec);
    if (!std::isfinite(left+top+right+bottom+alpha) || right<=left || bottom<=top || alpha<=0) return;
    call(ctx,func_006F16D0,vm_read32(style+0x4a8));
    constexpr int lines=192,segments=4;
    const float inset=.006f,x0=left+inset,x1=right-inset,y0=top+inset,y1=bottom-inset;
    for (int y=0;y<lines;++y) {
        const float t=float(y)/lines;
        uint32_t noise=uint32_t(y+1)*747796405u+2891336453u;
        noise=((noise>>((noise>>28)+4))^noise)*277803737u;
        const float grain=(float((noise^(noise>>22))&255)/255.f-.5f)*.025f;
        for (int x=0;x<segments;++x) {
            const float sheen=.035f*std::sin(t*3.14159265f);
            const float streak=float((noise>>((x*5)%24))&31)/31.f*.003f;
            const float shade=.13f+.045f*(1-t)+sheen+grain+streak;
            ctx->fpr[1]=x0+(x1-x0)*x/segments;
            ctx->fpr[2]=y0+(y1-y0)*y/lines;
            ctx->fpr[3]=x0+(x1-x0)*(x+1)/segments;
            ctx->fpr[4]=y0+(y1-y0)*(y+1)/lines;
            ctx->fpr[5]=0.;
            ctx->fpr[6]=ctx->fpr[7]=0.; ctx->fpr[8]=ctx->fpr[9]=1.;
            ctx->fpr[10]=shade; ctx->fpr[11]=shade; ctx->fpr[12]=shade;
            ctx->fpr[13]=std::clamp(alpha,0.f,1.f);
            call(ctx,func_006F5768);
        }
    }
    *ctx=saved;
}
void haze_native_menu_page(ppu_context* ctx,uint32_t page) {
    if (!valid(page)) return;
    const auto name=id(page);
    const bool main=name=="fefl_KS_page2",pause=name=="ingm_DC_page1";
    const auto manager=vm_read32(0xf48dd4);
    const auto context=valid(manager) ? vm_read32(manager+0x48):0;
    if (displayed.generation && (page!=displayed.page || context!=displayed.context)) {
        // The previous context may already have been destroyed. Only touch its
        // widgets while the manager still owns that same context and page ID.
        if (context==displayed.context && valid(displayed.page)) restore_children(ctx);
        haze_menu_command({displayed.generation,HazeMenuCommandKind::Departed});
        displayed={};
    }
    // Remember every activated native page, not just settings: window-close
    // status must also be renderable from campaign/loading/gameplay contexts.
    target_page=page; target_context=context;
    if ((!main && !pause) || !valid(target_context) || !register_callback(ctx)) return;
    if (main_handles[page]==vm_read32(page+0x28)) return;
    // The main list is a native resource-owned container. Insert the new handles
    // at its beginning because the list renders handles in reverse order.
    const std::string parent=main ? "fefl_DC_cntr2" : "ingm_DC_cntr2";
    for (unsigned i=0;i<(main ? 2u:1u);++i) {
        const auto widget=create(ctx,"text",next_id(),props(parent,int(i)));
        if (!widget || !attach(ctx,widget)) return;
        const auto container=call(ctx,func_003141D0,widget+0x40,target_context);
        const auto count=vm_read32(container+0x60),items=vm_read32(container+0x64);
        const auto last=vm_read64(items+(count-1)*8);
        for (uint32_t j=count-1;j>0;--j) vm_write64(items+j*8,vm_read64(items+(j-1)*8));
        vm_write64(items,last);
        label(ctx,widget,i ? "Add to Steam" : "Quit to Desktop");
        main_roots[widget]=page;
        bind(ctx,widget,{0,HazeMenuCommandKind::Activate,{},0,0,int(i)+4});
    }
    main_handles[page]=vm_read32(page+0x28);
}
void haze_native_menu_context_reset(uint32_t context) {
    // 00317300 deletes the context's root widgets recursively. Forget host
    // identities before the game can recycle the same addresses and handles.
    for (auto it=caches.begin();it!=caches.end();) {
        if (it->second.context!=context) { ++it; continue; }
        // Restore ownership before the guest recursively destroys its roots.
        restore_root_children(it->first,it->second);
        for (const auto& [key,widget]:it->second.widgets) {
            bindings.erase(widget); actions.erase(widget); popup_panels.erase(widget);
        }
        bindings.erase(it->first); actions.erase(it->first);
        it=caches.erase(it);
    }
    for (auto it=main_roots.begin();it!=main_roots.end();) {
        if (valid(it->first) && vm_read32(it->first+0xcc)==context) {
            main_handles.erase(it->second); bindings.erase(it->first); actions.erase(it->first);
            it=main_roots.erase(it);
        } else ++it;
    }
    if (displayed.context==context) {
        if (displayed.generation) haze_menu_command({displayed.generation,HazeMenuCommandKind::Departed});
        displayed={};
    }
    if (target_context==context) { target_page=0; target_context=0; return_widget=0; }
}
void haze_native_menu_tick(ppu_context* ctx) {
    const auto manager=vm_read32(0xf48dd4);
    if (!valid(manager) || !valid(target_page)) return;
    if (vm_read32(manager+0x48)!=target_context) return;
    const auto view=haze_menu_view();
    const auto count=vm_read32(target_context+0x50),stack=vm_read32(target_context+0x54);
    if (!count || count>64 || !valid(stack)) return;
    char top[17]{};
    for (unsigned i=0;i<16;++i) top[i]=char(vm_read8(stack+(count-1)*16+i));
    if (id(target_page)!=top) {
        if (displayed.generation) haze_menu_command({displayed.generation,HazeMenuCommandKind::Departed});
        restore_children(ctx); displayed={}; target_page=0; return;
    }
    if (view.page==HazeMenuPage::None) {
        if (displayed.generation && view.generation==displayed.generation && !view.pop) {
            restore_children(ctx);
            const auto origin=main_roots.find(return_widget);
            const auto selected=origin!=main_roots.end() && origin->second==target_page &&
                valid(return_widget) && vm_read32(return_widget+0xcc)==target_context ? return_widget:target_page;
            call(ctx,func_00319840,target_context,selected);
            if (selected!=target_page) call(ctx,func_002EF780,selected);
            displayed={}; return;
        }
        if (displayed.generation && view.generation==displayed.generation && view.pop) {
            // The popped native page owns its visibility. Keep its pooled
            // children's layout state intact for the next activation.
            restore_children(ctx,false); call(ctx,func_0031B7F8,target_context,0);
            displayed={}; target_page=0;
        }
        return;
    }
    const auto name=id(target_page);
    if ((view.page==HazeMenuPage::Video && name!="sett_DC_page10") ||
        (view.page==HazeMenuPage::Audio && name!="sett_DC_page3") ||
        (view.page==HazeMenuPage::Controls && name!="sett_DC_page2")) return;
    if (view.generation!=displayed.generation || view.modal.revision!=displayed.modal || refresh) {
        if (!build(ctx,view)) {
            // Construction is fail-closed: release provider input ownership and
            // restore the retail page instead of retrying an unsupported layout
            // every frame and leaving a blank, input-blocked screen.
            if (const auto found=caches.find(target_page);found!=caches.end()) {
                displayed={}; displayed.page=target_page; displayed.context=target_context;
                displayed.original_children=found->second.originals;
                displayed.original_events=found->second.events;
                restore_children(ctx);
            }
            haze_menu_command({view.generation,HazeMenuCommandKind::Departed});
            displayed={}; target_page=0;
            std::fprintf(stderr,"[native-menu] Widget construction failed; restored the retail page.\n");
            return;
        }
    }
    if (displayed.revision!=view.revision) {
        if (view.modal.revision) {
            const auto pages=modal_pages(view.modal.message);
            label(ctx,displayed.message,pages[std::min(modal_section,pages.size()-1)]);
        }
    }
    std::string status=view.notice;
    if (!view.modal.revision && choice_key.empty()) for (const auto& native:displayed.rows) {
        if (!(vm_read32(native.widget+0x30)&0x800)) continue;
        const auto row=std::find_if(view.rows.begin(),view.rows.end(),[&](const auto& r){return r.key==native.key;});
        if (row!=view.rows.end() && !row->hint.empty()) { if (!status.empty()) status+="\n"; status+=row->hint; }
        break;
    }
    if (status!=displayed.status) {
        label(ctx,displayed.notice,wrap(status,85)); displayed.status=status;
    }
    if (!view.modal.revision) sync(ctx,view);
    else displayed.revision=view.revision;
}
