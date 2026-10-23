#include "launch.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <crtdbg.h>
using namespace haze_launch;
static void put(const fs::path& path,const std::string& text) { fs::create_directories(path.parent_path()); std::ofstream f(path,std::ios::binary); f<<text; assert(f.good()); }
static std::string bytes(const fs::path& path) { std::ifstream f(path,std::ios::binary); assert(f.good()); return {std::istreambuf_iterator<char>(f),{}}; }
static bool has(const std::string& text,const std::string& part) { return text.find(part)!=text.npos; }
static uint32_t u16(const std::string& s,size_t o) { assert(o+2<=s.size()); return uint32_t((unsigned char)s[o]|(unsigned char)s[o+1]<<8); }
static uint32_t u32(const std::string& s,size_t o) { return u16(s,o)|u16(s,o+2)<<16; }
static uint32_t crc(const std::string& data) {
    uint32_t c=0xffffffffu; for (unsigned char b:data) { c^=b; for (int k=0;k<8;++k) c=(c>>1)^(0xEDB88320u&(0u-(c&1))); } return ~c;
}
// Independent reader: central directory, local headers and CRC must agree.
static std::map<std::string,std::string> unzip(const std::string& z) {
    assert(z.size()>=22); const size_t end=z.size()-22; assert(u32(z,end)==0x06054b50u);
    const auto count=u16(z,end+10); size_t at=u32(z,end+16); assert(at+u32(z,end+12)==end);
    std::map<std::string,std::string> files;
    for (unsigned i=0;i<count;++i) {
        assert(u32(z,at)==0x02014b50u && u16(z,at+10)==0 && (u16(z,at+8)&0x0800));
        const auto size=u32(z,at+24),name_size=u16(z,at+28),extra=u16(z,at+30),comment=u16(z,at+32),local=u32(z,at+42);
        const auto name=z.substr(at+46,name_size); assert(u32(z,at+20)==size);
        assert(u32(z,local)==0x04034b50u && u16(z,local+26)==name_size && z.substr(local+30,name_size)==name && u32(z,local+14)==u32(z,at+16));
        const auto data=z.substr(local+30+name_size+u16(z,local+28),size); assert(crc(data)==u32(z,at+16));
        assert(files.emplace(name,data).second); at+=46+name_size+extra+comment;
    }
    assert(at==end); return files;
}
static DWORD run_tool(const fs::path& exe,const std::wstring& args) {
    auto command=quote(exe.wstring())+L" "+args; STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) return 1000;
    assert(WaitForSingleObject(process.hProcess,30000)==WAIT_OBJECT_0); DWORD code=1; assert(GetExitCodeProcess(process.hProcess,&code));
    CloseHandle(process.hProcess); CloseHandle(process.hThread); return code;
}
static std::string sfo(const std::vector<std::pair<std::string,std::string>>& fields) {
    std::string keys,data,index;
    auto le=[](std::string& s,uint32_t v,int n) { for (int i=0;i<n;++i) s+=char(v>>(8*i)&0xff); };
    for (const auto& [key,value]:fields) {
        le(index,uint32_t(keys.size()),2); le(index,0x0204,2); le(index,uint32_t(value.size()+1),4); le(index,uint32_t(value.size()+1),4); le(index,uint32_t(data.size()),4);
        keys+=key+'\0'; data+=value+'\0';
    }
    std::string out("\0PSF",4); le(out,0x101,4); le(out,uint32_t(20+index.size()),4); le(out,uint32_t(20+index.size()+keys.size()),4); le(out,uint32_t(fields.size()),4);
    return out+index+keys+data;
}
int wmain(int argc,wchar_t** argv) {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT|_WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    try {
    assert(argc==2);
    const auto root=fs::path(argv[1])/(L"Diagnostics \u00e9 Jane Doe-"+std::to_wstring(GetCurrentProcessId()));
    // bsdtar reads ANSI arguments; keep its independent check on an ASCII path.
    const auto plain=fs::path(argv[1])/(L"diagnostics-tar-"+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root); fs::create_directories(plain);

    // Zip: binary, empty and nested UTF-8 entries round-trip with valid CRCs.
    std::string binary; for (int i=0;i<256;++i) binary+=char(i);
    const std::vector<Entry> sample={{"a.txt","hello\r\n"},{"dir/b.bin",binary},{"empty.txt",""},{"caf\xc3\xa9.txt","utf-8 name"}};
    auto files=unzip(zip(sample)); assert(files.size()==sample.size());
    for (const auto& e:sample) assert(files.at(e.name)==e.data);
    // Windows' bundled bsdtar is an independent reader where it is present.
    wchar_t system_folder[MAX_PATH]{}; GetSystemDirectoryW(system_folder,MAX_PATH); const auto tar=fs::path(system_folder)/L"tar.exe";
    if (fs::exists(tar)) {
        put(plain/L"sample.zip",zip({sample[0],sample[1],sample[2]})); fs::create_directories(plain/L"extracted");
        assert(run_tool(tar,L"-xf "+quote((plain/L"sample.zip").wstring())+L" -C "+quote((plain/L"extracted").wstring()))==0);
        assert(bytes(plain/L"extracted/dir/b.bin")==binary && bytes(plain/L"extracted/a.txt")=="hello\r\n" && fs::file_size(plain/L"extracted/empty.txt")==0);
    } else std::puts("tar.exe unavailable; independent archive extraction skipped.");

    // Redaction: separators, case, longest match and whole-word names.
    Plan plan; plan.root=L"D:\\Games\\Haze Install"; plan.shared=L"E:\\Shared Jane";
    const Values env={{L"USERPROFILE",L"C:\\Users\\Jane Doe"},{L"USERNAME",L"Jane"},{L"COMPUTERNAME",L"JANE-PC"},{L"USERDOMAIN",L"JANE-PC"},
        {L"OneDrive",L"C:\\Users\\Jane Doe\\OneDrive - Contoso"}};
    const auto r=redactor(plan,env);
    assert(redact("c:/users/jane doe/AppData x",r)=="<UserProfile>/AppData x");
    assert(redact("C:\\Users\\Jane Doe\\OneDrive - Contoso\\haze",r)=="<OneDrive>\\haze");
    assert(redact("path=d:\\games\\haze install\\bin\\HazeRecomp.exe",r)=="path=<InstallRoot>\\bin\\HazeRecomp.exe");
    assert(redact("D:\\Games\\Haze Installer",r)=="D:\\Games\\Haze Installer"); // a different folder
    assert(redact("user jane on JANE-PC; Janet and Jane_x",r)=="user <user> on <computer>; Janet and <user>_x");
    assert(redact("E:/Shared Jane/game",r)=="<SharedRoot>/game");
    assert(redact("no personal data",r)=="no personal data");
    plan.root=L"C:\\"; assert(redactor(plan,{}).rules.size()==1); // drive roots and short names are retained
    // Keys and identities are recognized by name, with or without the environment.
    for (const wchar_t* name:{L"PS3_RIF_KEY",L"ps3_open_psid",L"PS3_NP_ONLINE_ID",L"PSNR_SERVER",L"HAZE_API_TOKEN",L"MY_KEY",L"PS3_ACT_DAT",L"HAZE_IDPS",L"RAP_PATH"})
        assert(sensitive_variable(name));
    for (const wchar_t* name:{L"PAD_NO_KEYBOARD",L"HAZE_INPUT",L"RSX_LIVE_DRAW",L"PS3_NET_ONLINE",L"MONKEY",L"PS3_EDAT"}) assert(!sensitive_variable(name));
    assert(redact("Environment.PS3_RIF_KEY=00112233445566778899aabbccddeeff\nnext",Redactor{})=="Environment.PS3_RIF_KEY=<redacted>\nnext");
    assert(redact("set PS3_OPEN_PSID = \"0011 2233\" done; MONKEY=1 HAZE_INPUT=keyboard",Redactor{})=="set PS3_OPEN_PSID = <redacted> done; MONKEY=1 HAZE_INPUT=keyboard");
    assert(redact("$env:PS3_NP_ONLINE_ID='marge'",Redactor{})=="$env:PS3_NP_ONLINE_ID=<redacted>");
    assert(redact("Expected Key=Value; Token=1",Redactor{})=="Expected Key=Value; Token=1" && redact("ps3_rif_key=0011",Redactor{})=="ps3_rif_key=<redacted>");

    // Bounded reads keep the head and tail and never read past the limit.
    std::string big; for (int i=0;i<2000;++i) big+="line "+std::to_string(i)+"\n";
    put(root/L"big.log",big); std::string note;
    auto kept=bounded(root/L"big.log",4000,1000,note);
    assert(note.starts_with("truncated") && kept.starts_with(big.substr(0,1000)) && kept.ends_with("line 1999\n") && kept.size()<4100 && has(kept,"bytes omitted"));
    assert(bounded(root/L"big.log",big.size(),0,note)==big && note.starts_with("included"));
    assert(bounded(root/L"missing.log",100,10,note).empty() && note=="missing");
    assert(bounded(root,100,10,note).empty() && note.starts_with("unreadable"));

    // Sessions: newest first, status from launcher.log, incomplete and legacy runs.
    const auto sessions_root=root/L"install/local/sessions";
    put(sessions_root/L"20260101-000000-a/launcher.log","Started=1\nFinished=1\nExitCode=0\nError=\n");
    put(sessions_root/L"20260301-000000-c/launcher.log","Started=1\r\nFinished=1\r\nExitCode=3221225477\r\nError=Haze exited\r\n");
    put(sessions_root/L"20260201-000000-b/launcher.log","Started=1\nFinished=0\nExitCode=1\nError=\n");
    fs::create_directories(sessions_root/L"20250101-000000-play"); put(sessions_root/L"stray.txt","not a session");
    auto listed=sessions(sessions_root,3);
    assert(listed.size()==3 && listed[0].name==L"20260301-000000-c" && listed[1].name==L"20260201-000000-b" && listed[2].name==L"20260101-000000-a");
    assert(describe(listed[0])=="exited with 0xC0000005 (access violation)" && listed[0].error=="Haze exited");
    assert(describe(listed[1]).starts_with("incomplete") && describe(listed[2])=="exited normally");
    assert(describe(session(sessions_root/L"20250101-000000-play"))=="no launcher record");
    assert(sessions(root/L"no sessions").empty());
    Plan located; located.sessions=sessions_root;
    assert(find_session(located,L"")==sessions_root/L"20260301-000000-c" && find_session(located,L"20260101-000000-a")==sessions_root/L"20260101-000000-a");
    bool rejected=false; try { find_session(located,L"..\\install"); } catch (const std::exception&) { rejected=true; } assert(rejected);
    rejected=false; try { find_session(located,L"absent"); } catch (const std::exception&) { rejected=true; } assert(rejected);

    // Collection: a crashed session with oversized stderr and a missing stdout.
    const auto install=root/L"install";
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
    fs::create_directories(install/L"bin"); fs::copy_file(module,install/L"bin/HazeRecomp.exe");
    put(install/L"bin/HazeLauncher.exe","launcher bytes"); put(install/L"bin/launcher-defaults.ini","[Launcher]\nInstallRoot=..\n");
    put(install/L"bin/launcher.ini","[Launcher]\nInputMode=Controller\nSavedata=C:\\Users\\Jane Doe\\Saves\n");
    put(install/L"config/input.ini","[Bindings]\nCross=Space\n");
    put(install/L"game/disc/PS3_GAME/USRDIR/EBOOT.ELF","fixture ELF");
    put(install/L"game/disc/PS3_GAME/PARAM.SFO",sfo({{"APP_VER","01.00"},{"CATEGORY","DG"},{"TITLE","HAZE\x01"},{"TITLE_ID","BLUS30094"},{"VERSION","01.00"}}));
    const auto crashed=sessions_root/L"20260301-000000-c";
    std::string stderr_log="[haze-boot] last-hle=0x1 (first)\n";
    while (stderr_log.size()<300000) stderr_log+="noise from C:\\Users\\Jane Doe\\AppData and JANE-PC\n";
    stderr_log+="[haze-boot] last-hle=0x2 (cellGcmInit)\n[haze-crash-context] tid=7 module_base=0000 rip_offset=0x1234\n"
        "[haze-crash] exception=0xC0000005 address=0000 last-hle=0x2 (cellGcmInit)\nguest-lr 0x00123456\n";
    stderr_log+="open psid 0000111122223333aaaabbbbccccdddd in use\n";
    // A historical log from before redaction: the key is no longer in the environment.
    put(crashed/L"launcher.log","Started=1\r\nFinished=1\r\nExitCode=3221225477\r\nError=Haze exited\r\nEnvironment.PS3_RIF_KEY=00112233445566778899aabbccddeeff\r\n");
    put(crashed/L"game.stderr.log",stderr_log); put(crashed/L"trace.csv","a,b\n1,2\n"); put(crashed/L"core.bin",std::string(4096,'\x7f'));
    const Values inherited={{L"USERPROFILE",L"C:\\Users\\Jane Doe"},{L"USERNAME",L"Jane"},{L"COMPUTERNAME",L"JANE-PC"},
        {L"RSX_TEST_OVERRIDE",L"1"},{L"HAZE_INPUT",L"keyboard"},{L"HAZE_API_TOKEN",L"s3cr3t-value"},{L"UNRELATED",L"private"},
        {L"PS3_OPEN_PSID",L"0000111122223333aaaabbbbccccdddd"}};
    plan=resolve(install/L"bin/HazeLauncher.exe",{},inherited);
    // The session records the build it launched; the executable is replaced later.
    record_identity(plan,crashed);
    auto field=[](const std::string& text,const std::string& key) { const auto at=text.find(key+"="); assert(at!=text.npos); return text.substr(at+key.size()+1,text.find('\n',at)-at-key.size()-1); };
    const auto launched=field(build_identity(plan,""),"Game.SHA256"); assert(launched.size()==64);
    Limits limits; limits.file=64*1024; limits.head=8*1024; limits.total=160*1024;
    const auto system=system_report();
    assert(has(system,"OS.Version=") && has(system,"CPU.Name=") && has(system,"Memory.TotalMiB=") && has(system,"GPU"));
    auto entries=collect(plan,session(crashed),inherited,system,limits);
    std::map<std::string,std::string> report; for (const auto& e:entries) report[e.name]=e.data;
    for (const char* name:{"summary.txt","manifest.txt","system.txt","settings.txt","config/launcher.ini","config/input.ini","session/launcher.log","session/game.stderr.log","session/trace.csv"})
        assert(report.contains(name));
    assert(!report.contains("session/core.bin") && !report.contains("session/game.stdout.log") && !report.contains("config/graphics.ini"));
    const auto& summary=report["summary.txt"]; const auto& manifest=report["manifest.txt"]; const auto& settings_text=report["settings.txt"];
    assert(has(summary,"Status=exited with 0xC0000005 (access violation)") && has(summary,"[haze-crash] exception=0xC0000005") && has(summary,"guest-lr 0x00123456"));
    assert(has(summary,"[haze-boot] last-hle=0x2 (cellGcmInit)") && !has(summary,"(first)"));
    assert(has(summary,"TITLE_ID=BLUS30094") && has(summary,"APP_VER=01.00") && has(summary,"TITLE=HAZE?") && has(summary,"SupportedTitle=BLUS30094 (matches)"));
    assert(has(summary,"Game.SHA256=") && has(summary,"Game.Timestamp=0x") && has(summary,"Game.SymbolKey=") && has(summary,"ELF.SHA256="));
    assert(has(summary,"[Session build, recorded at launch]\nGame.Executable=<InstallRoot>") && has(summary,"\nGame.SHA256="+launched+"\n") && has(summary,"CurrentOnDisk.MatchesSession=yes"));
    assert(has(summary,"BuildInfo.Game=absent") && has(summary,"Launcher.Version=") && has(summary,"InstallRoot.NonASCII=1") && has(summary,"InstallRoot.Spaces=1"));
    assert(has(manifest,"session/game.stdout.log: missing") && has(manifest,"session/game.stderr.log: truncated") && has(manifest,"1 non-text files excluded"));
    assert(has(manifest,"config/graphics.ini: not configured") && has(manifest,"build/build-info.json: missing"));
    assert(report["session/game.stderr.log"].size()<limits.file+256 && has(report["session/game.stderr.log"],"[haze-crash]"));
    assert(has(settings_text,"InputMode=keyboard\nInputMode.Source=environment HAZE_INPUT") && has(settings_text,"Savedata.Source=launcher configuration"));
    assert(has(settings_text,"RSX_TEST_OVERRIDE=1  ; inherited") && has(settings_text,"PS3_NET_ONLINE=1  ; launcher") && !has(settings_text,"UNRELATED"));
    assert(has(settings_text,"HAZE_API_TOKEN=<redacted>  ; inherited") && has(settings_text,"PS3_OPEN_PSID=<redacted>  ; inherited"));
    assert(has(report["session/launcher.log"],"Environment.PS3_RIF_KEY=<redacted>") && has(report["session/game.stderr.log"],"open psid <redacted> in use"));
    size_t total=0;
    for (const auto& e:entries) {
        total+=e.data.size();
        assert(!has(e.data,"Jane") && !has(e.data,"JANE") && !has(e.data,"Diagnostics \xc3\xa9"));
        assert(!has(e.data,"00112233445566778899") && !has(e.data,"0000111122223333") && !has(e.data,"s3cr3t"));
    }
    assert(has(report["config/launcher.ini"],"Savedata=<UserProfile>\\Saves") && has(summary,"SessionFolder=<InstallRoot>") && has(summary,"sessions\\20260301-000000-c\n"));
    assert(total<=limits.total);
    // Replacing the executable does not change the session's reported build.
    { std::ofstream exe(install/L"bin/HazeRecomp.exe",std::ios::binary|std::ios::app); exe<<"rebuilt"; assert(exe.good()); }
    const auto rebuilt=field(build_identity(plan,""),"Game.SHA256"); assert(rebuilt!=launched);
    const auto again=collect(plan,session(crashed),inherited,system,limits)[0].data;
    assert(has(again,"\nGame.SHA256="+launched+"\n") && has(again,"CurrentOnDisk.Game.SHA256="+rebuilt+"\n") && has(again,"CurrentOnDisk.MatchesSession=no"));
    const auto unrecorded=collect(plan,session(sessions_root/L"20260201-000000-b"),inherited,system,limits)[0].data;
    assert(has(unrecorded,"unknown (this session did not record its build identity)") && has(unrecorded,"CurrentOnDisk.Game.SHA256="+rebuilt) && has(unrecorded,"CurrentOnDisk.MatchesSession=unknown"));
    // A tiny total budget omits logs, but still records why.
    limits.total=33*1024; entries=collect(plan,session(crashed),inherited,system,limits);
    assert(entries[1].name=="manifest.txt" && has(entries[1].data,"omitted (archive size limit"));
    // An incomplete session with no logs still exports and lists what is absent.
    const auto empty=sessions_root/L"20260401-000000-empty"; fs::create_directories(empty);
    entries=collect(plan,session(empty),inherited,system,{});
    assert(entries[0].name=="summary.txt" && has(entries[0].data,"Status=no launcher record") && has(entries[1].data,"session/launcher.log: missing"));

    // Saving: unique names, no replacement and actionable failures.
    const auto reports=root/L"reports"; std::wstring error;
    const auto first=export_diagnostics(plan,crashed,reports,inherited,error); assert(!first.empty() && error.empty());
    assert(first.filename()==L"haze-diagnostics-20260301-000000-c.zip");
    const auto archive=bytes(first); files=unzip(archive); assert(files.contains("summary.txt") && files.contains("session/game.stderr.log"));
    const auto second=export_diagnostics(plan,crashed,reports,inherited,error);
    assert(second.filename()==L"haze-diagnostics-20260301-000000-c-2.zip" && bytes(first)==archive);
    put(reports/L"haze-diagnostics-20260101-000000-a.zip","earlier report");
    const auto third=save_report("new",reports,L"haze-diagnostics-20260101-000000-a",error);
    assert(third.filename()==L"haze-diagnostics-20260101-000000-a-2.zip" && bytes(reports/L"haze-diagnostics-20260101-000000-a.zip")=="earlier report");
    put(root/L"not a folder","file");
    error.clear(); assert(save_report("x",root/L"not a folder",L"report",error).empty() && !error.empty());
    error.clear(); assert(export_diagnostics(plan,sessions_root/L"absent",reports,inherited,error).empty() && !error.empty());
    for (const auto& item:fs::directory_iterator(reports)) assert(!has(utf8(item.path().filename().wstring()),"partial"));
    if (fs::exists(tar)) {
        fs::copy_file(first,plain/L"report.zip"); fs::create_directories(plain/L"report");
        assert(run_tool(tar,L"-xf "+quote((plain/L"report.zip").wstring())+L" -C "+quote((plain/L"report").wstring()))==0);
        assert(bytes(plain/L"report/summary.txt")==files.at("summary.txt") && bytes(plain/L"report/session/game.stderr.log")==files.at("session/game.stderr.log"));
    }
    fs::remove_all(root); fs::remove_all(plain);
    std::puts("Launcher diagnostics: zip integrity, redaction, bounded logs, session status, missing files, settings sources, size limits and non-overwriting export passed.");
    } catch (const std::exception& e) { std::fprintf(stderr,"Launcher diagnostics test exception: %s\n",e.what()); return 1; }
}
