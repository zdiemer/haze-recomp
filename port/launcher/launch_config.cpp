#include "launch.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <set>

namespace haze_launch {
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if (!n) throw std::runtime_error("Launcher configuration is not valid UTF-8.");
    std::wstring result(size_t(n),L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),n); return result;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    if (!n) throw std::runtime_error("A launcher path contains invalid Unicode.");
    std::string result(size_t(n),'\0'); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),n,nullptr,nullptr); return result;
}
std::wstring error_text(const char* message) {
    try { return wide(message); }
    catch (...) {
        // MSVC filesystem exceptions can contain paths in the active ANSI code
        // page. Reporting an error must not throw another encoding exception.
        const int size=MultiByteToWideChar(CP_ACP,0,message,-1,nullptr,0);
        if (!size) return L"An error occurred while accessing launcher files.";
        std::wstring result(size_t(size),L'\0'); MultiByteToWideChar(CP_ACP,0,message,-1,result.data(),size); result.pop_back(); return result;
    }
}
static std::wstring trim(std::wstring value) {
    const auto first=value.find_first_not_of(L" \t\r"); if (first==value.npos) return {};
    return value.substr(first,value.find_last_not_of(L" \t\r")-first+1);
}
static const std::set<std::wstring,Less> keys={L"InstallRoot",L"SharedRoot",L"Executable",L"DiscDirectory",L"Firmware",L"Savedata",L"ShaderCache",L"Sessions",L"InputConfig",L"GraphicsConfig",L"InputMode",L"RenderResolution"};
Values read_config(const fs::path& path,bool required) {
    std::error_code ec; const bool exists=fs::exists(path,ec);
    if (ec || (!exists && required)) throw std::runtime_error("Cannot read launcher configuration: "+utf8(path.wstring()));
    if (!exists) return {};
    const auto size=fs::file_size(path,ec);
    if (ec || size>1024*1024) throw std::runtime_error("Launcher configuration is unreadable or exceeds 1 MiB.");
    std::ifstream file(path,std::ios::binary); if (!file) throw std::runtime_error("Cannot open launcher configuration.");
    std::string bytes{std::istreambuf_iterator<char>(file),{}};
    if (bytes.size()!=size || bytes.find('\0')!=bytes.npos) throw std::runtime_error("Cannot read valid launcher configuration.");
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0,3);
    const auto text=wide(bytes); Values values; bool section=false;
    size_t offset=0;
    while (offset<text.size()) {
        auto end=text.find(L'\n',offset); if (end==text.npos) end=text.size();
        auto line=trim(text.substr(offset,end-offset)); offset=end+1;
        if (line.empty() || line[0]==L';' || line[0]==L'#') continue;
        if (line[0]==L'[') { section=!_wcsicmp(line.c_str(),L"[Launcher]"); continue; }
        if (!section) continue;
        const auto equals=line.find(L'='); if (equals==line.npos) throw std::runtime_error("Expected Key=Value in the Launcher section.");
        auto key=trim(line.substr(0,equals)),value=trim(line.substr(equals+1));
        if (!keys.contains(key)) throw std::runtime_error("Unknown launcher setting: "+utf8(key));
        if (!values.emplace(key,value).second) throw std::runtime_error("Duplicate launcher setting: "+utf8(key));
    }
    return values;
}
std::wstring quote(const std::wstring& argument) {
    std::wstring result=L"\""; unsigned slashes=0;
    for (wchar_t c:argument) {
        if (!c) throw std::runtime_error("A launch argument contains a null character.");
        if (c==L'\\') { ++slashes; continue; }
        if (c==L'\"') { result.append(slashes*2+1,L'\\'); result+=c; }
        else { result.append(slashes,L'\\'); result+=c; }
        slashes=0;
    }
    result.append(slashes*2,L'\\'); result+=L'\"'; return result;
}
Options parse(const std::vector<std::wstring>& args) {
    const Values flags={{L"--install-root",L"InstallRoot"},{L"--shared-root",L"SharedRoot"},{L"--executable",L"Executable"},
        {L"--disc-directory",L"DiscDirectory"},{L"--firmware",L"Firmware"},{L"--savedata",L"Savedata"},
        {L"--shader-cache",L"ShaderCache"},{L"--sessions",L"Sessions"},{L"--input-config",L"InputConfig"},
        {L"--graphics-config",L"GraphicsConfig"},{L"--input-mode",L"InputMode"},{L"--render-resolution",L"RenderResolution"}};
    Options options;
    for (size_t i=0;i<args.size();++i) {
        if (args[i]==L"--no-ui") { options.no_ui=true; continue; }
        if (args[i]==L"--check") { options.check=true; continue; }
        const auto key=flags.find(args[i]);
        if (key==flags.end() && args[i]!=L"--config") throw std::runtime_error("Unknown launcher option: "+utf8(args[i]));
        if (i+1==args.size() || args[i+1].empty() || args[i+1].starts_with(L"--")) throw std::runtime_error("A launcher option is missing its value.");
        if (args[i]==L"--config") { if (!options.config.empty()) throw std::runtime_error("Duplicate --config option."); options.config=args[++i]; }
        else { const auto name=key->second; if (!options.overrides.emplace(name,args[++i]).second) throw std::runtime_error("Duplicate launcher option."); }
    }
    return options;
}
Values environment() {
    Values values; auto block=GetEnvironmentStringsW();
    if (!block) throw std::runtime_error("Cannot read process environment.");
    for (const wchar_t* p=block;*p;p+=wcslen(p)+1) {
        const std::wstring entry=p; const auto separator=entry.find(L'=',entry[0]==L'=' ? 1 : 0);
        if (separator!=entry.npos) values[entry.substr(0,separator)]=entry.substr(separator+1);
    }
    FreeEnvironmentStringsW(block); return values;
}
static fs::path absolute(const std::wstring& value,const fs::path& base) {
    fs::path path(value); if (value.find(L'\0')!=value.npos || value.find_first_of(L"\r\n\"")!=value.npos)
        throw std::runtime_error("A configured path contains unsupported characters.");
    if (path.has_root_name() && !path.is_absolute()) throw std::runtime_error("Drive-relative launcher paths are not supported. Use an absolute path.");
    auto result=(path.is_absolute() ? path : base/path).lexically_normal();
    if (!result.has_filename() && result!=result.root_path()) result=result.parent_path();
    return result;
}
Plan resolve(const fs::path& launcher,const Options& options,const Values& inherited) {
    if (!launcher.is_absolute()) throw std::runtime_error("Launcher location must be absolute.");
    Plan p; p.launcher=launcher; const auto folder=launcher.parent_path();
    Values defaults=read_config(folder/L"launcher-defaults.ini");
    auto env=[&](const wchar_t* key) { auto item=inherited.find(key); return item==inherited.end() ? std::wstring{} : item->second; };
    p.config=absolute(!options.config.empty() ? options.config.wstring() : !env(L"HAZE_LAUNCHER_CONFIG").empty() ? env(L"HAZE_LAUNCHER_CONFIG") : L"launcher.ini",folder);
    const auto saved=read_config(p.config,!options.config.empty() || !env(L"HAZE_LAUNCHER_CONFIG").empty());
    auto value=[&](const wchar_t* key,const wchar_t* variable=L"") {
        auto item=options.overrides.find(key); if (item!=options.overrides.end()) return item->second;
        if (*variable && !env(variable).empty()) return env(variable);
        item=saved.find(key); if (item!=saved.end() && !item->second.empty()) return item->second;
        item=defaults.find(key); return item==defaults.end() ? std::wstring{} : item->second;
    };
    auto path=[&](const wchar_t* key,const wchar_t* variable,const fs::path& fallback,const fs::path& base) {
        auto text=value(key,variable); return text.empty() ? fallback : absolute(text,base);
    };
    p.root=path(L"InstallRoot",L"HAZE_INSTALL_ROOT",folder,folder);
    p.shared=path(L"SharedRoot",L"HAZE_SHARED_ROOT",p.root,p.root);
    p.executable=path(L"Executable",L"HAZE_EXECUTABLE",folder/L"HazeRecomp.exe",p.root);
    p.disc=path(L"DiscDirectory",L"PS3_VFS_ROOT",p.shared/L"game/disc",p.root); p.elf=p.disc/L"PS3_GAME/USRDIR/EBOOT.ELF";
    p.firmware=path(L"Firmware",L"PS3_DEV_FLASH",p.shared/L"local/tools/rpcs3/dev_flash",p.root);
    p.saves=path(L"Savedata",L"PS3_SAVEDATA_ROOT",p.root/L"local/state/savedata",p.root);
    p.cache=path(L"ShaderCache",L"RSX_SHADER_CACHE_DIR",p.root/L"local/cache/rsx-shaders",p.root);
    p.sessions=path(L"Sessions",L"HAZE_SESSION_ROOT",p.root/L"local/sessions",p.root);
    p.input=path(L"InputConfig",L"HAZE_INPUT_CONFIG",p.root/L"config/input.ini",p.root);
    auto graphics=value(L"GraphicsConfig",L"HAZE_GRAPHICS_CONFIG");
    if (!graphics.empty()) p.graphics=absolute(graphics,p.root);
    else { std::error_code ec; auto candidate=p.root/L"config/graphics.ini"; if (fs::is_regular_file(candidate,ec)) p.graphics=candidate; }
    p.mode=value(L"InputMode",L"HAZE_INPUT"); if (p.mode.empty()) p.mode=L"auto";
    std::transform(p.mode.begin(),p.mode.end(),p.mode.begin(),[](wchar_t c){return c>=L'A' && c<=L'Z' ? c+32 : c;});
    if (p.mode==L"keyboardmouse") p.mode=L"keyboard";
    if (p.mode!=L"auto" && p.mode!=L"keyboard" && p.mode!=L"controller") throw std::runtime_error("InputMode must be Auto, KeyboardMouse or Controller.");
    p.resolution=value(L"RenderResolution",L"RSX_RENDER_RESOLUTION");
    if (!p.resolution.empty()) {
        unsigned w=0,h=0; wchar_t extra=0;
        if (swscanf_s(p.resolution.c_str(),L"%ux%u%c",&w,&h,&extra,1)!=2 || !w || !h || w>8192 || h>8192 ||
            std::to_wstring(w)+L"x"+std::to_wstring(h)!=p.resolution) throw std::runtime_error("RenderResolution must be WIDTHxHEIGHT, each from 1 to 8192.");
    }
    p.environment=inherited;
    const Values required={{L"PS3_VFS_ROOT",p.disc.wstring()},{L"PS3_DEV_FLASH",p.firmware.wstring()},
        {L"PS3_SAVEDATA_ROOT",p.saves.wstring()},{L"RSX_SHADER_CACHE_DIR",p.cache.wstring()},
        {L"HAZE_INPUT_CONFIG",p.input.wstring()},{L"HAZE_INPUT",p.mode},{L"HAZE_LAUNCHER_PATH",launcher.wstring()},
        {L"HAZE_LAUNCHER_CONFIG",p.config.wstring()},{L"PS3_NET_ONLINE",L"1"},{L"YDKJ_NO_CRI_CTX",L"1"},
        {L"PS3_HLE_UNRESOLVED",L"fail"},{L"PS3_VERBOSE",L"0"},{L"RSX_LIVE_DRAW",L"1"}};
    for (const auto& [key,v]:required) p.environment[key]=v;
    if (p.graphics.empty()) p.environment.erase(L"HAZE_GRAPHICS_CONFIG"); else p.environment[L"HAZE_GRAPHICS_CONFIG"]=p.graphics.wstring();
    if (p.resolution.empty()) p.environment.erase(L"RSX_RENDER_RESOLUTION"); else p.environment[L"RSX_RENDER_RESOLUTION"]=p.resolution;
    // Steam may start without this process's environment or command-line
    // overrides. Export a canonical invocation for the in-game registration UI.
    auto argument=[&](const wchar_t* key,const std::wstring& v) { if (!p.steam_arguments.empty()) p.steam_arguments+=L' '; p.steam_arguments+=key; p.steam_arguments+=L' '; p.steam_arguments+=quote(v); };
    argument(L"--install-root",p.root.wstring()); argument(L"--shared-root",p.shared.wstring());
    argument(L"--executable",p.executable.wstring()); argument(L"--disc-directory",p.disc.wstring());
    argument(L"--firmware",p.firmware.wstring()); argument(L"--savedata",p.saves.wstring());
    argument(L"--shader-cache",p.cache.wstring()); argument(L"--sessions",p.sessions.wstring());
    argument(L"--input-config",p.input.wstring()); argument(L"--input-mode",p.mode);
    if (!p.graphics.empty()) argument(L"--graphics-config",p.graphics.wstring());
    if (!p.resolution.empty()) argument(L"--render-resolution",p.resolution);
    p.environment[L"HAZE_LAUNCHER_ARGUMENTS"]=p.steam_arguments;
    return p;
}
std::wstring validate(const Plan& p,std::wstring& repair_key) {
    repair_key.clear(); std::error_code ec;
    if (!fs::is_regular_file(p.executable,ec)) { repair_key=L"Executable"; return L"HazeRecomp.exe is missing. Build the native game or choose its executable.\n\n"+p.executable.wstring(); }
    if (fs::equivalent(p.executable,p.launcher,ec)) { repair_key=L"Executable"; return L"The game executable points to the launcher itself. Choose HazeRecomp.exe."; }
    DWORD binary_type=0;
    if (!GetBinaryTypeW(p.executable.c_str(),&binary_type) || binary_type!=SCS_64BIT_BINARY) {
        repair_key=L"Executable"; return L"The selected file is not a usable 64-bit Windows executable. Rebuild or choose HazeRecomp.exe.\n\n"+p.executable.wstring();
    }
    if (!fs::is_regular_file(p.elf,ec)) { repair_key=L"DiscDirectory"; return L"The decrypted Haze EBOOT.ELF is missing. Choose the disc folder containing PS3_GAME.\n\n"+p.elf.wstring(); }
    if (!fs::is_regular_file(p.disc/L"PS3_GAME/PARAM.SFO",ec)) { repair_key=L"DiscDirectory"; return L"The disc folder is incomplete: PS3_GAME/PARAM.SFO is missing."; }
    if (!fs::is_regular_file(p.firmware/L"sys/external/flashMP3.pic",ec)) { repair_key=L"Firmware"; return L"The PS3 firmware folder is missing sys/external/flashMP3.pic. Choose an extracted dev_flash folder.\n\n"+p.firmware.wstring(); }
    if (!fs::is_regular_file(p.input,ec)) return L"The input configuration is missing. Reinstall config/input.ini or configure InputConfig in launcher.ini.\n\n"+p.input.wstring();
    if (!p.graphics.empty() && !fs::is_regular_file(p.graphics,ec)) return L"The configured graphics INI is missing. Correct GraphicsConfig in launcher.ini.\n\n"+p.graphics.wstring();
    return {};
}
}
