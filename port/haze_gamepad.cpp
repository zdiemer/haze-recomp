#include "haze_gamepad.h"
#include "haze_input_source.h"
#include <SDL3/SDL.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
struct Device {
    SDL_JoystickID id;
    SDL_Gamepad* pad;
    SDL_Joystick* joystick;
    CellPadHostInput previous{0,128,128,128,128,0,0};
    std::vector<Sint16> resting_axes;
    std::vector<Sint16> previous_axes;
    std::vector<bool> previous_buttons;
    std::vector<Uint8> previous_hats;
    std::vector<SDL_GamepadBinding> mapping;
    bool menu_aliases=false,standard_aliases=false;
};
std::vector<Device> devices;
SDL_JoystickID selected=0;
bool initialized=false;
Uint64 next_scan=0;
float move_deadzone=.15f, look_deadzone=.12f, trigger_deadzone=.05f;
bool invert_look=false;
bool rumble_enabled=true;
float rumble_intensity=1;
struct Rumble { SDL_JoystickID id=0; Uint16 low=0,high=0; Uint64 refresh=0; } rumble;
int forced_family=-1;
HazeBindingCapture capture_state;
SDL_JoystickID capture_device=0;
bool capture_axis=false;
std::vector<Sint16> capture_centers;
struct Source { int named=-1,sign=1; HazeRawSource raw; Source(int n=-1):named(n) {} };
struct Binding { const char* name; const char* fallback; u16 mask; Source source; };
constexpr int axis_source=32;
Binding bindings[]={
    {"Cross","south",CELL_PAD_CTRL_CROSS,SDL_GAMEPAD_BUTTON_SOUTH},
    {"Circle","east",CELL_PAD_CTRL_CIRCLE,SDL_GAMEPAD_BUTTON_EAST},
    {"Square","west",CELL_PAD_CTRL_SQUARE,SDL_GAMEPAD_BUTTON_WEST},
    {"Triangle","north",CELL_PAD_CTRL_TRIANGLE,SDL_GAMEPAD_BUTTON_NORTH},
    {"L1","leftshoulder",CELL_PAD_CTRL_L1,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"R1","rightshoulder",CELL_PAD_CTRL_R1,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"L2","lefttrigger",CELL_PAD_CTRL_L2,axis_source+SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    {"R2","righttrigger",CELL_PAD_CTRL_R2,axis_source+SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    {"L3","leftstick",CELL_PAD_CTRL_L3,SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"R3","rightstick",CELL_PAD_CTRL_R3,SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"Start","start",CELL_PAD_CTRL_START,SDL_GAMEPAD_BUTTON_START},
    {"Select","back",CELL_PAD_CTRL_SELECT,SDL_GAMEPAD_BUTTON_BACK},
    {"Up","dpup",CELL_PAD_CTRL_UP,SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"Down","dpdown",CELL_PAD_CTRL_DOWN,SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"Left","dpleft",CELL_PAD_CTRL_LEFT,SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"Right","dpright",CELL_PAD_CTRL_RIGHT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};
std::string setting(const char* path,const char* section,const char* key,const char* fallback) {
    char value[1024];
    GetPrivateProfileStringA(section,key,fallback,value,sizeof(value),path);
    return value;
}
float deadzone(const char* path,const char* key,const char* fallback) {
    const auto value=setting(path,"Controller",key,fallback);
    char* end=nullptr;
    const float result=strtof(value.c_str(),&end);
    if (end!=value.c_str() && !*end && std::isfinite(result) && result>=0 && result<.95f) return result;
    fprintf(stderr,"[input] Invalid %s=%s; using %s\n",key,value.c_str(),fallback);
    return strtof(fallback,nullptr);
}
Source source(std::string value) {
    for (auto& c:value) c=char(std::tolower((unsigned char)c));
    const auto raw=haze_raw_source(value);
    if (raw.kind!=HazeRawSource::None) { Source out; out.raw=raw; return out; }
    if (!_stricmp(value.c_str(),"none")) return -1;
    int sign=1;
    if (!value.empty() && (value[0]=='-' || value[0]=='+')) { sign=value[0]=='-' ? -1 : 1; value.erase(0,1); }
    const char* positions[]={"south","east","west","north"};
    for (int i=0;i<4;i++) if (!_stricmp(value.c_str(),positions[i])) return i;
    const auto button=SDL_GetGamepadButtonFromString(value.c_str());
    if (button!=SDL_GAMEPAD_BUTTON_INVALID) return button;
    const auto axis=SDL_GetGamepadAxisFromString(value.c_str());
    if (axis!=SDL_GAMEPAD_AXIS_INVALID) { Source out(axis_source+axis); out.sign=sign; return out; }
    fprintf(stderr,"[input] Unknown controller binding '%s'; disabled\n",value.c_str());
    return -1;
}
struct PhysicalSource { HazeRawSource::Kind kind; int index,mask=0; double low=0,high=0; };
std::vector<PhysicalSource> physical_sources(const Device* device,const Source& s,bool full_axis) {
    std::vector<PhysicalSource> result;
    if (s.raw.kind!=HazeRawSource::None) {
        const auto& r=s.raw;
        result.push_back({r.kind,r.index,r.mask,
            full_axis || r.sign<0 ? -32768. : 1.,full_axis || r.sign>=0 ? 32767. : -1.});
        return result;
    }
    if (!device || !device->pad || s.named<0) return result;
    for (const auto& b:device->mapping) {
        const bool axis=b.output_type==SDL_GAMEPAD_BINDTYPE_AXIS;
        if (axis ? s.named!=axis_source+b.output.axis.axis :
            b.output_type!=SDL_GAMEPAD_BINDTYPE_BUTTON || s.named!=b.output.button) continue;
        double first=0,last=1;
        if (axis && !full_axis) {
            const double start=b.output.axis.axis_min,end=b.output.axis.axis_max;
            const double low=s.sign<0 ? -32768. : 1.,high=s.sign<0 ? -1. : 32767.;
            if (start==end) { if (start<low || start>high) continue; }
            else {
                const double a=(low-start)/(end-start),z=(high-start)/(end-start);
                first=std::max(0.,std::min(a,z)); last=std::min(1.,std::max(a,z));
                if (first>last) continue;
            }
        }
        if (b.input_type==SDL_GAMEPAD_BINDTYPE_AXIS) {
            const double start=b.input.axis.axis_min,end=b.input.axis.axis_max;
            // SDL turns an axis into a button at the midpoint of its mapped
            // range. Axis outputs instead interpolate through the output range.
            if (!axis) first=.5;
            const double a=start+(end-start)*first,z=start+(end-start)*last;
            result.push_back({HazeRawSource::Axis,b.input.axis.axis,0,std::min(a,z),std::max(a,z)});
        } else if (!axis || full_axis || b.output.axis.axis_max*s.sign>0) {
            if (b.input_type==SDL_GAMEPAD_BINDTYPE_BUTTON)
                result.push_back({HazeRawSource::Button,b.input.button});
            else if (b.input_type==SDL_GAMEPAD_BINDTYPE_HAT)
                result.push_back({HazeRawSource::Hat,b.input.hat.hat,b.input.hat.hat_mask});
        }
    }
    return result;
}
bool overlaps(const Device* device,const Source& a,bool a_axis,const Source& b,bool b_axis) {
    if (a.named>=0 && a.named==b.named &&
        (a.named<axis_source || a_axis || b_axis || a.sign==b.sign)) return true;
    for (const auto& x:physical_sources(device,a,a_axis))
        for (const auto& y:physical_sources(device,b,b_axis)) {
            if (x.kind!=y.kind || x.index!=y.index) continue;
            if (x.kind==HazeRawSource::Button ||
                (x.kind==HazeRawSource::Hat && (x.mask&y.mask)) ||
                (x.kind==HazeRawSource::Axis && std::max(x.low,y.low)<=std::min(x.high,y.high))) return true;
        }
    return false;
}
void refresh_mapping(Device& device) {
    device.mapping.clear();
    if (device.pad) {
        int count=0; auto mapped=SDL_GetGamepadBindings(device.pad,&count);
        if (mapped) { for (int i=0;i<count;++i) device.mapping.push_back(*mapped[i]); SDL_free(mapped); }
    }
    device.menu_aliases=device.standard_aliases=false;
    for (int i=0;i<3;++i) for (int j=0;j<i;++j) {
        if (overlaps(&device,bindings[i].source,false,bindings[j].source,false)) device.menu_aliases=true;
        if (overlaps(&device,Source(i),false,Source(j),false)) device.standard_aliases=true;
    }
}
HazeInputFamily family(SDL_Gamepad* pad) {
    if (forced_family>=0) return static_cast<HazeInputFamily>(forced_family);
    if (!pad) return HazeInputFamily::Xbox;
    switch (SDL_GetRealGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5:
        return HazeInputFamily::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return HazeInputFamily::Switch;
    default: return HazeInputFamily::Xbox;
    }
}
Source stick_sources[]={axis_source+SDL_GAMEPAD_AXIS_LEFTX,axis_source+SDL_GAMEPAD_AXIS_LEFTY,
    axis_source+SDL_GAMEPAD_AXIS_RIGHTX,axis_source+SDL_GAMEPAD_AXIS_RIGHTY};
float read_source(const Device& device,const Source& source) {
    const auto& raw=source.raw;
    if (raw.kind==HazeRawSource::Button)
        return raw.index<SDL_GetNumJoystickButtons(device.joystick) && SDL_GetJoystickButton(device.joystick,raw.index) ? 1.f : 0.f;
    if (raw.kind==HazeRawSource::Hat)
        return raw.index<SDL_GetNumJoystickHats(device.joystick) && (SDL_GetJoystickHat(device.joystick,raw.index)&raw.mask) ? 1.f : 0.f;
    if (raw.kind==HazeRawSource::Axis) {
        if (raw.index>=SDL_GetNumJoystickAxes(device.joystick)) return 0;
        return SDL_GetJoystickAxis(device.joystick,raw.index)/32768.f*(raw.sign<0 ? -1.f : 1.f);
    }
    if (!device.pad) return 0;
    if (source.named>=axis_source)
        return SDL_GetGamepadAxis(device.pad,static_cast<SDL_GamepadAxis>(source.named-axis_source))/32768.f*source.sign;
    return source.named>=0 && SDL_GetGamepadButton(device.pad,static_cast<SDL_GamepadButton>(source.named)) ? 1.f : 0.f;
}
void stick(const Device& device,int first,float dz,u8& x,u8& y) {
    float fx=read_source(device,stick_sources[first]);
    float fy=read_source(device,stick_sources[first+1]);
    const float magnitude=std::hypot(fx,fy);
    const float scale=magnitude>dz ? (std::min(magnitude,1.f)-dz)/((1-dz)*magnitude) : 0;
    fx*=scale; fy*=scale;
    if (first==2 && invert_look) fy=-fy;
    auto quantize=[](float v) { return static_cast<u8>(std::clamp(std::lround(128+v*(v<0 ? 128 : 127)),0l,255l)); };
    x=quantize(fx); y=quantize(fy);
}
CellPadHostInput read(const Device& device) {
    CellPadHostInput out{0,128,128,128,128,0,0};
    stick(device,0,move_deadzone,out.lx,out.ly);
    stick(device,2,look_deadzone,out.rx,out.ry);
    for (const auto& b:bindings) {
        int pressure=0;
        const float v=read_source(device,b.source);
        if (b.source.named>=axis_source || b.source.raw.kind==HazeRawSource::Axis) {
            if (v>trigger_deadzone) pressure=std::clamp(int(std::lround((v-trigger_deadzone)/(1-trigger_deadzone)*255)),1,255);
        } else if (v>0) pressure=255;
        if (pressure) out.buttons|=b.mask;
        if (b.mask==CELL_PAD_CTRL_L2) out.l2=static_cast<u8>(pressure);
        if (b.mask==CELL_PAD_CTRL_R2) out.r2=static_cast<u8>(pressure);
    }
    return out;
}
bool activity(const CellPadHostInput& now,const CellPadHostInput& before) {
    // Releases and sub-deadzone noise do not steal the prompt family. Held
    // buttons do not repeatedly override a subsequent keyboard/mouse action.
    if (now.buttons & ~before.buttons) return true;
    const u8 a[]={now.lx,now.ly,now.rx,now.ry}, b[]={before.lx,before.ly,before.rx,before.ry};
    for (int i=0;i<4;i++) if (a[i]!=128 && std::abs(int(a[i])-int(b[i]))>=4) return true;
    return (now.l2>before.l2+4) || (now.r2>before.r2+4);
}
void stop_rumble() {
    if (rumble.low || rumble.high)
        for (const auto& d:devices) if (d.id==rumble.id) SDL_RumbleJoystick(d.joystick,0,0,0);
    rumble={};
}
void update_capture() {
    if (capture_state.phase!=HazeBindingCapturePhase::Release && capture_state.phase!=HazeBindingCapturePhase::Listening) return;
    const auto it=std::find_if(devices.begin(),devices.end(),[](const Device& d) { return d.id==capture_device; });
    if (it==devices.end()) {
        capture_state={HazeBindingCapturePhase::Cancelled,{},"Controller disconnected. Binding unchanged."}; return;
    }
    const auto& d=*it;
    bool neutral=true;
    for (int i=0;i<SDL_GetNumJoystickButtons(d.joystick);++i) if (SDL_GetJoystickButton(d.joystick,i)) neutral=false;
    for (int i=0;i<SDL_GetNumJoystickHats(d.joystick);++i) if (SDL_GetJoystickHat(d.joystick,i)) neutral=false;
    if (d.pad) {
        for (int i=0;i<SDL_GAMEPAD_AXIS_COUNT;++i) {
            const int value=SDL_GetGamepadAxis(d.pad,static_cast<SDL_GamepadAxis>(i));
            if (i<SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? std::abs(value)>8192 : value>8192) neutral=false;
        }
    } else for (size_t i=0;i<capture_centers.size();++i)
        if (std::abs(int(SDL_GetJoystickAxis(d.joystick,int(i)))-capture_centers[i])>8192) neutral=false;
    if (capture_state.phase==HazeBindingCapturePhase::Release) {
        if (neutral) capture_state={HazeBindingCapturePhase::Listening,{},capture_axis ? "Move the axis to bind." : "Press a button, move an axis or a hat."};
        return;
    }
    auto accept=[](std::string value) { capture_state={HazeBindingCapturePhase::Captured,std::move(value),"Binding captured."}; };
    if (d.pad) {
        if (!capture_axis) for (int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;++i)
            if (SDL_GetGamepadButton(d.pad,static_cast<SDL_GamepadButton>(i))) {
                const char* positions[]={"south","east","west","north"};
                accept(i<4 ? positions[i] : SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(i))); return;
            }
        for (int i=0;i<SDL_GAMEPAD_AXIS_COUNT;++i) {
            const int value=SDL_GetGamepadAxis(d.pad,static_cast<SDL_GamepadAxis>(i));
            if (std::abs(value)<20000) continue;
            if (i>=SDL_GAMEPAD_AXIS_LEFT_TRIGGER && value<0) continue;
            std::string name=SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(i));
            if (!capture_axis && value<0) name="-"+name;
            accept(name); return;
        }
    }
    if (!capture_axis) {
        for (int i=0;i<SDL_GetNumJoystickButtons(d.joystick);++i)
            if (SDL_GetJoystickButton(d.joystick,i)) { accept("raw:b"+std::to_string(i)); return; }
        for (int i=0;i<SDL_GetNumJoystickHats(d.joystick);++i) {
            const auto value=SDL_GetJoystickHat(d.joystick,i);
            for (int bit:{1,2,4,8}) if (value&bit) { accept("raw:h"+std::to_string(i)+"."+std::to_string(bit)); return; }
        }
    }
    for (size_t i=0;i<capture_centers.size();++i) {
        const int delta=int(SDL_GetJoystickAxis(d.joystick,int(i)))-capture_centers[i];
        if (std::abs(delta)>20000) {
            accept(std::string("raw:")+(capture_axis ? "" : delta>0 ? "+" : "-")+"a"+std::to_string(i)); return;
        }
    }
}
}

