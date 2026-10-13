#include "haze_controls_settings.h"
#include "haze_input_key.h"
#include "haze_input_source.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace {
std::string trim(std::string text) {
    const auto first=text.find_first_not_of(" \t\r"),last=text.find_last_not_of(" \t\r");
    return first==std::string::npos ? "" : text.substr(first,last-first+1);
}
std::string lower(std::string text) { for (auto& c:text) c=char(std::tolower((unsigned char)c)); return text; }
bool read(const std::string& path,std::string& contents,bool& exists,std::string& error) {
    const DWORD attributes=GetFileAttributesW(std::filesystem::path(path).c_str());
    exists=attributes!=INVALID_FILE_ATTRIBUTES;
    if (!exists && (GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND)) { contents.clear(); return true; }
    std::ifstream file(path,std::ios::binary);
    if (!file) { error="Cannot read the input settings file."; return false; }
    contents.assign(std::istreambuf_iterator<char>(file),{});
    if (file.bad() || contents.size()>1024*1024) { error="The input settings file cannot be read safely."; return false; }
    return true;
}
bool named_axis(std::string value) {
    if (!value.empty() && (value.front()=='-' || value.front()=='+')) value.erase(0,1);
    return value=="leftx" || value=="lefty" || value=="rightx" || value=="righty" || value=="lefttrigger" || value=="righttrigger";
}
bool button(const std::string& value) {
    if (named_axis(value) || haze_raw_source(value).kind!=HazeRawSource::None) return true;
    const char* names[]={"south","east","west","north","a","b","x","y","back","guide","start","leftstick","rightstick",
        "leftshoulder","rightshoulder","dpup","dpdown","dpleft","dpright","misc1","misc2","misc3","misc4","misc5","misc6",
        "paddle1","paddle2","paddle3","paddle4","touchpad"};
    return std::any_of(std::begin(names),std::end(names),[&](const char* name) { return value==name; });
}
}
const std::vector<HazeControlSpec>& haze_control_specs() {
    static const auto specs=[] {
        std::vector<HazeControlSpec> list;
        const char* movement[]={"Forward","Backward","StrafeLeft","StrafeRight"};
        const char* movement_labels[]={"Move forward","Move backward","Move left","Move right"};
        const char* keys[]={"W","S","A","D"};
        for (int i=0;i<4;++i) list.push_back({"Bindings",movement[i],movement_labels[i],keys[i],HazeControlKind::Key});
        const char* actions[]={"Cross","Circle","Square","Triangle","L1","R1","L2","R2","L3","R3","Start","Select","Up","Down","Left","Right"};
        const char* kb[]={"Space","X","E","R","G","Mouse1","Q","F","Ctrl","Mouse2","Enter","Tab","Up","Down","Left","Right"};
        const char* pad[]={"south","east","west","north","leftshoulder","rightshoulder","lefttrigger","righttrigger","leftstick","rightstick","start","back","dpup","dpdown","dpleft","dpright"};
        const char* labels[]={"Jump / Confirm (Cross)","Switch weapon / Back (Circle)","Use (Square)","Reload (Triangle)",
            "Throw grenade (L1)","Fire (R1)","Nectar / Play dead (L2)","Melee (R2)","Crouch (L3)","Zoom (R3)",
            "Start / Pause","Select","Menu up","Menu down","Menu left","Menu right"};
        for (int i=0;i<16;++i) list.push_back({"Bindings",actions[i],labels[i],kb[i],HazeControlKind::Key});
        for (int i=0;i<16;++i) list.push_back({"ControllerBindings",actions[i],labels[i],pad[i],HazeControlKind::Button});
        const char* axis_keys[]={"MoveX","MoveY","LookX","LookY"};
        const char* axes[]={"leftx","lefty","rightx","righty"};
        const char* axis_labels[]={"Move horizontally","Move vertically","Look horizontally","Look vertically"};
        for (int i=0;i<4;++i) list.push_back({"Controller",axis_keys[i],axis_labels[i],axes[i],HazeControlKind::Axis});
        list.push_back({"Mouse","Sensitivity","Mouse sensitivity","0.10",HazeControlKind::Number,.01,10,.01});
        list.push_back({"Mouse","InvertY","Invert mouse look","0",HazeControlKind::Toggle});
        list.push_back({"Controller","MoveDeadzone","Movement deadzone","0.15",HazeControlKind::Number,0,.94,.01});
        list.push_back({"Controller","LookDeadzone","Look deadzone","0.12",HazeControlKind::Number,0,.94,.01});
        list.push_back({"Controller","TriggerDeadzone","Trigger threshold","0.05",HazeControlKind::Number,0,.94,.01});
        list.push_back({"Controller","InvertLookY","Invert controller look","0",HazeControlKind::Toggle});
        list.push_back({"Controller","IconFamily","Controller prompts","auto",HazeControlKind::Family});
        list.push_back({"Controller","MappingFile","SDL mapping file","",HazeControlKind::Mapping});
        return list;
    }();
    return specs;
}
HazeControlValues haze_control_defaults() {
    HazeControlValues values; for (const auto& spec:haze_control_specs()) values[spec.id()]=spec.fallback; return values;
}
bool haze_control_validate(const HazeControlValues& values,std::string& error) {
    if (values.size()!=haze_control_specs().size()) { error="Incomplete controls settings."; return false; }
    for (const auto& spec:haze_control_specs()) {
        const auto found=values.find(spec.id());
        if (found==values.end()) { error="Missing control: "+spec.label; return false; }
        const auto& text=found->second; const auto value=lower(text); bool ok=false;
        switch (spec.kind) {
        case HazeControlKind::Key: ok=haze_key_code(text.c_str())>=0; break;
        case HazeControlKind::Button: ok=value=="none" || button(value); break;
        case HazeControlKind::Axis: ok=value=="none" || named_axis(value) || haze_raw_source(value).kind==HazeRawSource::Axis; break;
        case HazeControlKind::Toggle: ok=value=="0" || value=="1"; break;
        case HazeControlKind::Family: ok=value=="auto" || value=="xbox" || value=="playstation" || value=="switch"; break;
        case HazeControlKind::Number: {
            char* end=nullptr; double number=std::strtod(text.c_str(),&end);
            ok=end!=text.c_str() && !*end && std::isfinite(number) && number>=spec.minimum && number<=spec.maximum; break;
        }
        case HazeControlKind::Mapping: ok=text.size()<900 && text.find_first_of("\r\n\0",0,3)==std::string::npos; break;
        }
        if (!ok) { error="Invalid value for "+spec.label+"."; return false; }
    }
    return true;
}
bool haze_control_load(const std::string& path,HazeControlDocument& document,std::string& error) {
    HazeControlDocument next; next.path=path; next.values=haze_control_defaults();
    if (!read(path,next.original,next.existed,error)) return false;
    if (next.original.find('\0')!=std::string::npos || next.original.starts_with("\xff\xfe") || next.original.starts_with("\xfe\xff")) {
        error="Controls requires a text input.ini in UTF-8 or the current Windows code page."; return false;
    }
    std::map<std::string,std::string> ids;
    for (const auto& spec:haze_control_specs()) ids[lower(spec.id())]=spec.id();
    std::string source=next.original;
    if (source.starts_with("\xef\xbb\xbf")) source.erase(0,3);
    std::istringstream lines(source); std::string line,section; std::set<std::string> seen,sections;
    while (std::getline(lines,line)) {
        line=trim(line); if (line.empty() || line[0]==';' || line[0]=='#') continue;
        if (line[0]=='[' && line.back()==']') {
            section=lower(trim(line.substr(1,line.size()-2)));
            if ((section=="bindings" || section=="controllerbindings" || section=="mouse" || section=="controller") && !sections.insert(section).second) {
                error="Duplicate input settings section: "+section; return false;
            }
            continue;
        }
        const auto equal=line.find('='); if (equal==std::string::npos) continue;
        const auto found=ids.find(section+"."+lower(trim(line.substr(0,equal))));
        if (found==ids.end()) continue;
        if (!seen.insert(found->second).second) { error="Duplicate control setting: "+found->second; return false; }
        next.values[found->second]=trim(line.substr(equal+1));
    }
    if (!haze_control_validate(next.values,error)) return false;
    document=std::move(next); return true;
}
bool haze_control_save(HazeControlDocument& document,const HazeControlValues& values,std::string& error) {
    error.clear(); if (!haze_control_validate(values,error)) return false;
    std::string current; bool exists=false;
    if (!read(document.path,current,exists,error)) return false;
    if (exists!=document.existed || current!=document.original) { error="Input settings changed elsewhere. Reopen Controls before applying."; return false; }
    std::map<std::string,std::string> ids; std::set<std::string> written;
    for (const auto& spec:haze_control_specs()) ids[lower(spec.id())]=spec.id();
    std::string source=current,bom;
    if (source.starts_with("\xef\xbb\xbf")) { bom=source.substr(0,3); source.erase(0,3); }
    std::ostringstream output; output<<bom; std::string section,line; std::istringstream lines(source);
    auto missing=[&] {
        for (const auto& spec:haze_control_specs()) if (lower(spec.section)==section && written.insert(spec.id()).second)
            output<<spec.key<<'='<<values.at(spec.id())<<"\r\n";
    };
    std::set<std::string> sections;
    while (std::getline(lines,line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        auto clean=trim(line);
        if (clean.size()>1 && clean[0]=='[' && clean.back()==']') {
            missing(); section=lower(trim(clean.substr(1,clean.size()-2))); sections.insert(section);
        } else if (!clean.empty() && clean[0]!=';' && clean[0]!='#') {
            const auto equal=line.find('=');
            if (equal!=std::string::npos) {
                const auto found=ids.find(section+"."+lower(trim(line.substr(0,equal))));
                if (found!=ids.end()) { line=line.substr(0,equal+1)+values.at(found->second); written.insert(found->second); }
            }
        }
        output<<line<<"\r\n";
    }
    missing();
    for (const auto& spec:haze_control_specs()) if (sections.insert(lower(spec.section)).second) {
        section=lower(spec.section); output<<"\r\n["<<spec.section<<"]\r\n"; missing();
    }
    const auto text=output.str(); const auto destination=std::filesystem::path(document.path).wstring();
    static std::atomic<unsigned> serial{0};
    const auto temporary=destination+L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++serial);
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) { error="Cannot create an input settings file. Check folder permissions."; return false; }
    DWORD written_bytes=0;
    bool ok=WriteFile(file,text.data(),DWORD(text.size()),&written_bytes,nullptr) && written_bytes==text.size() && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok=false;
    if (!read(document.path,current,exists,error) || exists!=document.existed || current!=document.original) {
        error="Input settings changed or became unavailable. Reopen Controls before applying."; ok=false;
    }
    if (ok) ok=MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if (!ok) { DeleteFileW(temporary.c_str()); if (error.empty()) error="Could not save controls. Previous bindings retained."; return false; }
    document.values=values; document.original=text; document.existed=true; return true;
}
std::vector<std::string> haze_control_conflicts(const HazeControlValues& values,const std::string& id,const std::string& binding) {
    std::vector<std::string> result; if (lower(binding)=="none") return result;
    const bool keyboard=id.starts_with("Bindings.");
    auto identity=[&](std::string text) {
        text=lower(text);
        if (!keyboard) {
            if (text=="a") return std::string("south"); if (text=="b") return std::string("east");
            if (text=="x") return std::string("west"); if (text=="y") return std::string("north");
            if (text.starts_with("raw:+")) text.erase(4,1);
            else if (text.starts_with("+")) text.erase(0,1);
        }
        return text;
    };
    for (const auto& spec:haze_control_specs()) {
        if (spec.id()==id || (spec.kind!=HazeControlKind::Key && spec.kind!=HazeControlKind::Button && spec.kind!=HazeControlKind::Axis)) continue;
        if ((spec.kind==HazeControlKind::Key)!=keyboard) continue;
        const auto found=values.find(spec.id());
        if (found!=values.end() && identity(found->second)==identity(binding)) result.push_back(spec.id());
    }
    return result;
}
