#include "haze_steam.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <stdexcept>

namespace steam_windows {
namespace fs=std::filesystem;
constexpr DWORD max_file=16*1024*1024;
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h) {}
    ~Handle() { if (value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    void close() { if (value!=INVALID_HANDLE_VALUE) CloseHandle(value); value=INVALID_HANDLE_VALUE; }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    explicit operator bool() const { return value!=INVALID_HANDLE_VALUE; }
};
std::string error(const char* action) { return std::string(action)+" (Windows error "+std::to_string(GetLastError())+")."; }
bool regular(const fs::path& path) {
    auto attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
bool directory(const fs::path& path) {
    auto attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY) && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
}
// Check the whole path, not just its last component. A junction inside userdata
// must not redirect a selected account's writes to a different account/location.
void plain_path(const fs::path& path) {
    if (!path.is_absolute()) throw std::runtime_error("Steam paths must be absolute.");
    fs::path prefix=path.root_path();
    for (const auto& part:path.relative_path()) {
        if (part==L".." || part==L".") throw std::runtime_error("Steam paths must be normalized.");
        prefix/=part;
        const auto attributes=GetFileAttributesW(prefix.c_str());
        if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Steam integration requires accessible folders without symbolic links or junctions.");
    }
}
void client_closed() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if (!snapshot) throw std::runtime_error(error("Cannot check whether Steam is running"));
    PROCESSENTRY32W process{}; process.dwSize=sizeof(process);
    if (!Process32FirstW(snapshot.value,&process)) throw std::runtime_error(error("Cannot inspect running processes"));
    do {
        if (!_wcsicmp(process.szExeFile,L"steam.exe") || !_wcsicmp(process.szExeFile,L"steamwebhelper.exe"))
            throw std::runtime_error("Exit Steam using Steam > Exit, wait for it to close, then retry. Haze will not stop Steam or any running game.");
    } while (Process32NextW(snapshot.value,&process));
    if (GetLastError()!=ERROR_NO_MORE_FILES) throw std::runtime_error(error("Cannot finish checking Steam processes"));
}
std::string read(HANDLE file) {
    LARGE_INTEGER size{},zero{};
    if (!GetFileSizeEx(file,&size) || size.QuadPart<1 || size.QuadPart>max_file)
        throw std::runtime_error("The existing shortcuts file is empty, unreadable or too large. No changes made.");
    if (!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)) throw std::runtime_error(error("Cannot seek shortcuts file"));
    std::string bytes(size_t(size.QuadPart),'\0'); DWORD got=0;
    if (!ReadFile(file,bytes.data(),DWORD(bytes.size()),&got,nullptr) || got!=bytes.size())
        throw std::runtime_error(error("Cannot read the complete shortcuts file"));
    return bytes;
}
void write_new(const fs::path& path,const std::string& bytes) {
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if (!file) throw std::runtime_error(error("Cannot create a Steam integration file"));
    DWORD written=0;
    if (!WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(file.value)) {
        const auto detail=error("Cannot finish writing a Steam integration file");
        file.close(); DeleteFileW(path.c_str());
        throw std::runtime_error(detail);
    }
}
std::wstring suffix() {
    static std::atomic<unsigned> serial{0};
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    return std::to_wstring((uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime)+L"-"+
        std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial);
}
std::wstring registry(HKEY hive,const wchar_t* key,const wchar_t* name,REGSAM view=0) {
    wchar_t value[32768]{}; DWORD size=sizeof(value);
    if (RegGetValueW(hive,key,name,RRF_RT_REG_SZ|view,nullptr,value,&size)!=ERROR_SUCCESS) return {};
    return value;
}
void profile_names(const fs::path& root,std::vector<HazeSteamProfile>& profiles) {
    // Optional labels only. A missing, locked or malformed cache never changes
    // profile identity, selection, or the ability to register by account ID.
    try {
        const auto path=root/L"config/loginusers.vdf";
        if (!regular(path)) return;
        plain_path(path);
        Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if (!file) return;
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(file.value,&info) || (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(file.value,&size) || size.QuadPart<1 || size.QuadPart>2*1024*1024) return;
        std::string bytes(size_t(size.QuadPart),'\0'); DWORD got=0;
        if (!ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&got,nullptr) || got!=bytes.size()) return;
        haze_steam_profile_names(bytes,profiles);
    } catch (const std::exception&) { /* Cached names are not required. */ }
}
std::vector<HazeSteamProfile> profiles_at(const fs::path& root) {
    plain_path(root);
    if (!regular(root/L"steam.exe")) throw std::runtime_error("The Steam installation is missing steam.exe.");
    const auto userdata=root/L"userdata"; plain_path(userdata);
    std::vector<HazeSteamProfile> profiles;
    for (const auto& entry:fs::directory_iterator(userdata)) {
        const auto name=entry.path().filename().string();
        uint32_t id=0; auto parsed=std::from_chars(name.data(),name.data()+name.size(),id);
        if (parsed.ec!=std::errc{} || parsed.ptr!=name.data()+name.size() || !id || std::to_string(id)!=name) continue;
        if (!directory(entry.path()) || !directory(entry.path()/L"config")) continue;
        profiles.push_back({root,entry.path()/L"config",name,{}});
    }
    std::sort(profiles.begin(),profiles.end(),[](const auto& a,const auto& b){return a.account_id<b.account_id;});
    profile_names(root,profiles);
    return profiles;
}
void validate_profile(const HazeSteamProfile& profile) {
    const auto found=profiles_at(profile.steam_root);
    for (const auto& item:found) if (item.account_id==profile.account_id && item.config_directory==profile.config_directory) {
        plain_path(profile.config_directory); return;
    }
    throw std::runtime_error("The selected Steam account folder changed or no longer exists. Select the profile again.");
}
}

