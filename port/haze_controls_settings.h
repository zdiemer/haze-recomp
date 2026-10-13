#pragma once
#include <map>
#include <string>
#include <vector>

enum class HazeControlKind { Key, Button, Axis, Number, Toggle, Family, Mapping };
struct HazeControlSpec {
    std::string section,key,label,fallback;
    HazeControlKind kind;
    double minimum=0,maximum=1,step=.05;
    std::string id() const { return section+"."+key; }
};
const std::vector<HazeControlSpec>& haze_control_specs();
using HazeControlValues=std::map<std::string,std::string>;
HazeControlValues haze_control_defaults();
bool haze_control_validate(const HazeControlValues& values,std::string& error);
struct HazeControlDocument {
    HazeControlValues values;
    std::string path,original;
    bool existed=false;
};
bool haze_control_load(const std::string& path,HazeControlDocument& document,std::string& error);
bool haze_control_save(HazeControlDocument& document,const HazeControlValues& values,std::string& error);
// Exact duplicates within a device group. Shared actions may intentionally
// keep duplicates; the capture UI presents those conflicts before staging.
std::vector<std::string> haze_control_conflicts(const HazeControlValues& values,const std::string& id,const std::string& binding);
