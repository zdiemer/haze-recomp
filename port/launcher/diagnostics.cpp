#include "launch.h"
#include <bcrypt.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#ifndef HAZE_LAUNCH_VERSION
#define HAZE_LAUNCH_VERSION "unknown"
#endif
#ifndef HAZE_LAUNCH_COMMIT
#define HAZE_LAUNCH_COMMIT "unknown"
#endif
#ifndef HAZE_LAUNCH_SDK
#define HAZE_LAUNCH_SDK "unknown"
#endif

namespace haze_launch {
std::vector<std::pair<std::wstring,std::wstring>> settings(const Plan& p) {
    return {{L"InstallRoot",p.root.wstring()},{L"SharedRoot",p.shared.wstring()},{L"Executable",p.executable.wstring()},
        {L"DiscDirectory",p.disc.wstring()},{L"Firmware",p.firmware.wstring()},{L"Savedata",p.saves.wstring()},
        {L"ShaderCache",p.cache.wstring()},{L"Sessions",p.sessions.wstring()},{L"InputConfig",p.input.wstring()},
        {L"GraphicsConfig",p.graphics.wstring()},{L"InputMode",p.mode},{L"RenderResolution",p.resolution}};
}
static std::wstring upper(std::wstring text) { for (auto& c:text) if (c>=L'a' && c<=L'z') c-=32; return text; }
bool diagnostic_variable(const std::wstring& name) {
    const auto key=upper(name);
    for (const wchar_t* prefix:{L"HAZE_",L"RSX_",L"PS3_",L"SDL_",L"YDKJ_"}) if (key.starts_with(prefix)) return true;
    return false;
}
// Values never recorded or exported. The SDK reads the EDAT license key
// (PS3_RIF_KEY), console PSID (PS3_OPEN_PSID), NP online ID and PSN relay host
// from the environment; any other name with a credential-like word matches too.
bool sensitive_variable(const std::wstring& name) {
    const auto key=upper(name);
    for (const wchar_t* exact:{L"PS3_RIF_KEY",L"PS3_OPEN_PSID",L"PS3_NP_ONLINE_ID",L"PSNR_SERVER"}) if (key==exact) return true;
    for (const wchar_t* part:{L"TOKEN",L"SECRET",L"PASSWORD",L"PASSWD",L"CREDENTIAL",L"RIFKEY",L"APIKEY",L"KLICENSEE"}) if (key.find(part)!=key.npos) return true;
    for (size_t start=0;start<=key.size();) {
        auto end=key.find(L'_',start); if (end==key.npos) end=key.size();
        const auto word=key.substr(start,end-start);
        for (const wchar_t* token:{L"KEY",L"KEYS",L"RIF",L"RAP",L"IDPS",L"PSID",L"KLIC",L"ACT",L"ACTDAT",L"LICENSE",L"AUTH",L"TICKET",L"NPID",L"ONLINEID",L"PWD",L"CERT"})
            if (word==token) return true;
        if (word==L"ONLINE" && key.compare(end,3,L"_ID")==0) return true;
        start=end+1;
    }
    return false;
}
static std::string text(const std::wstring& value) { try { return utf8(value); } catch (...) { return "<invalid Unicode>"; } }
static std::string hex32(DWORD value) { char b[16]; std::snprintf(b,sizeof(b),"0x%08lX",value); return b; }
static std::string utc(const FILETIME& time) {
    SYSTEMTIME t{}; if (!FileTimeToSystemTime(&time,&t)) return "unknown";
    char b[32]; std::snprintf(b,sizeof(b),"%04u-%02u-%02uT%02u:%02u:%02uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond); return b;
}
static std::string modified(const fs::path& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&data) ? utc(data.ftLastWriteTime) : "unknown";
}

// Redaction: path prefixes and whole words, ASCII case-insensitive, either
// path separator. Longest rule wins so specific roots beat the user profile.
static unsigned char lower(unsigned char c) { return c>='A' && c<='Z' ? c+32 : c; }
static bool word(unsigned char c) { return (c>='0' && c<='9') || (c>='a' && c<='z') || (c>='A' && c<='Z') || c>=0x80; }
static bool slash(unsigned char c) { return c=='\\' || c=='/'; }
static bool matches(const std::string& s,size_t at,const std::string& needle) {
    if (at+needle.size()>s.size() || (at && word(s[at-1]) && word(needle[0]))) return false;
    for (size_t i=0;i<needle.size();++i) {
        const unsigned char a=s[at+i],b=needle[i];
        if (lower(a)!=lower(b) && !(slash(a) && slash(b))) return false;
    }
    const auto end=at+needle.size(); return end==s.size() || !word(s[end]) || !word(needle.back());
}
Redactor redactor(const Plan& p,const Values& env) {
    Redactor r;
    auto get=[&](const wchar_t* key) { auto item=env.find(key); return item==env.end() ? std::wstring{} : item->second; };
    auto path=[&](const fs::path& value,const char* placeholder) {
        if (value.empty()) return;
        const auto normal=value.lexically_normal(); auto s=normal.wstring();
        while (s.size()>3 && (s.back()==L'\\' || s.back()==L'/')) s.pop_back();
        if (normal==normal.root_path() || s.size()<4) return;
        try { r.rules.emplace_back(utf8(s),placeholder); } catch (...) {}
    };
    auto name=[&](const std::wstring& value,const char* placeholder) {
        if (value.size()<3 || value.find_first_of(L"\\/")!=value.npos) return;
        try { r.rules.emplace_back(utf8(value),placeholder); } catch (...) {}
    };
    path(p.root,"<InstallRoot>"); if (p.shared!=p.root) path(p.shared,"<SharedRoot>");
    path(get(L"USERPROFILE"),"<UserProfile>");
    if (!get(L"HOMEDRIVE").empty() && !get(L"HOMEPATH").empty()) path(get(L"HOMEDRIVE")+get(L"HOMEPATH"),"<UserProfile>");
    for (const wchar_t* key:{L"OneDrive",L"OneDriveCommercial",L"OneDriveConsumer"}) path(get(key),"<OneDrive>");
    name(get(L"USERNAME"),"<user>"); name(get(L"COMPUTERNAME"),"<computer>"); name(get(L"USERDOMAIN"),"<domain>");
    // Current values of sensitive variables, wherever the game printed them.
    for (const auto* source:{&env,&p.environment})
        for (const auto& [key,value]:*source) if (sensitive_variable(key) && value.size()>=4) try { r.rules.emplace_back(utf8(value),"<redacted>"); } catch (...) {}
    std::stable_sort(r.rules.begin(),r.rules.end(),[](const auto& a,const auto& b) { return a.first.size()>b.first.size(); });
    return r;
}
// NAME=value assignments of sensitive variables, independent of the current
// environment, so a historical launcher.log or a game message is still scrubbed.
static bool ident(unsigned char c) { return (c>='0' && c<='9') || (c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_'; }
// Prose such as "Expected Key=Value" is not a variable assignment.
static bool sensitive_assignment(const std::string& name) {
    const bool prose=name.find('_')==name.npos && std::any_of(name.begin(),name.end(),[](char c) { return c>='a' && c<='z'; });
    return !prose && sensitive_variable(std::wstring(name.begin(),name.end()));
}
static std::string scrub(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i=0;i<s.size();) {
        if (!ident(s[i]) || (i && ident(s[i-1]))) { out+=s[i++]; continue; }
        size_t j=i; while (j<s.size() && ident(s[j])) ++j;
        size_t k=j; while (k<s.size() && (s[k]==' ' || s[k]=='\t')) ++k;
        if (k>=s.size() || s[k]!='=' || j-i>96 || !sensitive_assignment(s.substr(i,j-i))) { out.append(s,i,j-i); i=j; continue; }
        ++k; while (k<s.size() && (s[k]==' ' || s[k]=='\t')) ++k;
        out.append(s,i,k-i); out+="<redacted>";
        if (k<s.size() && (s[k]=='"' || s[k]=='\'')) { const auto close=s.find_first_of(std::string(1,s[k])+"\r\n",k+1); k=close==s.npos ? s.size() : s[close]==s[k] ? close+1 : close; }
        else while (k<s.size() && !std::strchr(" \t\r\n\";&",s[k])) ++k;
        i=k;
    }
    return out;
}
std::string redact(const std::string& input,const Redactor& r) {
    const auto s=scrub(input);
    bool first[256]{}; for (const auto& rule:r.rules) { first[lower(rule.first[0])]=true; first[(unsigned char)rule.first[0]]=true; if (slash(rule.first[0])) first['\\']=first['/']=true; }
    std::string out; out.reserve(s.size());
    for (size_t i=0;i<s.size();) {
        bool hit=false;
        if (first[lower(s[i])] || first[(unsigned char)s[i]])
            for (const auto& [needle,replacement]:r.rules) if (matches(s,i,needle)) { out+=replacement; i+=needle.size(); hit=true; break; }
        if (!hit) out+=s[i++];
    }
    return out;
}

std::string bounded(const fs::path& file,uintmax_t limit,uintmax_t head,std::string& note) {
    std::error_code ec; const auto status=fs::status(file,ec);
    if (!fs::exists(status)) { note="missing"; return {}; }
    if (!fs::is_regular_file(status)) { note="unreadable: not a regular file"; return {}; }
    const auto size=fs::file_size(file,ec); if (ec) { note="unreadable: "+ec.message(); return {}; }
    std::ifstream in(file,std::ios::binary); if (!in) { note="unreadable: cannot open the file"; return {}; }
    auto read=[&](uintmax_t offset,uintmax_t count) {
        std::string s(size_t(count),'\0'); in.clear(); in.seekg(std::streamoff(offset)); in.read(s.data(),std::streamsize(count));
        s.resize(size_t(std::max<std::streamsize>(in.gcount(),0))); return s;
    };
    if (size<=limit) { auto s=read(0,size); note="included "+std::to_string(s.size())+" bytes"; return s; }
    head=std::min(head,limit);
    auto first=read(0,head),last=read(size-(limit-head),limit-head);
    // Begin the retained tail at a line boundary when one is available.
    if (const auto line=last.find('\n'); line!=last.npos && line+1<last.size() && line<4096) last.erase(0,line+1);
    note="truncated: kept the first "+std::to_string(first.size())+" and last "+std::to_string(last.size())+" of "+std::to_string(size)+" bytes";
    return first+"\n\n[haze-diagnostics: "+std::to_string(size-first.size()-last.size())+" bytes omitted]\n\n"+last;
}

Session session(const fs::path& folder) {
    Session s; s.path=folder; s.name=folder.filename().wstring();
    std::string note; const auto log=bounded(folder/L"launcher.log",256*1024,256*1024,note);
    if (note.starts_with("missing") || note.starts_with("unreadable")) return s;
    s.record=true; size_t offset=0;
    while (offset<log.size()) {
        auto end=log.find('\n',offset); if (end==log.npos) end=log.size();
        auto line=log.substr(offset,end-offset); offset=end+1; if (line.ends_with('\r')) line.pop_back();
        const auto equals=line.find('='); if (equals==line.npos) continue;
        const auto key=line.substr(0,equals),value=line.substr(equals+1);
        if (key=="Started") s.started=value=="1";
        else if (key=="Finished") { s.has_finished=true; s.finished=value=="1"; }
        else if (key=="ExitCode") s.exit_code=DWORD(std::strtoul(value.c_str(),nullptr,10));
        else if (key=="Error") s.error=value;
    }
    return s;
}
std::vector<Session> sessions(const fs::path& root,size_t limit) {
    std::vector<fs::path> folders; std::error_code ec;
    for (fs::directory_iterator item(root,ec),end;!ec && item!=end;item.increment(ec))
        if (item->is_directory(ec)) folders.push_back(item->path());
    // Launcher and play.ps1 session names begin with a UTC timestamp.
    std::sort(folders.begin(),folders.end(),[](const auto& a,const auto& b) { return a.filename().wstring()>b.filename().wstring(); });
    if (folders.size()>limit) folders.resize(limit);
    std::vector<Session> result; for (const auto& folder:folders) result.push_back(session(folder)); return result;
}
fs::path find_session(const Plan& p,const std::wstring& name) {
    if (name.empty()) {
        const auto found=sessions(p.sessions,1);
        if (found.empty()) throw std::runtime_error("No session logs were found in "+text(p.sessions.wstring())+".");
        return found[0].path;
    }
    if (name==L"." || name==L".." || name.find_first_of(L"\\/:")!=name.npos) throw std::runtime_error("--session expects a session folder name.");
    std::error_code ec; const auto folder=p.sessions/name;
    if (!fs::is_directory(folder,ec)) throw std::runtime_error("The session "+text(name)+" was not found in "+text(p.sessions.wstring())+".");
    return folder;
}
static const char* status_name(DWORD code) {
    switch (code) {
    case 3: return "abort or CRT fatal error";
    case 0x80000003u: return "breakpoint";
    case 0xC0000005u: return "access violation";
    case 0xC000001Du: return "illegal instruction";
    case 0xC0000094u: return "integer divide by zero";
    case 0xC0000096u: return "privileged instruction";
    case 0xC00000FDu: return "stack overflow";
    case 0xC0000135u: return "missing DLL";
    case 0xC0000142u: return "DLL initialization failed";
    case 0xC000007Bu: return "invalid or wrong-architecture image";
    case 0xC000013Au: return "terminated by Ctrl+C";
    case 0xC0000374u: return "heap corruption";
    case 0xC0000409u: return "stack buffer overrun or fail-fast";
    default: return nullptr;
    }
}
std::string describe(const Session& s) {
    if (!s.record) return "no launcher record";
    if (s.has_finished && !s.finished) return "incomplete: the launcher did not record an exit";
    if (!s.started) return "did not start";
    if (!s.exit_code) return "exited normally";
    const auto name=status_name(s.exit_code);
    return "exited with "+hex32(s.exit_code)+(name ? std::string(" (")+name+")" : std::string{});
}

static std::string sha256(const fs::path& file) {
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; std::string result="unavailable";
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0))) return result;
    if (BCRYPT_SUCCESS(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0))) {
        std::ifstream in(file,std::ios::binary); std::vector<char> buffer(1<<20); bool ok=bool(in);
        while (ok && in) {
            in.read(buffer.data(),std::streamsize(buffer.size())); const auto n=in.gcount();
            if (n>0 && !BCRYPT_SUCCESS(BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),ULONG(n),0))) ok=false;
        }
        unsigned char digest[32];
        if (ok && !in.bad() && BCRYPT_SUCCESS(BCryptFinishHash(hash,digest,32,0))) {
            result.clear(); char b[3]; for (auto byte:digest) { std::snprintf(b,sizeof(b),"%02x",byte); result+=b; }
        } else if (!ok || in.bad()) result="unavailable (read failed)";
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm,0); return result;
}
// PE timestamp/size and CodeView record: the keys a symbol server or a local
// build tree use to match HazeRecomp.exe with its PDB.
static std::string image(const fs::path& file,const std::string& prefix) {
    std::ifstream in(file,std::ios::binary); if (!in) return prefix+"Image=unreadable\n";
    auto at=[&](uint64_t offset,void* out,size_t size) { in.clear(); in.seekg(std::streamoff(offset)); in.read(static_cast<char*>(out),std::streamsize(size)); return size_t(in.gcount())==size; };
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!at(0,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || !at(uint64_t(dos.e_lfanew),&nt,sizeof(nt)) ||
        nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return prefix+"Image=not a 64-bit Windows image\n";
    char b[160]; std::snprintf(b,sizeof(b),"%sTimestamp=0x%08lX\n%sSizeOfImage=0x%lX\n",prefix.c_str(),nt.FileHeader.TimeDateStamp,prefix.c_str(),nt.OptionalHeader.SizeOfImage);
    std::string out=b;
    const auto& debug=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const uint64_t sections=uint64_t(dos.e_lfanew)+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
    uint64_t offset=0;
    for (unsigned i=0;i<std::min<unsigned>(nt.FileHeader.NumberOfSections,96);++i) {
        IMAGE_SECTION_HEADER section{}; if (!at(sections+i*sizeof(section),&section,sizeof(section))) break;
        const auto extent=std::max(section.Misc.VirtualSize,section.SizeOfRawData);
        if (debug.VirtualAddress>=section.VirtualAddress && debug.VirtualAddress<section.VirtualAddress+extent) { offset=debug.VirtualAddress-section.VirtualAddress+section.PointerToRawData; break; }
    }
    bool symbols=false;
    for (unsigned i=0;offset && i<std::min<DWORD>(debug.Size/sizeof(IMAGE_DEBUG_DIRECTORY),16);++i) {
        IMAGE_DEBUG_DIRECTORY entry{}; if (!at(offset+i*sizeof(entry),&entry,sizeof(entry))) break;
        if (entry.Type!=IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData<25 || entry.SizeOfData>4096) continue;
        std::string record(entry.SizeOfData,'\0'); if (!at(entry.PointerToRawData,record.data(),record.size()) || record.compare(0,4,"RSDS")) continue;
        GUID guid; DWORD age; std::memcpy(&guid,record.data()+4,sizeof(guid)); std::memcpy(&age,record.data()+20,sizeof(age));
        // Retain only the PDB file name; its build path can identify the builder.
        std::string pdb(record.c_str()+24); const auto slash_at=pdb.find_last_of("\\/"); if (slash_at!=pdb.npos) pdb.erase(0,slash_at+1);
        std::snprintf(b,sizeof(b),"%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX",guid.Data1,guid.Data2,guid.Data3,
            guid.Data4[0],guid.Data4[1],guid.Data4[2],guid.Data4[3],guid.Data4[4],guid.Data4[5],guid.Data4[6],guid.Data4[7],age);
        out+=prefix+"PDB="+pdb+"\n"+prefix+"SymbolKey="+b+"\n"; symbols=true; break;
    }
    if (!symbols) out+=prefix+"SymbolKey=unavailable (no CodeView record)\n";
    return out;
}
static std::string clean(std::string value) { for (auto& c:value) if ((unsigned char)c<0x20) c='?'; return value; }
static std::string sfo(const fs::path& file) {
    std::string note; const auto s=bounded(file,64*1024,64*1024,note);
    if (note.starts_with("missing") || note.starts_with("unreadable")) return "PARAM.SFO="+note+"\n";
    if (note.starts_with("truncated") || s.size()<20 || s.compare(0,4,std::string("\0PSF",4))) return "PARAM.SFO=invalid\n";
    auto u16=[&](size_t o) { return o+2<=s.size() ? uint32_t((unsigned char)s[o]|(unsigned char)s[o+1]<<8) : 0u; };
    auto u32=[&](size_t o) { return o+4<=s.size() ? u16(o)|u16(o+2)<<16 : 0u; };
    const size_t keys=u32(8),data=u32(12),count=std::min<uint32_t>(u32(16),256);
    std::string out;
    for (size_t i=0;i<count;++i) {
        const size_t e=20+16*i; if (e+16>s.size()) break;
        const size_t key=keys+u16(e),format=u16(e+2),length=u32(e+4),value=data+u32(e+12);
        if (key>=s.size() || value+length>s.size() || value+length<value) continue;
        const std::string name(s.c_str()+key,strnlen(s.c_str()+key,s.size()-key));
        if (name!="TITLE_ID" && name!="TITLE" && name!="VERSION" && name!="APP_VER" && name!="PS3_SYSTEM_VER" && name!="CATEGORY") continue;
        if (format==0x0404 && length>=4) out+=name+"="+std::to_string(u32(value))+"\n";
        else if (format==0x0204 || format==0x0004) out+=name+"="+clean(std::string(s.data()+value,strnlen(s.data()+value,length)))+"\n";
    }
    return out.empty() ? "PARAM.SFO=no identifying fields\n" : out;
}

std::string system_report() {
    std::string out; char b[512];
    auto reg=[](const wchar_t* key,const wchar_t* name) {
        DWORD size=0; if (RegGetValueW(HKEY_LOCAL_MACHINE,key,name,RRF_RT_REG_SZ,nullptr,nullptr,&size)!=ERROR_SUCCESS || size<2) return std::string{};
        std::wstring value(size/2,L'\0'); if (RegGetValueW(HKEY_LOCAL_MACHINE,key,name,RRF_RT_REG_SZ,nullptr,value.data(),&size)!=ERROR_SUCCESS) return std::string{};
        value.resize(wcslen(value.c_str())); return text(value);
    };
    auto line=[&](const char* key,const std::string& value) { out+=key; out+="="; out+=value.empty() ? "unavailable" : value; out+="\n"; };
    using Version=LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW os{}; os.dwOSVersionInfoSize=sizeof(os);
    const auto rtl=reinterpret_cast<Version>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion")));
    if (rtl && !rtl(&os)) { std::snprintf(b,sizeof(b),"%lu.%lu.%lu",os.dwMajorVersion,os.dwMinorVersion,os.dwBuildNumber); line("OS.Version",b); }
    else line("OS.Version","");
    const wchar_t* nt=L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    line("OS.ProductName",reg(nt,L"ProductName")); line("OS.DisplayVersion",reg(nt,L"DisplayVersion"));
    DWORD ubr=0,size=sizeof(ubr);
    line("OS.UpdateBuildRevision",RegGetValueW(HKEY_LOCAL_MACHINE,nt,L"UBR",RRF_RT_REG_DWORD,nullptr,&ubr,&size)==ERROR_SUCCESS ? std::to_string(ubr) : "");
    if (rtl && os.dwMajorVersion==10) line("OS.Family",os.dwBuildNumber>=22000 ? "Windows 11" : "Windows 10");
    SYSTEM_INFO info{}; GetNativeSystemInfo(&info);
    line("OS.Architecture",info.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64 ? "x64" : info.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_ARM64 ? "ARM64" : std::to_string(info.wProcessorArchitecture));
    const wchar_t* cpu=L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    line("CPU.Name",reg(cpu,L"ProcessorNameString")); line("CPU.Vendor",reg(cpu,L"VendorIdentifier")); line("CPU.Identifier",reg(cpu,L"Identifier"));
    line("CPU.LogicalProcessors",std::to_string(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    MEMORYSTATUSEX memory{}; memory.dwLength=sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) { line("Memory.TotalMiB",std::to_string(memory.ullTotalPhys>>20)); line("Memory.AvailableMiB",std::to_string(memory.ullAvailPhys>>20)); }
    else line("Memory.TotalMiB","");
    DEVMODEW mode{}; mode.dmSize=sizeof(mode);
    if (EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode)) { std::snprintf(b,sizeof(b),"%lux%lu @ %lu Hz",mode.dmPelsWidth,mode.dmPelsHeight,mode.dmDisplayFrequency); line("Display.Primary",b); }
    else line("Display.Primary","");
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { line("GPU","unavailable (DXGI factory could not be created)"); return out; }
    unsigned found=0;
    for (UINT i=0;i<16;++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter; const auto hr=factory->EnumAdapters1(i,&adapter);
        if (hr==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; if (FAILED(hr) || FAILED(adapter->GetDesc1(&d))) { std::snprintf(b,sizeof(b),"GPU%u=unavailable\n",i); out+=b; continue; }
        d.Description[127]=0; ++found;
        std::snprintf(b,sizeof(b),"GPU%u=%s\nGPU%u.PCI=VEN_%04X&DEV_%04X&SUBSYS_%08X&REV_%02X\nGPU%u.DedicatedVideoMiB=%llu\nGPU%u.Software=%d\n",
            i,text(d.Description).c_str(),i,d.VendorId,d.DeviceId,d.SubSysId,d.Revision,i,(unsigned long long)(d.DedicatedVideoMemory>>20),i,(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0);
        out+=b;
        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&umd)))
            std::snprintf(b,sizeof(b),"GPU%u.Driver=%u.%u.%u.%u\n",i,unsigned(HIWORD(umd.HighPart)),unsigned(LOWORD(umd.HighPart)),unsigned(HIWORD(umd.LowPart)),unsigned(LOWORD(umd.LowPart)));
        else std::snprintf(b,sizeof(b),"GPU%u.Driver=unavailable\n",i);
        out+=b;
    }
    if (!found) line("GPU","unavailable (no DXGI adapters)");
    return out;
}