std::vector<HazeSteamProfile> haze_steam_profiles(std::string& message) {
    using namespace steam_windows;
    message.clear();
    try {
        auto root=registry(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath");
        if (root.empty()) root=registry(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Valve\\Steam",L"InstallPath",RRF_SUBKEY_WOW6432KEY);
        if (root.empty()) root=registry(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Valve\\Steam",L"InstallPath",RRF_SUBKEY_WOW6464KEY);
        if (root.empty()) throw std::runtime_error("Steam was not found. Install Steam and sign in once, then retry. Haze can still be played normally.");
        auto profiles=profiles_at(fs::path(root).lexically_normal());
        if (profiles.empty()) throw std::runtime_error("No local Steam profiles were found. Sign in to Steam once, exit Steam, then retry.");
        return profiles;
    } catch (const std::exception& e) { message=e.what(); return {}; }
}

HazeSteamResult haze_steam_install(const HazeSteamProfile& profile,const HazeSteamTarget& target) {
    using namespace steam_windows;
    HazeSteamResult result;
    fs::path temporary;
    try {
        validate_profile(profile);
        if (!regular(target.launcher) || !directory(target.working_directory) || !regular(target.icon))
            throw std::runtime_error("The executable launcher or icon is missing. Install/build the Haze launcher first, then retry.");
        client_closed();
        // Deny new opens of the actual client executable during the transaction.
        // A process check alone would allow Steam to start between check/write.
        Handle client(CreateFileW((profile.steam_root/L"steam.exe").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr));
        if (!client) throw std::runtime_error("Steam is running, starting, or its executable is busy. Exit Steam and retry after it has closed.");
        client_closed();
        const auto lock_path=profile.config_directory/L".haze-recomp-shortcuts.lock";
        const auto lock_attributes=GetFileAttributesW(lock_path.c_str());
        if (lock_attributes!=INVALID_FILE_ATTRIBUTES && (lock_attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("The Steam integration lock is not a regular file. No changes made.");
        Handle lock(CreateFileW(lock_path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if (!lock) throw std::runtime_error("Another Haze Steam update is in progress, or the account folder is not writable. Retry when it finishes.");
        const auto destination=profile.config_directory/L"shortcuts.vdf";
        const auto attributes=GetFileAttributesW(destination.c_str());
        const bool existed=attributes!=INVALID_FILE_ATTRIBUTES;
        if (!existed && GetLastError()!=ERROR_FILE_NOT_FOUND) throw std::runtime_error(error("Cannot inspect shortcuts file"));
        if (existed && !regular(destination)) throw std::runtime_error("The Steam shortcuts file is not a regular file. No changes made.");
        Handle original(existed ? CreateFileW(destination.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr) : INVALID_HANDLE_VALUE);
        if (existed && !original) throw std::runtime_error("Steam shortcuts are open for writing or unreadable. Close the writer and retry.");
        const auto before=existed ? read(original.value) : std::string{};
        HazeSteamPlan plan;
        if (!haze_steam_plan(before,target,plan,result.message)) return result;
        result.appid=plan.appid;
        const auto artwork=[&] {
            const auto art=haze_steam_artwork(profile.config_directory,plan.appid,target.artwork_directory);
            result.changed=result.changed || art.installed!=0;
            if (!art.warning.empty()) result.message+=" Artwork incomplete: "+art.warning;
            else result.message+=" Library artwork: "+std::to_string(art.installed)+" installed, "+std::to_string(art.preserved)+" existing images retained.";
        };
        if (before==plan.after) { result.success=true; result.message="Haze Recomp is already registered in this Steam profile. Start Steam to play."; artwork(); return result; }
        const auto unique=suffix();
        const auto candidate=profile.config_directory/(L".haze-shortcuts-"+unique+L".tmp");
        write_new(candidate,plan.after); temporary=candidate;
        if (existed) {
            const auto backup=profile.config_directory/(L"shortcuts.vdf.haze-backup-"+unique);
            write_new(backup,before); result.backup=backup;
        }
        client_closed();
        // Re-read the name too: another editor could have replaced the file while
        // our original read handle remained valid. Never overwrite that change.
        if (existed) {
            Handle current(CreateFileW(destination.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
            if (!current || read(current.value)!=before) throw std::runtime_error("Steam shortcuts changed while preparing the update. Retry; the changed file was left untouched.");
        } else if (GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES || GetLastError()!=ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("A Steam shortcuts file appeared while preparing the update. Retry; it was left untouched.");
        // Windows replacement may need the target's read handle closed. The
        // client executable gate and our account lock remain held across rename.
        original.close();
        // Sibling rename, never a truncate/rewrite. Missing-file installation does
        // not use REPLACE_EXISTING, so a concurrent first file cannot be lost.
        if (!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH|(existed ? MOVEFILE_REPLACE_EXISTING : 0)))
            throw std::runtime_error(error("Cannot atomically install Steam shortcuts; the original file was retained"));
        temporary.clear();
        result.success=true; result.changed=true;
        result.message=plan.created ? "Haze Recomp was added. Start Steam to see it in the selected library." :
            "The Haze Recomp shortcut was repaired. Its library ID, categories and custom artwork were retained. Start Steam to play.";
        artwork();
    } catch (const std::exception& e) { result.message=e.what(); }
    if (!temporary.empty()) DeleteFileW(temporary.c_str());
    return result;
}
#else
std::vector<HazeSteamProfile> haze_steam_profiles(std::string& message) { message="Add to Steam is available in the native Windows build."; return {}; }
HazeSteamResult haze_steam_install(const HazeSteamProfile&,const HazeSteamTarget&) { return {false,false,0,{},"Add to Steam is available in the native Windows build."}; }
#endif
