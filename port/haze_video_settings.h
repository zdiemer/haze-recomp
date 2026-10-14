#pragma once
#include <cstring>
#include <cstdlib>
#include "rsx_graphics_settings.h"
#include <map>
#include <string>
#include <sstream>
#include <vector>

namespace haze_video {
using Values=std::map<std::string,std::string>;
inline const std::vector<rsx_graphics_option>& options() {
    static const auto result=[] {
        std::vector<rsx_graphics_option> out(std::begin(rsx_graphics_options),std::end(rsx_graphics_options));
        out.push_back({"performance_overlay","HAZE_PERFORMANCE_OVERLAY","off","off, compact or expanded",9,0,0});
        out.push_back({"overlay_position","HAZE_OVERLAY_POSITION","top-left","top-left, top-right, bottom-left or bottom-right",10,0,0});
        return out;
    }();
    return result;
}
inline const rsx_graphics_option* find_option(const char* key) {
    for (const auto& o:options()) if (!std::strcmp(o.key,key)) return &o;
    return nullptr;
}
inline std::string trim(std::string text) {
    const auto first=text.find_first_not_of(" \t\r\n");
    return first==std::string::npos ? std::string{} :
        text.substr(first,text.find_last_not_of(" \t\r\n")-first+1);
}
inline bool automatic_size(const rsx_graphics_option& o) {
    return !std::strcmp(o.key,"render_resolution") || !std::strcmp(o.key,"window_size");
}
inline const char* fallback(const rsx_graphics_option& o) {
    if (!std::strcmp(o.key,"display_mode")) return "borderless";
    if (automatic_size(o)) return "auto";
    if (!std::strcmp(o.key,"shadow_resolution")) return "native";
    return o.fallback;
}
inline bool valid(const rsx_graphics_option& o,const char* value) {
    if (!value) return false;
    if (o.kind==9) return !std::strcmp(value,"off") || !std::strcmp(value,"compact") || !std::strcmp(value,"expanded");
    if (o.kind==10) return !std::strcmp(value,"top-left") || !std::strcmp(value,"top-right") ||
        !std::strcmp(value,"bottom-left") || !std::strcmp(value,"bottom-right");
    return (automatic_size(o) && value && !std::strcmp(value,"auto")) || rsx_graphics_valid(&o,value);
}
inline Values defaults() {
    Values out; for (const auto& o:options()) out[o.key]=fallback(o); return out;
}
inline bool validate(const Values& values,std::string& error) {
    for (const auto& [key,value]:values) {
        const auto* o=find_option(key.c_str());
        if (!o || !valid(*o,value.c_str())) { error="Invalid video setting: "+key+"="+value; return false; }
    }
    return true;
}
inline bool parse(std::istream& file,Values& result,std::string& error) {
    Values read; std::string line,section; unsigned number=0;
    while (std::getline(file,line)) {
        ++number;
        if (number==1 && line.compare(0,3,"\xef\xbb\xbf")==0) line.erase(0,3);
        line=trim(line.substr(0,line.find_first_of(";#")));
        if (line.empty()) continue;
        if (line.front()=='[' && line.back()==']') {
            section=trim(line.substr(1,line.size()-2));
            if (section=="graphics") continue;
            error="Unknown section on line "+std::to_string(number); return false;
        }
        const auto split=line.find('='); const auto key=trim(line.substr(0,split));
        if (section!="graphics" || split==std::string::npos || read.count(key)) {
            error="Invalid or duplicate setting on line "+std::to_string(number); return false;
        }
        read[key]=trim(line.substr(split+1));
    }
    if (file.bad()) { error="Could not read video settings."; return false; }
    if (!validate(read,error)) return false;
    result=defaults(); for (const auto& [key,value]:read) result[key]=value;
    return true;
}
inline bool restart_required(const std::string& key) {
    return key=="display_mode" || key=="monitor" || key=="window_size" ||
        key=="render_resolution" || key=="shadow_resolution";
}
// Quality presets leave display, timing and brightness preferences alone.
inline bool preset(Values& values,const std::string& name) {
    if (name!="Low" && name!="Medium" && name!="High") return false;
    values["anti_aliasing"]=name=="Low" ? "off" : "fxaa";
    values["anisotropic_filtering"]=name=="Low" ? "1" : name=="Medium" ? "4" : "16";
    values["shadow_resolution"]=name=="Low" ? "1024" : name=="Medium" ? "native" : "4096";
    values["mip_lod_bias"]="0";
    values["sharpening"]=name=="High" ? "0.15" : "0";
    return true;
}
inline std::string preset_name(const Values& values) {
    for (const auto* name:{"Low","Medium","High"}) {
        Values candidate; preset(candidate,name); bool match=true;
        for (const auto& [key,value]:candidate) {
            const auto found=values.find(key);
            if (found==values.end()) { match=false; break; }
            const auto* o=rsx_graphics_find_option(key.c_str());
            const bool same=o->kind==2 || o->kind==4 ?
                std::strtod(found->second.c_str(),nullptr)==std::strtod(value.c_str(),nullptr) : found->second==value;
            if (!same) { match=false; break; }
        }
        if (match) return name;
    }
    return "Custom";
}
inline std::string serialize(const Values& values) {
    std::ostringstream out; out<<"[graphics]\n";
    for (const auto& o:options()) out<<o.key<<" = "<<values.at(o.key)<<'\n';
    return out.str();
}
}
