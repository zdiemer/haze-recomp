#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace haze_launch {
namespace fs=std::filesystem;
struct Less { bool operator()(const std::wstring& a,const std::wstring& b) const { return _wcsicmp(a.c_str(),b.c_str())<0; } };
using Values=std::map<std::wstring,std::wstring,Less>;
struct Options { Values overrides; fs::path config; bool no_ui=false,check=false; };
struct Plan {
    fs::path launcher,config,root,shared,executable,disc,elf,firmware,saves,cache,input,graphics,sessions;
    Values environment;
    std::wstring mode,resolution;
    std::wstring steam_arguments;
};
struct Result { DWORD exit_code=1; bool started=false; fs::path session; std::wstring error; };
std::wstring wide(const std::string& utf8);
std::wstring error_text(const char* message);
std::string utf8(const std::wstring& wide);
std::wstring quote(const std::wstring& argument);
Values read_config(const fs::path& path,bool required=false);
Options parse(const std::vector<std::wstring>& args);
Values environment();
Plan resolve(const fs::path& launcher,const Options& options,const Values& inherited);
// Returns the setting that can repair a missing input (SharedRoot, Firmware,
// Executable), or an empty key for a non-picker error.
std::wstring validate(const Plan& plan,std::wstring& repair_key);
bool save_setting(const fs::path& config,const std::wstring& key,const std::wstring& value,std::wstring& error);
Result run(const Plan& plan);
void write_report(const Plan& plan,const Result& result);
}