std::string build_identity(const Plan& p,const std::string& prefix) {
    std::error_code ec; std::string out=prefix+"Game.Executable="+text(p.executable.wstring())+"\n";
    if (fs::is_regular_file(p.executable,ec))
        out+=prefix+"Game.Size="+std::to_string(fs::file_size(p.executable,ec))+"\n"+prefix+"Game.SHA256="+sha256(p.executable)+"\n"+image(p.executable,prefix+"Game.");
    else out+=prefix+"Game.SHA256=unavailable (executable missing)\n";
    if (fs::is_regular_file(p.elf,ec)) out+=prefix+"ELF.Size="+std::to_string(fs::file_size(p.elf,ec))+"\n"+prefix+"ELF.SHA256="+sha256(p.elf)+"\n";
    else out+=prefix+"ELF=missing\n";
    const auto param=sfo(p.disc/L"PS3_GAME/PARAM.SFO");
    for (size_t at=0;at<param.size();) { const auto end=param.find('\n',at); out+=prefix+param.substr(at,end==param.npos ? param.npos : end-at+1); at=end==param.npos ? param.size() : end+1; }
    return out;
}
void record_identity(const Plan& p,const fs::path& session) {
    const auto text="# Build identity recorded when this session launched.\n"+build_identity(p,"")+"Complete=1\n";
    std::ofstream file(session/L"build-identity.txt",std::ios::binary|std::ios::trunc); file<<text;
}
std::vector<Entry> collect(const Plan& p,const Session& s,const Values& env,const std::string& system,const Limits& limits) {
    const auto r=redactor(p,env);
    std::vector<Entry> files; std::vector<std::string> manifest; uintmax_t used=0;
    const uintmax_t reserve=32*1024; // summary and manifest
    auto add=[&](const std::string& name,std::string data) { used+=data.size()+2*name.size()+128; files.push_back({name,std::move(data)}); };
    auto file=[&](const std::string& name,const fs::path& path,uintmax_t limit) {
        const auto room=limits.total>used+reserve ? limits.total-used-reserve : 0;
        if (room<1024) { manifest.push_back(name+": omitted (archive size limit of "+std::to_string(limits.total)+" bytes reached)"); return false; }
        limit=std::min(limit,room); std::string note; auto data=bounded(path,limit,std::min(limits.head,limit/4),note);
        if (note.starts_with("missing") || note.starts_with("unreadable")) { manifest.push_back(name+": "+note); return false; }
        data=redact(data,r);
        if (data.size()>limit+256) { data.resize(size_t(limit)); note+=", cut after redaction"; }
        manifest.push_back(name+": "+note+", modified "+modified(path)); add(name,std::move(data)); return true;
    };
    add("system.txt","# Collected at export time.\n"+redact(system,r));
    manifest.push_back("system.txt: collected at export time");
    std::string configured="# Effective launcher settings resolved at export time, with the source of each value.\n"
        "# session/launcher.log records the values used by the session's launch.\n[Launcher]\n";
    for (const auto& [key,value]:settings(p)) {
        auto source=p.sources.find(key);
        configured+=text(key)+"="+text(value)+"\n"+text(key)+".Source="+(source==p.sources.end() ? std::string("unknown") : text(source->second))+"\n";
    }
    configured+="\n# Variables passed to the game. 'inherited' came from the environment; 'launcher' is set by HazeLauncher.\n[Environment]\n";
    for (const auto& [key,value]:p.environment) {
        if (!diagnostic_variable(key)) continue;
        auto inherited=env.find(key);
        const char* origin=inherited==env.end() ? "launcher" : inherited->second==value ? "inherited" : "launcher, overriding inherited";
        configured+=text(key)+"="+(sensitive_variable(key) ? std::string("<redacted>") : text(value))+"  ; "+origin+"\n";
    }
    if (!p.problems.empty()) {
        configured+="\n# The configuration is not launchable; values above use defaults where it was invalid.\n[Problems]\n";
        for (const auto& problem:p.problems) configured+=text(problem)+"\n";
    }
    add("settings.txt",redact(configured,r)); manifest.push_back("settings.txt: resolved at export time");
    file("config/launcher.ini",p.config,limits.config);
    file("config/launcher-defaults.ini",p.launcher.parent_path()/L"launcher-defaults.ini",limits.config);
    file("config/input.ini",p.input,limits.config);
    if (p.graphics.empty()) manifest.push_back("config/graphics.ini: not configured; built-in graphics defaults apply");
    else file("config/graphics.ini",p.graphics,limits.config);
    const bool build_info=file("build/build-info.json",p.executable.parent_path()/L"build-info.json",64*1024);

    // Logs: launcher record first, then stderr (crash context), then stdout.
    std::vector<fs::path> logs; std::error_code ec; size_t skipped=0;
    for (fs::directory_iterator item(s.path,ec),end;!ec && item!=end;item.increment(ec)) {
        if (!item->is_regular_file(ec)) continue;
        auto extension=upper(item->path().extension().wstring());
        if (extension==L".LOG" || extension==L".TXT" || extension==L".CSV" || extension==L".JSON") logs.push_back(item->path()); else ++skipped;
    }
    if (ec) manifest.push_back("session: listing failed: "+ec.message());
    auto rank=[](const fs::path& path) { const auto name=upper(path.filename().wstring()); return name==L"LAUNCHER.LOG" ? 0 : name==L"GAME.STDERR.LOG" ? 1 : name==L"GAME.STDOUT.LOG" ? 2 : 3; };
    std::sort(logs.begin(),logs.end(),[&](const auto& a,const auto& b) { return rank(a)!=rank(b) ? rank(a)<rank(b) : a.filename()<b.filename(); });
    for (const wchar_t* expected:{L"launcher.log",L"game.stderr.log",L"game.stdout.log"})
        if (std::none_of(logs.begin(),logs.end(),[&](const auto& path) { return !_wcsicmp(path.filename().c_str(),expected); }))
            manifest.push_back("session/"+text(expected)+": missing");
    if (logs.size()>limits.files) { manifest.push_back("session: "+std::to_string(logs.size()-limits.files)+" further text files omitted (file count limit)"); logs.resize(limits.files); }
    std::string crash,boot;
    for (const auto& log:logs) {
        const auto name="session/"+text(log.filename().wstring());
        if (!file(name,log,limits.file) || rank(log)!=1) continue;
        // haze_crash_filter writes its context, then the guest LR report.
        const auto& data=files.back().data; size_t start=data.find("[haze-crash"),lines=0;
        for (size_t at=start;at!=data.npos && at<data.size() && lines<64;++lines) {
            auto end=data.find('\n',at); if (end==data.npos) end=data.size();
            crash+=data.substr(at,end-at)+"\n"; at=end+1;
        }
        if (const auto at=data.rfind("[haze-boot]"); at!=data.npos) boot=data.substr(at,data.find('\n',at)-at)+"\n";
    }
    if (skipped) manifest.push_back("session: "+std::to_string(skipped)+" non-text files excluded");

    SYSTEMTIME now{}; GetSystemTime(&now); FILETIME stamp{}; SystemTimeToFileTime(&now,&stamp);
    std::string summary="Haze Recomp diagnostics report\nCreated="+utc(stamp)+"\nSession="+text(s.name)+"\nSessionFolder="+text(s.path.wstring())+
        "\nStatus="+describe(s)+"\n";
    if (s.record) summary+="Started="+std::to_string(s.started)+"\nFinished="+(s.has_finished ? std::to_string(s.finished) : std::string("not recorded"))+
        "\nExitCode="+hex32(s.exit_code)+"\nLauncherError="+s.error+"\n";
    summary+="\n[Crash evidence from game.stderr.log]\n"+(crash.empty() ? std::string("none recorded\n") : crash);
    summary+="\n[Last boot progress]\n"+(boot.empty() ? std::string("none recorded\n") : boot);
    summary+="\n[Build]\nLauncher.Version=" HAZE_LAUNCH_VERSION "\nLauncher.SourceCommit=" HAZE_LAUNCH_COMMIT "\nLauncher.SDK=" HAZE_LAUNCH_SDK
        "\n; Launcher identifiers are recorded when the launcher build is configured.\n";
    summary+="Launcher.SHA256="+sha256(p.launcher)+"\n";
    // The session's own build, recorded at launch, is authoritative; the file on
    // disk may have been rebuilt or updated since.
    std::string note; auto recorded=bounded(s.path/L"build-identity.txt",64*1024,64*1024,note);
    if (!recorded.ends_with("Complete=1\n")) recorded.clear();
    auto field=[](const std::string& text,const std::string& key) {
        const auto at=text.find("\n"+key+"="); if (at==text.npos) return std::string{};
        const auto start=at+key.size()+2; return text.substr(start,text.find('\n',start)-start);
    };
    if (recorded.empty()) summary+="\n[Session build, recorded at launch]\nunknown (this session did not record its build identity)\n";
    else summary+="\n[Session build, recorded at launch]\n"+recorded.substr(recorded.find('\n')+1);
    const auto current=build_identity(p,"CurrentOnDisk.");
    summary+="\n[Current on disk at export; may differ from the session's build]\n"+current;
    const auto session_hash=field("\n"+recorded,"Game.SHA256"),current_hash=field("\n"+current,"CurrentOnDisk.Game.SHA256");
    summary+="CurrentOnDisk.MatchesSession="+std::string(session_hash.empty() ? "unknown" : session_hash==current_hash ? "yes" : "no")+"\n";
    const auto game_hash=session_hash.empty() ? current_hash : session_hash;
    if (build_info) {
        const auto& json=std::find_if(files.begin(),files.end(),[](const Entry& e) { return e.name=="build/build-info.json"; })->data;
        const auto key=json.find("\"HazeRecomp.exe\""),colon=key==json.npos ? json.npos : json.find(':',key+16);
        const auto quote=colon==json.npos ? json.npos : json.find('"',colon+1);
        const auto listed=quote==json.npos ? std::string{} : json.substr(quote+1,64);
        summary+="BuildInfo.Game="+std::string(listed.empty() ? "no HazeRecomp.exe hash" : listed==game_hash ? "matches " : "does not match ")+
            (listed.empty() ? "" : session_hash.empty() ? "the current executable" : "the session's executable")+" (build-info.json is read at export time)\n";
    } else summary+="BuildInfo.Game=absent (development build)\n";
    const auto title=recorded.empty() ? field("\n"+current,"CurrentOnDisk.TITLE_ID") : field("\n"+recorded,"TITLE_ID");
    summary+="SupportedTitle=BLUS30094 ("+std::string(title=="BLUS30094" ? "matches" : "does not match or unknown")+")\n";
    if (!p.problems.empty()) {
        summary+="\n[Configuration problems at export]\n";
        for (const auto& problem:p.problems) summary+=text(problem)+"\n";
    }
    const auto root=p.root.wstring();
    summary+="\n[Paths]\nInstallRoot.Length="+std::to_string(root.size())+"\nInstallRoot.Spaces="+std::to_string(root.find(L' ')!=root.npos)+
        "\nInstallRoot.NonASCII="+std::to_string(std::any_of(root.begin(),root.end(),[](wchar_t c) { return c>0x7f; }))+"\n";
    summary+="\n[Privacy]\nRedacted: install/shared roots, user profile and OneDrive folders, user, computer and domain names.\n"
        "Excluded: game and firmware files, saves, shader cache, keys, minidumps, and environment variables other than HAZE_/RSX_/PS3_/SDL_/YDKJ_.\n"
        "Always redacted: license keys, console and online IDs and other credential-like variables such as PS3_RIF_KEY and PS3_OPEN_PSID.\n"
        "Nothing is uploaded. Review the files before sharing them.\n";
    std::string list="# Each item and whether it was collected.\n"; for (const auto& line:manifest) list+=line+"\n";
    std::vector<Entry> result={{"summary.txt",redact(summary,r)},{"manifest.txt",redact(list,r)}};
    for (auto& entry:files) result.push_back(std::move(entry));
    return result;
}

