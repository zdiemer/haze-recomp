#include "haze_controls_settings.h"
#include "haze_input_key.h"
#include "haze_input_source.h"
#include "haze_input.h"
#include "haze_menu.h"
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace {
HazeControlDocument document;
HazeControlValues edited;
std::string error,capture_id,capture_label,candidate;
std::vector<std::string> conflicts;
HazeMenuModal dialog;
unsigned revision=0;
bool ready=false,keyboard=false,keyboard_neutral=false;
enum class Stage { None, Capture, Preview, Conflict, Mapping } stage=Stage::None;
std::string binding_label(const std::string& source,bool key) {
    if (!_stricmp(source.c_str(),"none")) return "Unbound";
    if (key) { const auto code=haze_key_code(source.c_str()); return code>=0 ? haze_key_name(code) : source; }
    const auto raw=haze_raw_source(source);
    if (raw.kind==HazeRawSource::Button) return "Button "+std::to_string(raw.index+1);
    if (raw.kind==HazeRawSource::Axis) return "Axis "+std::to_string(raw.index+1)+(raw.sign>0 ? " +" : raw.sign<0 ? " -" : "");
    if (raw.kind==HazeRawSource::Hat) return "Hat "+std::to_string(raw.index+1)+" "+(raw.mask==1 ? "Up" : raw.mask==2 ? "Right" : raw.mask==4 ? "Down" : "Left");
    std::string name=source,suffix;
    if (!name.empty() && (name[0]=='-' || name[0]=='+')) { suffix=name[0]=='-' ? " (-)" : " (+)"; name.erase(0,1); }
    const std::pair<const char*,const char*> labels[]={
        {"south","South face button"},{"east","East face button"},{"west","West face button"},{"north","North face button"},
        {"a","South face button"},{"b","East face button"},{"x","West face button"},{"y","North face button"},
        {"leftx","Left stick X"},{"lefty","Left stick Y"},{"rightx","Right stick X"},{"righty","Right stick Y"},
        {"leftshoulder","Left shoulder"},{"rightshoulder","Right shoulder"},{"lefttrigger","Left trigger"},{"righttrigger","Right trigger"},
        {"leftstick","Left stick press"},{"rightstick","Right stick press"},{"start","Start / Menu"},{"back","Back / View"},
        {"dpup","D-pad Up"},{"dpdown","D-pad Down"},{"dpleft","D-pad Left"},{"dpright","D-pad Right"}};
    for (const auto& [from,to]:labels) if (!_stricmp(name.c_str(),from)) return std::string(to)+suffix;
    return source;
}
void close_capture() {
    haze_gamepad_capture_cancel(); dialog={}; stage=Stage::None; capture_id.clear(); candidate.clear();
}
void preview(const std::string& binding) {
    candidate=binding; stage=Stage::Preview; haze_gamepad_capture_cancel();
    dialog={++revision,"Use this binding?",binding_label(binding,keyboard)+" will be staged for "+capture_label+". Apply saves it.",{"Use input","Unbind","Cancel"},false};
}
void choose(int choice) {
    if (choice<0 || choice>=int(dialog.choices.size())) return;
    if (choice==int(dialog.choices.size())-1) { close_capture(); return; }
    if (stage==Stage::Mapping) {
        const auto id=capture_id; close_capture();
        if (choice==1) { edited[id].clear(); return; }
        char file[MAX_PATH]{};
        OPENFILENAMEA open{}; open.lStructSize=sizeof(open); open.hwndOwner=GetActiveWindow();
        open.lpstrFilter="SDL mapping files\0*.txt;*.db\0All files\0*.*\0";
        open.lpstrFile=file; open.nMaxFile=MAX_PATH; open.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&open)) edited[id]=file;
        return;
    }
    if (stage==Stage::Capture || (stage==Stage::Preview && choice==1)) {
        edited[capture_id]="None"; close_capture(); return;
    }
    if (stage==Stage::Preview) {
        conflicts=haze_control_conflicts(edited,capture_id,candidate);
        if (!conflicts.empty()) {
            std::string text=binding_label(candidate,keyboard)+" is also used by:";
            for (const auto& id:conflicts) for (const auto& spec:haze_control_specs()) if (id==spec.id()) text+="\n"+spec.label;
            text+="\nKeep both actions, or unbind the other actions?";
            dialog={++revision,"Binding conflict",text,{"Keep both","Replace others","Cancel"},false}; stage=Stage::Conflict; return;
        }
    }
    if (stage==Stage::Conflict && choice==1) for (const auto& id:conflicts) edited[id]="None";
    edited[capture_id]=candidate; close_capture();
}
void activate(const std::string& id) {
    if (id=="game-options") { close_capture(); haze_menu_retail(); return; }
    if (!ready) return;
    const auto& specs=haze_control_specs();
    const auto spec=std::find_if(specs.begin(),specs.end(),[&](const auto& item) { return item.id()==id; });
    if (spec==specs.end()) return;
    if (spec->kind==HazeControlKind::Mapping) {
        capture_id=id; stage=Stage::Mapping;
        dialog={++revision,"SDL controller mapping","Choose an SDL gamecontrollerdb text file for additional controllers. Apply saves the selection. Clearing the file needs a restart to remove mappings SDL already loaded.",{"Choose file","Clear","Cancel"},false};
        return;
    }
    capture_id=id; capture_label=spec->label; candidate.clear(); error.clear(); stage=Stage::Capture;
    keyboard=spec->kind==HazeControlKind::Key; keyboard_neutral=false;
    dialog={++revision,keyboard ? "Bind keyboard / mouse" : "Bind controller",
        "Release held inputs, then press the input to preview. Escape cancels.",{"Unbind","Cancel"},true};
    if (!keyboard) haze_gamepad_capture_begin(spec->kind==HazeControlKind::Axis);
}
void capture_key(int key) {
    if (stage!=Stage::Capture || !keyboard || !keyboard_neutral) return;
    const auto name=haze_key_name(key);
    if (name.empty()) {
        const char* reserved=haze_key_reserved(key);
        dialog.message=std::string(reserved ? reserved : "That key is unsupported")+". Choose another key; Escape cancels."; return;
    }
    haze_input_menu_button(key); preview(name);
}
void update(bool focused) {
    if (stage==Stage::None) return;
    if (!focused) { close_capture(); error="Capture cancelled when the game lost focus. Binding unchanged."; return; }
    if (stage!=Stage::Capture) return;
    if (keyboard) {
        if (!keyboard_neutral) {
            bool held=false;
            // Wait for inputs this dialog can capture. Unsupported lock keys
            // can report held indefinitely and must not block rebinding.
            for (int key=1;key<256;++key)
                if (!haze_key_name(key).empty() && (GetAsyncKeyState(key)&0x8000)) held=true;
            if (!held) { keyboard_neutral=true; dialog.message="Press a key or mouse button to preview. Escape cancels."; }
        }
        return;
    }
    const auto capture=haze_gamepad_capture(); dialog.message=capture.message;
    if (capture.phase==HazeBindingCapturePhase::Captured) preview(capture.source);
    else if (capture.phase==HazeBindingCapturePhase::Cancelled) { close_capture(); error=capture.message; }
}
}
void haze_controls_menu_register() {
    HazeMenuProvider page; page.title="CONTROLS";
    page.begin=[] { close_capture(); error.clear(); ready=haze_control_load(haze_input_config_path(),document,error); edited=ready ? document.values : haze_control_defaults(); };
    page.defaults=[] { edited=haze_control_defaults(); error.clear(); };
    page.rows=[] {
        std::vector<HazeMenuRow> rows;
        const auto device=haze_gamepad_info();
        const std::string status=device.name+(device.connected && !device.mapped ? " (unrecognized; choose an SDL mapping file or bind raw inputs)" : "");
        for (const auto& spec:haze_control_specs()) {
            HazeMenuRow row;
            row.key=spec.id(); row.value=edited[spec.id()]; row.label=spec.label;
            row.hint=error.empty() ? status+". Action names assume the Default game layout. Apply saves; Back discards edits." : error;
            switch (spec.kind) {
            case HazeControlKind::Key: row.label="Keyboard / mouse: "+spec.label; row.action=true; break;
            case HazeControlKind::Button: row.label="Controller: "+spec.label; row.action=true; break;
            case HazeControlKind::Axis: row.label="Controller axis: "+spec.label; row.action=true; break;
            case HazeControlKind::Number: row.slider=true; row.minimum=spec.minimum; row.maximum=spec.maximum; row.step=spec.step; break;
            case HazeControlKind::Toggle: row.choices={{"0","Off"},{"1","On"}}; break;
            case HazeControlKind::Family: row.choices={{"auto","Automatic"},{"xbox","Xbox"},{"playstation","PlayStation"},{"switch","Switch"}}; break;
            case HazeControlKind::Mapping:
                row.action=true; if (row.value.empty()) row.value="Choose file...";
                row.hint="Load an SDL gamecontrollerdb mapping file for unrecognized devices. "+status; break;
            }
            if (spec.kind==HazeControlKind::Key || spec.kind==HazeControlKind::Button || spec.kind==HazeControlKind::Axis) {
                row.prompt_binding=edited[spec.id()];
                row.prompt_family=spec.kind==HazeControlKind::Key ? HazeInputFamily::Keyboard : device.family;
                row.value=binding_label(row.prompt_binding,spec.kind==HazeControlKind::Key);
            }
            rows.push_back(std::move(row));
        }
        rows.insert(rows.begin()+4,{
            {"mouse-look-x","Mouse: Look horizontally","Mouse X","Mouse movement rotates the camera. Adjust sensitivity below."},
            {"mouse-look-y","Mouse: Look vertically","Mouse Y","Mouse movement rotates the camera. Adjust sensitivity or Invert mouse look below."}});
        rows.push_back({"game-options","Game control options","Open",
            "Discard unapplied PC edits and open the game's controller layout, toggle aim/crouch and other control options.",{},false,0,1,.05,true});
        return rows;
    };
    page.change=[](const std::string& key,const std::string& value) {
        auto next=edited; next[key]=value; std::string problem;
        if (haze_control_validate(next,problem)) { edited=std::move(next); error.clear(); }
        else error=problem;
    };
    page.apply=[](std::string& message) {
        if (!ready) { message=error.empty() ? "Reopen Controls to read the current input settings." : error; return HazeMenuApply::Failed; }
        const auto mapping=edited.at("Controller.MappingFile");
        if (!mapping.empty()) {
            std::ifstream file(std::filesystem::path(document.path).parent_path()/mapping);
            if (!file) { message="The selected SDL mapping file cannot be opened. Previous controls retained."; return HazeMenuApply::Failed; }
            std::string line; bool mapping_found=false; size_t bytes=0;
            while (std::getline(file,line)) {
                bytes+=line.size(); if (bytes>8*1024*1024) break;
                if (line.starts_with("\xef\xbb\xbf")) line.erase(0,3);
                const auto first=line.find_first_not_of(" \t\r");
                if (first==std::string::npos) continue;
                line.erase(0,first);
                if (line.empty() || line[0]=='#' || line[0]=='\r') continue;
                const bool guid=line.size()>34 && line[32]==',' &&
                    std::all_of(line.begin(),line.begin()+32,[](unsigned char c) { return std::isxdigit(c)!=0; });
                if (!guid || line.find(',',33)==std::string::npos) { mapping_found=false; break; }
                mapping_found=true;
            }
            if (!mapping_found || file.bad() || bytes>8*1024*1024) {
                message="Choose a valid SDL gamecontrollerdb text file. Previous controls retained."; return HazeMenuApply::Failed;
            }
        }
        const bool mapping_restart=!document.values.at("Controller.MappingFile").empty() && mapping!=document.values.at("Controller.MappingFile");
        if (!haze_control_save(document,edited,message)) return HazeMenuApply::Failed;
        haze_input_reload(); message=mapping_restart ? "Controls applied. Restart to discard previously loaded SDL mappings." : "Controls saved and applied.";
        return HazeMenuApply::Applied;
    };
    page.activate=[](const std::string& id) { activate(id); }; page.update=update; page.modal=[] { return dialog; };
    page.modal_key=capture_key; page.modal_choose=choose;
    haze_menu_register(HazeMenuPage::Controls,std::move(page));
}
