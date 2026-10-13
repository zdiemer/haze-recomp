#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <fstream>
#include <cstdio>
#include <filesystem>
#include <thread>
#include "../port/haze_steam_vdf.cpp"
#include "../port/haze_steam_art.cpp"
static bool fail_replace=false,probe_client=false,client_blocked=false,fail_backup=false,fail_stage=false;
static std::filesystem::path client_path;
static BOOL write_file(HANDLE file,LPCVOID data,DWORD count,LPDWORD written,LPOVERLAPPED overlapped) {
    wchar_t path[32768]{}; GetFinalPathNameByHandleW(file,path,32768,0);
    const std::wstring name(path);
    if ((fail_backup && name.find(L"haze-backup")!=std::wstring::npos) ||
        (fail_stage && name.find(L".tmp")!=std::wstring::npos)) {
        WriteFile(file,data,count/2,written,overlapped); SetLastError(ERROR_DISK_FULL); return FALSE;
    }
    return WriteFile(file,data,count,written,overlapped);
}
static BOOL replace_file(LPCWSTR from,LPCWSTR to,DWORD flags) {
    if (probe_client) {
        auto command=L'"'+client_path.wstring()+L"\" --probe";
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
        auto launched=CreateProcessW(client_path.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);
        client_blocked=!launched;
        if (launched) { WaitForSingleObject(process.hProcess,5000); CloseHandle(process.hProcess); CloseHandle(process.hThread); }
    }
    if (fail_replace) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return MoveFileExW(from,to,flags);
}
#define MoveFileExW replace_file
#define WriteFile write_file
#include "../port/haze_steam_win32.cpp"
#undef MoveFileExW
#undef WriteFile
namespace fs=std::filesystem;
static std::string bytes(const fs::path& path) { std::ifstream f(path,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
static void put(const fs::path& path,const std::string& text) { std::ofstream f(path,std::ios::binary); f.write(text.data(),text.size()); assert(f.good()); }
static steam_vdf::Node entry(const std::string& value) { steam_vdf::Reader r{value}; return r.object(0).at(0); }
static std::string serialize(steam_vdf::Node root) { std::string s; steam_vdf::write({root},s); return s; }
int main(int argc,char** argv) {
    if (argc==3 && std::string(argv[1])=="--audit-file") {
        const auto before=bytes(fs::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2]))));
        HazeSteamPlan plan; std::string message;
        HazeSteamTarget target{L"C:/Haze audit fixture/HazeLauncher.exe",L"C:/Haze audit fixture",L"C:/Haze audit fixture/HazeLauncher.exe",{}, {}};
        if (before.empty() || !haze_steam_plan(before,target,plan,message)) { std::fprintf(stderr,"Audit failed: %s\n",message.c_str()); return 1; }
        const auto original=entry(before),updated=entry(plan.after);
        for (const auto& prior:original.children) {
            auto found=std::find_if(updated.children.begin(),updated.children.end(),[&](const auto& n){return n.key==prior.key;});
            if (found==updated.children.end()) return 2;
            std::string a,b; steam_vdf::write({prior},a); steam_vdf::write({*found},b); if (a!=b) return 3;
        }
        if (!plan.created || updated.children.size()!=original.children.size()+1) return 4;
        std::printf("External fixture audit: %zu bytes, %zu unrelated entries preserved exactly; one Haze entry added in memory. No files written.\n",before.size(),original.children.size());
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--audit-live") {
        // Read-only compatibility audit. Do not print profile IDs or shortcut
        // contents, and never invoke the installer on a real account here.
        std::string message;
        const auto profiles=haze_steam_profiles(message);
        if (profiles.empty()) { std::fprintf(stderr,"Discovery failed: %s\n",message.c_str()); return 1; }
        unsigned existing=0; size_t inspected=0;
        for (const auto& profile:profiles) {
            const auto file=profile.config_directory/L"shortcuts.vdf";
            if (!fs::exists(file)) continue;
            const auto before=bytes(file);
            steam_vdf::Reader reader{before};
            try {
                auto root=reader.object(0); std::string roundtrip; steam_vdf::write(root,roundtrip);
                if (reader.offset!=before.size() || roundtrip!=before) return 2;
                HazeSteamPlan plan;
                HazeSteamTarget target{L"C:/Haze audit fixture/HazeLauncher.exe",L"C:/Haze audit fixture",L"C:/Haze audit fixture/HazeLauncher.exe",{}, {}};
                if (!haze_steam_plan(before,target,plan,message)) { std::fprintf(stderr,"Plan refused: %s\n",message.c_str()); return 3; }
                auto updated=entry(plan.after);
                if (plan.created) {
                    for (const auto& original:root.at(0).children) {
                        auto found=std::find_if(updated.children.begin(),updated.children.end(),[&](const auto& n){return n.key==original.key;});
                        if (found==updated.children.end()) return 4;
                        std::string old_bytes,new_bytes; steam_vdf::write({original},old_bytes); steam_vdf::write({*found},new_bytes);
                        if (old_bytes!=new_bytes) return 5;
                    }
                }
                ++existing; inspected+=before.size();
                if (bytes(file)!=before) { std::puts("The client changed a file during the read-only audit; retry when closed."); return 6; }
            } catch (const std::exception& e) { std::fprintf(stderr,"Audit failed: %s\n",e.what()); return 7; }
        }
        std::printf("Read-only Steam audit: %zu profiles discovered; %u existing shortcut files, %zu bytes parsed and round-tripped exactly. No files written.\n",profiles.size(),existing,inspected);
        return 0;
    }
    if (argc==4 && std::string(argv[1])=="--hold") {
        steam_windows::Handle stop(OpenEventA(SYNCHRONIZE,FALSE,argv[2]));
        steam_windows::Handle ready(OpenEventA(EVENT_MODIFY_STATE,FALSE,argv[3]));
        if (!stop || !ready) return 2;
        SetEvent(ready.value); return WaitForSingleObject(stop.value,10000)==WAIT_OBJECT_0 ? 0 : 3;
    }
    if (argc>1) return 0; // copied executable used to check Windows loader exclusion
    const auto root=fs::absolute("local/steam-test-"+std::to_string(GetCurrentProcessId()));
    fs::create_directories(root/"userdata/42/config"); fs::create_directories(root/"userdata/7/config");
    fs::create_directories(root/"userdata/not-an-account/config"); fs::create_directories(root/"userdata/0/config");
    fs::create_directories(root/L"Haze install \u00e9 space");
    wchar_t self[32768]{}; assert(GetModuleFileNameW(nullptr,self,32768));
    client_path=root/L"steam.exe"; assert(CopyFileW(self,client_path.c_str(),TRUE));
    HazeSteamTarget target{root/L"Haze install \u00e9 space/HazeLauncher.exe",root/L"Haze install \u00e9 space",root/L"Haze install \u00e9 space/HazeLauncher.exe",{}, {}};
    put(target.launcher,"launcher fixture");
    HazeSteamPlan plan; std::string message;
    assert(haze_steam_plan({},target,plan,message) && plan.created && plan.appid);
    auto original=plan.after;
    assert(haze_steam_plan(original,target,plan,message) && !plan.created && plan.after==original);
    const uint32_t id=plan.appid;
    auto data=entry(original);
    auto& owned=data.children[0];
    assert(steam_vdf::string(owned,"Exe").find("Haze install \xc3\xa9 space")!=std::string::npos);
    assert(steam_vdf::string(owned,"Exe").front()=='"' && steam_vdf::string(owned,"Exe").back()=='"');
    steam_vdf::set_string(owned,"AppName","My Haze");
    steam_vdf::set_integer(owned,"LastPlayTime",1234567);
    steam_vdf::set_string(owned,"icon","D:/custom art/my icon.ico");
    steam_vdf::set_string(*steam_vdf::find(owned,"tags"),"1","My favorites");
    // Non-Haze entry has mixed key casing, unknown scalar fields and nested tags.
    steam_vdf::Node other{0,"7",{}, {}};
    steam_vdf::set_integer(other,"AppID",0xabcdef01); steam_vdf::set_string(other,"AppName","Keep this other game");
    steam_vdf::set_string(other,"exe","\"C:/Other/Game.exe\"");
    steam_vdf::set(other,"unknownFloat",3,std::string("\x80\x00\x00\x40",4));
    steam_vdf::set(other,"unknownColor",6,std::string("\x08\x00\x01\xff",4));
    steam_vdf::set(other,"unknownU64",7,std::string("\x00\x08\x00\x08\x01\x02\x03\x04",8));
    other.children.push_back({0,"unknownObject",{},{{1,"field",std::string("keep\0",5),{}}}});
    std::string other_bytes; steam_vdf::write({other},other_bytes); other_bytes.pop_back();
    data.children.insert(data.children.begin(),other);
    const auto customized=serialize(data);
    auto moved=target; fs::create_directories(root/"Moved install");
    moved.launcher=root/"Moved install/HazeLauncher.exe"; moved.working_directory=root/"Moved install"; moved.icon=moved.launcher;
    put(moved.launcher,"new launcher fixture");
    assert(haze_steam_plan(customized,moved,plan,message) && plan.appid==id && !plan.created);
    assert(plan.after.find(other_bytes)!=std::string::npos);
    auto repaired=entry(plan.after); auto& fixed=repaired.children[1];
    assert(steam_vdf::string(fixed,"AppName")=="My Haze");
    assert(steam_vdf::string(fixed,"icon")=="D:/custom art/my icon.ico");
    assert(steam_vdf::integer(fixed,"LastPlayTime")==1234567);
    assert(steam_vdf::string(*steam_vdf::find(fixed,"tags"),"1")=="My favorites");
    assert(steam_vdf::string(fixed,"Exe")=="\""+steam_vdf::path(moved.launcher)+"\"");
    for (size_t i=1;i<customized.size();++i) assert(!haze_steam_plan(customized.substr(0,i),target,plan,message) && plan.after.empty());
    assert(!haze_steam_plan(customized+"junk",target,plan,message));
    auto duplicate=data; duplicate.children.push_back(duplicate.children[1]); duplicate.children.back().key="9";
    assert(!haze_steam_plan(serialize(duplicate),target,plan,message));
    duplicate=data; duplicate.children[0].children.push_back({1,"appname",std::string("duplicate\0",10),{}});
    assert(!haze_steam_plan(serialize(duplicate),target,plan,message));
    auto unsupported=data; unsupported.children[0].children[0].type=5;
    assert(!haze_steam_plan(serialize(unsupported),target,plan,message));
    auto unmanaged=entry(original); steam_vdf::find(unmanaged.children[0],"tags")->children.clear();
    assert(!haze_steam_plan(serialize(unmanaged),target,plan,message) && message.find("not managed")!=std::string::npos);
    auto colliding=data; colliding.children.erase(colliding.children.begin()+1); steam_vdf::set_integer(colliding.children[0],"appid",id);
    assert(!haze_steam_plan(serialize(colliding),target,plan,message) && message.find("collides")!=std::string::npos);
    auto unrelated_duplicates=data; unrelated_duplicates.children.push_back(other); unrelated_duplicates.children.back().key="99";
    assert(haze_steam_plan(serialize(unrelated_duplicates),moved,plan,message));
    auto managed_collision=data; steam_vdf::set_integer(managed_collision.children[0],"appid",id);
    assert(!haze_steam_plan(serialize(managed_collision),moved,plan,message) && message.find("shares an ID")!=std::string::npos);
    auto bad=target; bad.launcher="relative.exe"; assert(!haze_steam_plan({},bad,plan,message));
    bad=target; bad.arguments=std::string("bad\0args",8); assert(!haze_steam_plan({},bad,plan,message));

    const auto profiles=steam_windows::profiles_at(root); assert(profiles.size()==2 && profiles[0].account_id=="42" && profiles[1].account_id=="7");
    const auto profile=profiles[0]; const auto file=profile.config_directory/L"shortcuts.vdf";
    put(file,customized);
    probe_client=true;
    auto installed=haze_steam_install(profile,moved);
    if (!installed.success) std::fprintf(stderr,"Install failed: %s\n",installed.message.c_str());
    assert(installed.success && installed.changed && installed.appid==id && client_blocked);
    assert(installed.message.find("Artwork incomplete")!=std::string::npos);
    assert(bytes(installed.backup)==customized);
    const auto final=bytes(file);
    assert(final.find(other_bytes)!=std::string::npos);
    auto repeat=haze_steam_install(profile,moved); assert(repeat.success && !repeat.changed && repeat.backup.empty() && bytes(file)==final);
    auto missing=moved; missing.launcher=root/L"does-not-exist.exe";
    assert(!haze_steam_install(profile,missing).success && bytes(file)==final);
    // Denied atomic replacement retains original + recovery backup; no temp leaks.
    fail_replace=true; auto failure=haze_steam_install(profile,target); fail_replace=false;
    assert(!failure.success && bytes(file)==final && bytes(failure.backup)==final);
    for (const auto& f:fs::directory_iterator(profile.config_directory)) assert(f.path().extension()!=L".tmp");
    fail_stage=true; failure=haze_steam_install(profile,target); fail_stage=false;
    assert(!failure.success && failure.backup.empty() && bytes(file)==final);
    const auto count_before=std::distance(fs::directory_iterator(profile.config_directory),fs::directory_iterator{});
    fail_backup=true; failure=haze_steam_install(profile,target); fail_backup=false;
    assert(!failure.success && failure.backup.empty() && bytes(file)==final);
    assert(std::distance(fs::directory_iterator(profile.config_directory),fs::directory_iterator{})==count_before);
    {
        steam_windows::Handle busy(CreateFileW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
        assert(busy && !haze_steam_install(profile,target).success && bytes(file)==final);
    }
    {
        steam_windows::Handle lock(CreateFileW((profile.config_directory/L".haze-recomp-shortcuts.lock").c_str(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_DELETE_ON_CLOSE,nullptr));
        assert(lock && !haze_steam_install(profile,target).success && bytes(file)==final);
    }
    auto wrong=profile; wrong.config_directory=profiles[1].config_directory;
    assert(!haze_steam_install(wrong,target).success && bytes(file)==final);
    // A real owned child named steam.exe exercises process enumeration. It exits
    // through its event; no user Steam process is signalled or terminated.
    const auto event_name="Local\\HazeSteamFixture"+std::to_string(GetCurrentProcessId());
    steam_windows::Handle stop(CreateEventA(nullptr,TRUE,FALSE,(event_name+"Stop").c_str()));
    steam_windows::Handle ready(CreateEventA(nullptr,TRUE,FALSE,(event_name+"Ready").c_str()));
    assert(stop && ready);
    std::string command="\""+client_path.string()+"\" --hold "+event_name+"Stop "+event_name+"Ready";
    STARTUPINFOA startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION child{};
    assert(CreateProcessA(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child));
    assert(WaitForSingleObject(ready.value,5000)==WAIT_OBJECT_0);
    auto running=haze_steam_install(profile,target);
    SetEvent(stop.value); assert(WaitForSingleObject(child.hProcess,5000)==WAIT_OBJECT_0);
    DWORD code=1; assert(GetExitCodeProcess(child.hProcess,&code) && code==0);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    assert(!running.success && running.message.find("Exit Steam")!=std::string::npos && bytes(file)==final);
    auto fresh=haze_steam_install(profiles[1],target); assert(fresh.success && fresh.changed && fresh.backup.empty());
    put(file,""); assert(!haze_steam_install(profile,target).success && bytes(file).empty());
    put(file,"corrupt"); assert(!haze_steam_install(profile,target).success && bytes(file)=="corrupt");
    // No Steam library is touched: all paths are children of this PID's fixture.
    fs::remove_all(root);
    std::puts("Steam: bounded VDF parsing, unrelated-data preservation, duplicate/collision refusal, relocation, profile selection, launcher checks, running-client/start exclusion, short-write cleanup, backup and atomic failure checks passed.");
}
