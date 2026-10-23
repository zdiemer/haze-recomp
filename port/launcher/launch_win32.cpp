#include "launch.h"
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace haze_launch {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h) {}
    ~Handle() { close(); }
    void close() { if (value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); value=INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return value && value!=INVALID_HANDLE_VALUE; }
    Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
};
std::wstring windows_error(const wchar_t* action,DWORD code) {
    wchar_t* text=nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,(wchar_t*)&text,0,nullptr);
    std::wstring result=std::wstring(action)+L" (Windows error "+std::to_wstring(code)+L").";
    if (text) { result+=L"\n"; result+=text; LocalFree(text); } return result;
}
static std::string contents(const fs::path& path) {
    std::ifstream file(path,std::ios::binary); if (!file) throw std::runtime_error("Cannot read existing launcher configuration.");
    return {std::istreambuf_iterator<char>(file),{}};
}
std::wstring serial() {
    static std::atomic<unsigned> sequence{0}; FILETIME time{}; GetSystemTimeAsFileTime(&time);
    return std::to_wstring((uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime)+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence);
}
bool save_setting(const fs::path& config,const std::wstring& key,const std::wstring& value,std::wstring& error) {
    fs::path temporary;
    try {
        if (value.find_first_of(L"\r\n")!=value.npos || value.find(L'\0')!=value.npos) throw std::runtime_error("Invalid configuration value.");
        // Validate the current document before retaining its untouched lines.
        read_config(config);
        const bool exists=fs::exists(config); const auto before=exists ? contents(config) : std::string{};
        std::wstring text=wide(before),output; bool in_section=false,had_section=false,replaced=false;
        size_t offset=0;
        auto append=[&] { if (!replaced) { output+=key+L"="+value+L"\r\n"; replaced=true; } };
        while (offset<text.size()) {
            auto end=text.find(L'\n',offset); if (end==text.npos) end=text.size();
            auto line=text.substr(offset,end-offset); if (line.ends_with(L'\r')) line.pop_back(); offset=end+1;
            const auto first=line.find_first_not_of(L" \t\ufeff");
            const auto stripped=first==line.npos ? std::wstring{} : line.substr(first);
            if (!stripped.empty() && stripped[0]==L'[') {
                if (in_section) append();
                auto name=stripped; while (!name.empty() && (name.back()==L' ' || name.back()==L'\t')) name.pop_back();
                in_section=!_wcsicmp(name.c_str(),L"[Launcher]"); had_section=had_section || in_section;
            }
            if (in_section && !stripped.empty() && stripped[0]!=L';' && stripped[0]!=L'#') {
                const auto separator=stripped.find(L'=');
                if (separator!=stripped.npos) {
                    auto name=stripped.substr(0,separator); while (!name.empty() && (name.back()==L' ' || name.back()==L'\t')) name.pop_back();
                    if (!_wcsicmp(name.c_str(),key.c_str())) { append(); continue; }
                }
            }
            output+=line+L"\r\n";
        }
        if (!had_section) output+=L"[Launcher]\r\n";
        append();
        const auto bytes=utf8(output);
        const auto candidate=fs::path(config.wstring()+L".tmp-"+serial());
        Handle staged(CreateFileW(candidate.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if (!staged) throw std::runtime_error(utf8(windows_error(L"Cannot save launcher configuration. Use a writable installation folder")));
        temporary=candidate; DWORD written=0;
        if (!WriteFile(staged.value,bytes.data(),DWORD(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(staged.value))
            throw std::runtime_error("Could not finish writing launcher configuration; the original was retained.");
        staged.close();
        if (fs::exists(config)!=exists || (exists && contents(config)!=before)) throw std::runtime_error("Launcher configuration changed. Retry after closing the other editor.");
        if (!MoveFileExW(temporary.c_str(),config.c_str(),MOVEFILE_WRITE_THROUGH|(exists ? MOVEFILE_REPLACE_EXISTING : 0)))
            throw std::runtime_error(utf8(windows_error(L"Cannot replace launcher configuration; the original was retained")));
        temporary.clear(); return true;
    } catch (const std::exception& e) { error=error_text(e.what()); }
    if (!temporary.empty()) DeleteFileW(temporary.c_str()); return false;
}
void write_report(const Plan& p,const Result& r) {
    std::ofstream file(r.session/L"launcher.log",std::ios::binary|std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot create launcher.log in the session folder.");
    auto path=[&](const char* label,const fs::path& value) { file<<label<<"="<<utf8(value.wstring())<<"\n"; };
    path("Launcher",p.launcher); path("Configuration",p.config); path("InstallRoot",p.root); path("SharedRoot",p.shared);
    path("Executable",p.executable); path("ELF",p.elf); path("Firmware",p.firmware); path("Savedata",p.saves);
    path("ShaderCache",p.cache); path("InputConfig",p.input); path("GraphicsConfig",p.graphics); path("Session",r.session);
    file<<"InputMode="<<utf8(p.mode)<<"\nRenderResolution="<<utf8(p.resolution)<<"\nStarted="<<r.started<<"\nFinished="<<r.finished<<"\nExitCode="<<r.exit_code<<"\nError="<<utf8(r.error)<<"\n";
    for (const auto& [key,source]:p.sources) file<<"Source."<<utf8(key)<<"="<<utf8(source)<<"\n";
    for (const auto& [key,value]:p.environment) if (diagnostic_variable(key)) file<<"Environment."<<utf8(key)<<"="<<(sensitive_variable(key) ? std::string("<redacted>") : utf8(value))<<"\n";
    file.flush(); if (!file) throw std::runtime_error("Cannot finish writing launcher.log.");
}
fs::path save_report(const std::string& bytes,const fs::path& folder,const std::wstring& base,std::wstring& error) {
    try {
        std::error_code ec; fs::create_directories(folder,ec);
        if (!fs::is_directory(folder,ec)) throw std::runtime_error("The diagnostics destination is not a usable folder: "+utf8(folder.wstring()));
        if (bytes.size()>0xffffffffu) throw std::runtime_error("The diagnostics report is too large.");
        for (unsigned n=1;n<1000;++n) {
            const auto target=folder/(base+(n>1 ? L"-"+std::to_wstring(n) : L"")+L".zip");
            if (fs::exists(target,ec) || ec) continue;
            // Stage beside the destination, then publish without replacement:
            // an existing or concurrently created report is never overwritten.
            const auto temporary=folder/(base+L".partial-"+serial());
            Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            if (!file) throw std::runtime_error(utf8(windows_error(L"Cannot create a diagnostics report in this folder. Choose a writable folder")));
            DWORD written=0;
            if (!WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(file.value)) {
                const auto code=GetLastError(); file.close(); DeleteFileW(temporary.c_str());
                throw std::runtime_error(utf8(windows_error(L"Could not finish writing the diagnostics report; no partial report was kept",code ? code : ERROR_WRITE_FAULT)));
            }
            file.close();
            if (MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)) return target;
            const auto code=GetLastError(); DeleteFileW(temporary.c_str());
            if (code!=ERROR_ALREADY_EXISTS && code!=ERROR_FILE_EXISTS) throw std::runtime_error(utf8(windows_error(L"Cannot finish the diagnostics report",code)));
        }
        throw std::runtime_error("Too many diagnostics reports for this session already exist in that folder.");
    } catch (const std::exception& e) { error=error_text(e.what()); }
    return {};
}
struct Attributes {
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list=nullptr;
    explicit Attributes(const std::vector<HANDLE>& handles) {
        SIZE_T size=0; InitializeProcThreadAttributeList(nullptr,1,0,&size); storage.resize(size);
        list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list,1,0,&size)) { list=nullptr; throw std::runtime_error("Cannot initialize child process handles."); }
        if (!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,const_cast<HANDLE*>(handles.data()),handles.size()*sizeof(HANDLE),nullptr,nullptr)) {
            DeleteProcThreadAttributeList(list); list=nullptr; throw std::runtime_error("Cannot restrict child process handle inheritance.");
        }
    }
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
};
Result run(const Plan& p) {
    Result result;
    try {
        std::wstring repair; const auto problem=validate(p,repair);
        if (!problem.empty()) { result.error=problem; return result; }
        fs::create_directories(p.saves); fs::create_directories(p.cache); fs::create_directories(p.sessions);
        SYSTEMTIME time{}; GetSystemTime(&time); wchar_t date[40]{};
        swprintf_s(date,L"%04u%02u%02u-%02u%02u%02u-",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
        result.session=p.sessions/(std::wstring(date)+serial());
        if (!fs::create_directory(result.session)) throw std::runtime_error("Could not create a unique session directory.");
        write_report(p,result);
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        Handle output(CreateFileW((result.session/L"game.stdout.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        Handle errors(CreateFileW((result.session/L"game.stderr.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
        if (!output || !errors || !input) throw std::runtime_error(utf8(windows_error(L"Cannot create game log handles")));
        std::vector<wchar_t> env;
        for (const auto& [key,value]:p.environment) {
            const auto entry=key+L"="+value;
            env.insert(env.end(),entry.begin(),entry.end()); env.push_back(0);
        }
        env.push_back(0); if (env.size()==1) env.push_back(0);
        auto command=quote(p.executable.wstring())+L" "+quote(p.elf.wstring());
        if (command.size()>=32767) throw std::runtime_error("The native game command line exceeds the Windows limit.");
        const std::vector<HANDLE> handles={output.value,errors.value,input.value}; Attributes attributes(handles);
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.lpAttributeList=attributes.list;
        startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES; startup.StartupInfo.hStdOutput=output.value;
        startup.StartupInfo.hStdError=errors.value; startup.StartupInfo.hStdInput=input.value;
        PROCESS_INFORMATION process{};
        std::thread identity; struct Join { std::thread& thread; ~Join() { if (thread.joinable()) thread.join(); } } join{identity};
        auto record=[&] { try { record_identity(p,result.session); } catch (...) {} };
        if (!CreateProcessW(p.executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,env.data(),result.session.c_str(),&startup.StartupInfo,&process)) {
            result.exit_code=GetLastError(); result.error=windows_error(L"Haze could not start",result.exit_code); record();
        } else {
            result.started=true; Handle game(process.hProcess),thread(process.hThread); thread.close();
            // Hash while the running image keeps the executable from being replaced.
            identity=std::thread(record);
            // The launcher remains Steam's tracked process for the game's full
            // lifetime. No timeout or elapsed-time heuristic means success.
            if (WaitForSingleObject(game.value,INFINITE)!=WAIT_OBJECT_0 || !GetExitCodeProcess(game.value,&result.exit_code))
                throw std::runtime_error(utf8(windows_error(L"Cannot observe the native game's exit")));
            if (result.exit_code) {
                wchar_t code[24]{}; swprintf_s(code,L"0x%08lX",result.exit_code);
                result.error=L"Haze exited with code "+std::wstring(code)+L". Open the session logs for details.";
                if (result.exit_code==0xc0000135u) result.error+=L" A required runtime DLL is missing. Reinstall the game and Microsoft Visual C++ runtime.";
                if (result.exit_code==0xc000007bu) result.error+=L" A runtime DLL has the wrong architecture or is damaged. Reinstall the 64-bit game/runtime files.";
            }
        }
        FlushFileBuffers(output.value); FlushFileBuffers(errors.value);
        result.finished=true; write_report(p,result);
    } catch (const std::exception& e) {
        result.error=error_text(e.what()); if (!result.exit_code) result.exit_code=1; result.finished=true;
        if (!result.session.empty()) { try { write_report(p,result); } catch (...) {} }
    }
    return result;
}
}
