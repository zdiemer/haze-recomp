#include "haze_player_profile.h"
#include <fstream>
#include <mutex>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
std::mutex profile_mutex;
std::filesystem::path profile_file;
std::string player_name="User",load_error;
bool letter(unsigned char c) { return (c>='A' && c<='Z') || (c>='a' && c<='z'); }
}
bool haze_player_name_valid(const std::string& name,std::string& error) {
    if (name.size()<3 || name.size()>16) { error="Use 3 to 16 characters."; return false; }
    if (!letter(static_cast<unsigned char>(name.front()))) { error="Start your name with a letter (A-Z)."; return false; }
    for (unsigned char c:name) if (!letter(c) && !(c>='0' && c<='9') && c!='_' && c!='-') {
        error="Use only letters A-Z, numbers, underscores (_) and hyphens (-)."; return false;
    }
    error.clear(); return true;
}
void haze_player_profile_initialize(const std::filesystem::path& save_root) {
    std::lock_guard lock(profile_mutex);
    profile_file=save_root/"player-name.txt"; player_name="User"; load_error.clear();
    std::error_code ec;
    const auto exists=std::filesystem::exists(profile_file,ec);
    if (!ec && !exists) return;
    const auto size=std::filesystem::file_size(profile_file,ec);
    if (ec || size>18) { load_error="Cannot read the saved profile name. Choose a name and Apply to repair it."; return; }
    std::ifstream file(profile_file,std::ios::binary);
    std::string name(size_t(size),'\0');
    if (!file.read(name.data(),std::streamsize(size))) { load_error="Cannot read the saved profile name."; return; }
    if (!name.empty() && name.back()=='\n') name.pop_back();
    if (!name.empty() && name.back()=='\r') name.pop_back();
    if (haze_player_name_valid(name,load_error)) player_name=std::move(name);
    else load_error="Saved profile name is invalid. "+load_error;
}
std::string haze_player_name() { std::lock_guard lock(profile_mutex); return player_name; }
std::string haze_player_profile_error() { std::lock_guard lock(profile_mutex); return load_error; }
bool haze_player_name_save(const std::string& name,std::string& error) {
    if (!haze_player_name_valid(name,error)) return false;
    std::lock_guard lock(profile_mutex);
    if (profile_file.empty()) { error="Profile storage is not ready."; return false; }
    std::error_code ec;
    std::filesystem::create_directories(profile_file.parent_path(),ec);
    if (ec) { error="Cannot create the profile folder. Your name is unchanged."; return false; }
#ifdef _WIN32
    // Exclusive per-process staging, flushed before atomic replacement. A
    // failed write never publishes a new name or touches campaign save data.
    const auto temporary=profile_file.wstring()+L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) { error="Cannot write the profile name. Your name is unchanged."; return false; }
    const auto bytes=name+"\n"; DWORD written=0;
    bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr) && written==bytes.size() && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok=false;
    if (ok) ok=MoveFileExW(temporary.c_str(),profile_file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if (!ok) { DeleteFileW(temporary.c_str()); error="Cannot save the profile name. Your name is unchanged."; return false; }
#else
    error="Profile-name saving is not supported on this platform."; return false;
#endif
    player_name=name; load_error.clear(); error.clear(); return true;
}