static uint32_t crc32(const std::string& data) {
    static const auto table=[] { std::vector<uint32_t> t(256); for (uint32_t i=0;i<256;++i) { uint32_t c=i; for (int k=0;k<8;++k) c=c&1 ? 0xEDB88320u^(c>>1) : c>>1; t[i]=c; } return t; }();
    uint32_t c=0xffffffffu; for (unsigned char byte:data) c=table[(c^byte)&0xff]^(c>>8); return c^0xffffffffu;
}
// Stored (uncompressed) PKZIP with UTF-8 names: readable by Explorer and tar.
std::string zip(const std::vector<Entry>& entries) {
    if (entries.size()>=0xffff) throw std::runtime_error("Too many diagnostics files.");
    SYSTEMTIME local{}; GetLocalTime(&local); FILETIME time{}; WORD dos_date=0x21,dos_time=0;
    if (SystemTimeToFileTime(&local,&time)) FileTimeToDosDateTime(&time,&dos_date,&dos_time);
    std::string out,directory;
    auto u16=[](std::string& s,uint32_t v) { s+=char(v&0xff); s+=char(v>>8&0xff); };
    auto u32=[&](std::string& s,uint32_t v) { u16(s,v&0xffff); u16(s,v>>16); };
    for (const auto& e:entries) {
        if (e.name.empty() || e.name.size()>=0xffff || e.data.size()>=0xffffffffu || out.size()+e.data.size()>=0xfffffff0u) throw std::runtime_error("The diagnostics report is too large.");
        const auto crc=crc32(e.data); const auto offset=uint32_t(out.size());
        auto header=[&](std::string& s,bool central) {
            u32(s,central ? 0x02014b50u : 0x04034b50u); if (central) u16(s,20);
            u16(s,20); u16(s,0x0800); u16(s,0); u16(s,dos_time); u16(s,dos_date);
            u32(s,crc); u32(s,uint32_t(e.data.size())); u32(s,uint32_t(e.data.size())); u16(s,uint32_t(e.name.size())); u16(s,0);
            if (central) { u16(s,0); u16(s,0); u16(s,0); u32(s,0); u32(s,offset); }
            s+=e.name;
        };
        header(out,false); out+=e.data; header(directory,true);
    }
    const auto start=uint32_t(out.size()); out+=directory;
    u32(out,0x06054b50u); u16(out,0); u16(out,0); u16(out,uint32_t(entries.size())); u16(out,uint32_t(entries.size()));
    u32(out,uint32_t(directory.size())); u32(out,start); u16(out,0);
    return out;
}

fs::path export_diagnostics(const Plan& p,const fs::path& folder,const fs::path& destination,const Values& env,std::wstring& error) {
    try {
        std::error_code ec; if (!fs::is_directory(folder,ec)) throw std::runtime_error("The selected session folder no longer exists.");
        const auto s=session(folder);
        return save_report(zip(collect(p,s,env,system_report())),destination,L"haze-diagnostics-"+s.name,error);
    } catch (const std::exception& e) { error=error_text(e.what()); }
    return {};
}
}
