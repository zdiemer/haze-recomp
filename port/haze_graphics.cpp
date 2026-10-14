#include "haze_graphics.h"
#include "haze_display.h"
#include "rsx_graphics_settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <map>

namespace {
using haze_video::automatic_size;
using haze_video::fallback;
using haze_video::valid;
std::string allowed(const rsx_graphics_option& option) {
    return std::string(automatic_size(option) ? "auto, or " : "")+option.allowed;
}
}
bool haze_graphics_initialize() {
    const char* configured=std::getenv("HAZE_GRAPHICS_CONFIG");
    const char* path=configured && *configured ? configured : "config/graphics.ini";
    std::ifstream file(path);
    if (!file && configured && *configured) {
        std::fprintf(stderr,"[graphics] Cannot open %s\n",path); return false;
    }
    haze_video::Values values;
    std::string error;
    if (!haze_video::parse(file,values,error)) {
        std::fprintf(stderr,"[graphics] %s: %s\n",path,error.c_str()); return false;
    }
    // Validate the complete file and overrides before changing the process.
    for (const auto& option:haze_video::options()) {
        const char* value=std::getenv(option.environment);
        if (value && *value && !valid(option,value)) {
            std::fprintf(stderr,"[graphics] Invalid %s=%s (expected %s)\n",option.environment,value,allowed(option).c_str()); return false;
        }
    }
    std::map<std::string,std::string> effective,overrides;
    for (const auto& option:haze_video::options()) {
        const char* current=std::getenv(option.environment);
        if (current && *current) overrides[option.key]=current;
        const auto found=values.find(option.key);
        effective[option.key]=current && *current ? current :
            found!=values.end() ? found->second.c_str() : fallback(option);
    }
    if(effective["render_resolution"]=="auto" || effective["window_size"]=="auto") {
        haze_display_defaults detected{};
        if(!haze_display_detect(effective["monitor"].c_str(),detected)) return false;
        auto dimensions=[](unsigned w,unsigned h) { return std::to_string(w)+"x"+std::to_string(h); };
        if(effective["render_resolution"]=="auto")
            effective["render_resolution"]=dimensions(detected.render_width,detected.render_height);
        if(effective["window_size"]=="auto")
            effective["window_size"]=dimensions(detected.window_width,detected.window_height);
    }
    std::fprintf(stderr,"[graphics] %s %s\n",file.is_open() ? "Loaded" : "Defaults; no file at",path);
    for (const auto& option:haze_video::options()) {
        const auto& value=effective.at(option.key);
#ifdef _WIN32
        if (_putenv_s(option.environment,value.c_str())!=0) return false;
#else
        if (setenv(option.environment,value.c_str(),1)!=0) return false;
#endif
        std::fprintf(stderr,"[graphics] %s=%s\n",option.key,value.c_str());
    }
    haze_graphics_started(values,effective,overrides,path);
    return true;
}