void haze_gamepad_configure(const char* path) {
    for (auto& b:bindings) b.source=source(setting(path,"ControllerBindings",b.name,b.fallback));
    const char* keys[]={"MoveX","MoveY","LookX","LookY"};
    const char* defaults[]={"leftx","lefty","rightx","righty"};
    for (int i=0;i<4;i++) {
        const auto value=setting(path,"Controller",keys[i],defaults[i]);
        stick_sources[i]=source(value);
        if (stick_sources[i].raw.kind!=HazeRawSource::Axis && stick_sources[i].named<axis_source && _stricmp(value.c_str(),"none")) {
            fprintf(stderr,"[input] Unknown controller axis '%s'; disabled\n",value.c_str());
            stick_sources[i]=Source();
        }
    }
    move_deadzone=deadzone(path,"MoveDeadzone","0.15");
    look_deadzone=deadzone(path,"LookDeadzone","0.12");
    trigger_deadzone=deadzone(path,"TriggerDeadzone","0.05");
    invert_look=GetPrivateProfileIntA("Controller","InvertLookY",0,path)!=0;
    rumble_enabled=GetPrivateProfileIntA("Controller","Rumble",1,path)!=0;
    const auto intensity=setting(path,"Controller","RumbleIntensity","1.00");
    char* end=nullptr; rumble_intensity=strtof(intensity.c_str(),&end);
    if (end==intensity.c_str() || *end || !std::isfinite(rumble_intensity) || rumble_intensity<0 || rumble_intensity>1) {
        fprintf(stderr,"[input] Invalid RumbleIntensity=%s; using 1.00\n",intensity.c_str()); rumble_intensity=1;
    }
    auto style=setting(path,"Controller","IconFamily","auto");
    forced_family=!_stricmp(style.c_str(),"playstation") ? int(HazeInputFamily::PlayStation) :
                  !_stricmp(style.c_str(),"switch") ? int(HazeInputFamily::Switch) :
                  !_stricmp(style.c_str(),"xbox") ? int(HazeInputFamily::Xbox) : -1;
    const auto mapping=setting(path,"Controller","MappingFile","");
    if (!mapping.empty()) {
        const auto file=std::filesystem::path(path).parent_path()/mapping;
        if (SDL_AddGamepadMappingsFromFile(file.string().c_str())<0)
            fprintf(stderr,"[input] Cannot load mapping file %s: %s\n",file.string().c_str(),SDL_GetError());
    }
    for (auto& device:devices) refresh_mapping(device);
}
bool haze_gamepad_initialize(const char* config) {
    if (initialized) return true;
    // Use physical face positions consistently on Nintendo hardware too.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr,"[input] SDL gamepad initialization failed: %s\n",SDL_GetError()); return false;
    }
    initialized=true;
    SDL_SetGamepadEventsEnabled(false);
    haze_gamepad_configure(config);
    fprintf(stderr,"[input] SDL %d.%d.%d gamepad wrapper enabled\n",SDL_MAJOR_VERSION,SDL_MINOR_VERSION,SDL_MICRO_VERSION);
    return true;
}
HazeGamepadState haze_gamepad_update() {
    HazeGamepadState out;
    if (!initialized) return out;
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    for (auto it=devices.begin();it!=devices.end();) {
        if (!SDL_JoystickConnected(it->joystick)) {
            fprintf(stderr,"[input] Controller disconnected: %u\n",it->id);
            if (selected==it->id) selected=0;
            if (rumble.id==it->id) rumble={};
            if (it->pad) SDL_CloseGamepad(it->pad);
            SDL_CloseJoystick(it->joystick); it=devices.erase(it); next_scan=0;
        } else ++it;
    }
    if (SDL_GetTicks()>=next_scan) {
        next_scan=SDL_GetTicks()+500;
        int count=0;
        SDL_JoystickID* ids=SDL_GetJoysticks(&count);
        for (int i=0;i<count;i++) {
            auto found=std::find_if(devices.begin(),devices.end(),[&](const Device& d) { return d.id==ids[i]; });
            if (found!=devices.end()) {
                if (!found->pad && SDL_IsGamepad(ids[i])) found->pad=SDL_OpenGamepad(ids[i]);
                refresh_mapping(*found);
                continue;
            }
            if (auto* joystick=SDL_OpenJoystick(ids[i])) {
                auto* pad=SDL_IsGamepad(ids[i]) ? SDL_OpenGamepad(ids[i]) : nullptr;
                devices.push_back({ids[i],pad,joystick});
                refresh_mapping(devices.back());
                // Keep connection-time rest values stable across captures.
                // SDL's initial-state report can initialize lazily on a device's
                // first axis event, which may be the motion being rebound.
                for (int axis=0;axis<std::min(256,SDL_GetNumJoystickAxes(joystick));++axis)
                    devices.back().resting_axes.push_back(SDL_GetJoystickAxis(joystick,axis));
                devices.back().previous_axes=devices.back().resting_axes;
                devices.back().previous_buttons.resize(std::min(256,SDL_GetNumJoystickButtons(joystick)));
                devices.back().previous_hats.resize(std::min(256,SDL_GetNumJoystickHats(joystick)));
                fprintf(stderr,"[input] Controller connected: %s (%u)%s\n",SDL_GetJoystickName(joystick),ids[i],pad ? "":" (needs mapping)");
            }
        }
        SDL_free(ids);
    }
    if (!selected && !devices.empty()) selected=devices.front().id;
    for (auto& d:devices) {
        const auto value=read(d);
        bool used=activity(value,d.previous);
        for (size_t i=0;i<d.previous_buttons.size();++i) {
            const bool pressed=SDL_GetJoystickButton(d.joystick,int(i));
            used|=pressed && !d.previous_buttons[i]; d.previous_buttons[i]=pressed;
        }
        for (size_t i=0;i<d.previous_hats.size();++i) {
            const auto hat=SDL_GetJoystickHat(d.joystick,int(i));
            used|=(hat&~d.previous_hats[i])!=0; d.previous_hats[i]=hat;
        }
        for (size_t i=0;i<d.previous_axes.size();++i) {
            const int axis=SDL_GetJoystickAxis(d.joystick,int(i));
            used|=std::abs(axis-d.resting_axes[i])>8192 && std::abs(axis-d.previous_axes[i])>4096;
            d.previous_axes[i]=Sint16(axis);
        }
        d.previous=value;
        if (used) { selected=d.id; out.activity=true; }
    }
    for (const auto& d:devices) if (d.id==selected) {
        out.input=d.previous; out.family=family(d.pad); out.connected=true;
        out.menu_input=out.input; out.menu_input_valid=true;
        // Unbinding gameplay inputs must not strand the player in Settings.
        // Recovery applies only to absent menu bindings and only to the UI.
        auto unbound=[](const Source& s) { return s.named<0 && s.raw.kind==HazeRawSource::None; };
        bool recovery=d.menu_aliases;
        for (int i=0;i<3;++i) {
            const auto& s=bindings[i].source;
            if (unbound(s) || s.named>=axis_source || s.raw.kind==HazeRawSource::Axis) recovery=true;
        }
        if (recovery) {
            const bool mapped=d.pad && !d.standard_aliases;
            out.menu_recovery_raw=!mapped;
            out.menu_recovery=CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_CIRCLE|CELL_PAD_CTRL_SQUARE;
            out.menu_input.buttons=0;
            const unsigned masks[]={CELL_PAD_CTRL_CROSS,CELL_PAD_CTRL_CIRCLE,CELL_PAD_CTRL_SQUARE};
            for (int i=0;i<3;++i) if (mapped ? SDL_GetGamepadButton(d.pad,static_cast<SDL_GamepadButton>(i)) :
                i<SDL_GetNumJoystickButtons(d.joystick) && SDL_GetJoystickButton(d.joystick,i)) out.menu_input.buttons|=masks[i];
            const unsigned directions[]={CELL_PAD_CTRL_UP,CELL_PAD_CTRL_RIGHT,CELL_PAD_CTRL_DOWN,CELL_PAD_CTRL_LEFT};
            const SDL_GamepadButton pad_directions[]={SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT};
            for (int i=0;i<4;++i) if (mapped ? SDL_GetGamepadButton(d.pad,pad_directions[i]) :
                SDL_GetNumJoystickHats(d.joystick)>0 && (SDL_GetJoystickHat(d.joystick,0)&(1<<i))) out.menu_input.buttons|=directions[i];
        }
        if (recovery || unbound(stick_sources[0]) || unbound(stick_sources[1])) {
            const bool mapped=d.pad && !out.menu_recovery_raw;
            const int x=mapped ? SDL_GetGamepadAxis(d.pad,SDL_GAMEPAD_AXIS_LEFTX) : SDL_GetNumJoystickAxes(d.joystick)>0 ? SDL_GetJoystickAxis(d.joystick,0) : 0;
            const int y=mapped ? SDL_GetGamepadAxis(d.pad,SDL_GAMEPAD_AXIS_LEFTY) : SDL_GetNumJoystickAxes(d.joystick)>1 ? SDL_GetJoystickAxis(d.joystick,1) : 0;
            if (recovery || unbound(stick_sources[0])) out.menu_input.lx=x<-16384 ? 0 : x>16384 ? 255 : 128;
            if (recovery || unbound(stick_sources[1])) out.menu_input.ly=y<-16384 ? 0 : y>16384 ? 255 : 128;
        }
    }
    update_capture();
    return out;
}
HazeControllerInfo haze_gamepad_info() {
    for (const auto& d:devices) if (d.id==selected) {
        const char* name=SDL_GetJoystickName(d.joystick);
        return {name ? name : "Controller",true,d.pad!=nullptr,family(d.pad)};
    }
    return {"No controller connected",false,false};
}
bool haze_gamepad_bindings_overlap(const std::string& first,bool first_axis,const std::string& second,bool second_axis) {
    for (auto& device:devices) if (device.id==selected) {
        refresh_mapping(device);
        return overlaps(&device,source(first),first_axis,source(second),second_axis);
    }
    return overlaps(nullptr,source(first),first_axis,source(second),second_axis);
}
void haze_gamepad_capture_begin(bool axis_only) {
    capture_axis=axis_only; capture_device=selected; capture_centers.clear();
    for (const auto& d:devices) if (d.id==selected) {
        capture_centers=d.resting_axes;
        capture_state={HazeBindingCapturePhase::Release,{},"Release all controller inputs first."}; return;
    }
    capture_state={HazeBindingCapturePhase::Cancelled,{},"Connect a controller before capturing a binding."};
}
void haze_gamepad_capture_cancel() { capture_state={HazeBindingCapturePhase::Cancelled,{},"Capture cancelled. Binding unchanged."}; }
HazeBindingCapture haze_gamepad_capture() { return capture_state; }
void haze_gamepad_rumble(u8 small,u8 large) {
    if (!initialized) return;
    // PS3 actuators hold until changed: small is on/off, large 0-255. Short
    // refreshed effects stop by themselves if this thread stops servicing them.
    const float scale=rumble_enabled ? rumble_intensity : 0;
    const auto low=Uint16(std::lround(large*257*scale)),high=Uint16(small ? std::lround(65535*scale) : 0);
    if (rumble.id!=selected) stop_rumble();
    const auto it=std::find_if(devices.begin(),devices.end(),[](const Device& d) { return d.id==selected; });
    if (it==devices.end()) return;
    const auto now=SDL_GetTicks();
    if (rumble.id==it->id && rumble.low==low && rumble.high==high && (!(low|high) || now<rumble.refresh)) return;
    if (((low|high) || (rumble.low|rumble.high)) && !SDL_RumbleJoystick(it->joystick,low,high,(low|high) ? 250 : 0) &&
        (low|high) && rumble.id!=it->id)
        fprintf(stderr,"[input] Controller %u has no rumble: %s\n",it->id,SDL_GetError());
    rumble={it->id,low,high,now+100};
}
void haze_gamepad_shutdown() {
    stop_rumble();
    for (auto& d:devices) { if (d.pad) SDL_CloseGamepad(d.pad); SDL_CloseJoystick(d.joystick); }
    devices.clear(); selected=0; next_scan=0;
    if (initialized) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    initialized=false;
    capture_state={}; capture_device=0; capture_centers.clear();
}
