#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace haze_launch {
namespace fs=std::filesystem;
struct Less { bool operator()(const std::wstring& a,const std::wstring& b) const { return _wcsicmp(a.c_str(),b.c_str())<0; } };
using Values=std::map<std::wstring,std::wstring,Less>;
struct Options { Values overrides; fs::path config,export_to; std::wstring session; bool no_ui=false,check=false,diagnostics=false; };
struct Plan {
    fs::path launcher,config,root,shared,executable,disc,elf,firmware,saves,cache,input,graphics,sessions;
    Values environment,sources;
    std::wstring mode,resolution;
    std::wstring steam_arguments;
    std::vector<std::wstring> problems; // tolerant resolution only
};
struct Result { DWORD exit_code=1; bool started=false,finished=false; fs::path session; std::wstring error; };
std::wstring wide(const std::string& utf8);
std::wstring error_text(const char* message);
std::string utf8(const std::wstring& wide);
std::wstring quote(const std::wstring& argument);
Values read_config(const fs::path& path,bool required=false,std::vector<std::wstring>* problems=nullptr);
Options parse(const std::vector<std::wstring>& args);
Values environment();
// tolerant records configuration errors in Plan::problems and keeps the raw or
// default value, so diagnostics can export with an invalid configuration.
Plan resolve(const fs::path& launcher,const Options& options,const Values& inherited,bool tolerant=false);
// Returns the setting that can repair a missing input (SharedRoot, Firmware,
// Executable), or an empty key for a non-picker error.
std::wstring validate(const Plan& plan,std::wstring& repair_key);
bool save_setting(const fs::path& config,const std::wstring& key,const std::wstring& value,std::wstring& error);
Result run(const Plan& plan);
void write_report(const Plan& plan,const Result& result);
std::wstring serial();
std::wstring windows_error(const wchar_t* action,DWORD code=GetLastError());
// Resolved launcher settings, in documented order, and the diagnostics-relevant
// variables (HAZE_/RSX_/PS3_/SDL_/YDKJ_) recorded in reports.
std::vector<std::pair<std::wstring,std::wstring>> settings(const Plan& plan);
bool diagnostic_variable(const std::wstring& name);
// License keys, console/online IDs and credential-like names: never recorded.
bool sensitive_variable(const std::wstring& name);

// Diagnostics export. Collection is independent of the launcher's dialogs.
struct Limits { uintmax_t file=4u<<20,head=1u<<20,config=256u<<10,total=32u<<20; size_t files=32; };
struct Entry { std::string name,data; };
struct Session { fs::path path; std::wstring name; bool record=false,started=false,finished=false,has_finished=false; DWORD exit_code=0; std::string error; };
struct Redactor { std::vector<std::pair<std::string,std::string>> rules; };
Redactor redactor(const Plan& plan,const Values& environment);
std::string redact(const std::string& text,const Redactor& redactor);
// Reads at most limit bytes: the first head bytes and the remaining tail.
// note describes what was kept, or starts with "missing"/"unreadable".
std::string bounded(const fs::path& file,uintmax_t limit,uintmax_t head,std::string& note);
Session session(const fs::path& folder);
std::vector<Session> sessions(const fs::path& root,size_t limit=16);
fs::path find_session(const Plan& plan,const std::wstring& name);
std::string describe(const Session& session);
std::string system_report();
// Executable/EBOOT SHA-256, PE and PDB identity and PARAM.SFO fields. The
// launcher records this in each session so later exports name the right build.
std::string build_identity(const Plan& plan,const std::string& prefix);
void record_identity(const Plan& plan,const fs::path& session);
std::vector<Entry> collect(const Plan& plan,const Session& session,const Values& environment,const std::string& system,const Limits& limits={});
std::string zip(const std::vector<Entry>& entries);
// Never replaces an existing file: picks base.zip, base-2.zip, ...
fs::path save_report(const std::string& bytes,const fs::path& folder,const std::wstring& base,std::wstring& error);
fs::path export_diagnostics(const Plan& plan,const fs::path& session,const fs::path& folder,const Values& environment,std::wstring& error);
}
