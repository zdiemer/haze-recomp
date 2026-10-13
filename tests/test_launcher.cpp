#include "launch.h"
#include <shellapi.h>
#include <cassert>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <crtdbg.h>
using namespace haze_launch;
static void put(const fs::path& path,const std::string& text) { fs::create_directories(path.parent_path()); std::ofstream f(path,std::ios::binary); f<<text; assert(f.good()); }
static std::string bytes(const fs::path& path) { std::ifstream f(path,std::ios::binary); assert(f.good()); return {std::istreambuf_iterator<char>(f),{}}; }
template<class F> static void rejects(F f) { bool rejected=false; try { f(); } catch (const std::exception&) { rejected=true; } assert(rejected); }
static DWORD launch(const fs::path& exe,std::wstring args,const fs::path& cwd) {
    auto command=quote(exe.wstring())+L" "+args; STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    assert(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,cwd.c_str(),&startup,&process));
    assert(WaitForSingleObject(process.hProcess,15000)==WAIT_OBJECT_0); DWORD code=1; assert(GetExitCodeProcess(process.hProcess,&code));
    CloseHandle(process.hProcess); CloseHandle(process.hThread); return code;
}
int wmain(int argc,wchar_t** argv) {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_CALL_REPORTFAULT|_WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    try {
    assert(argc==4);
    const fs::path fixture=argv[1],launcher=argv[2],test_base=argv[3];
    const auto root=test_base/(L"Haze install \u00e9 space-"+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root/L"bin"); fs::create_directories(root/L"unrelated cwd");
    const auto target=root/L"bin/HazeRecomp.exe",module=root/L"bin/HazeLauncher.exe";
    fs::copy_file(fixture,target); fs::copy_file(launcher,module);
    fs::copy_file(fixture.parent_path()/L"HazeLaunchFixtureRuntime.dll",root/L"bin/HazeLaunchFixtureRuntime.dll");
    put(root/L"bin/launcher-defaults.ini","[Launcher]\nInstallRoot=..\nSharedRoot=shared assets\n");
    put(root/L"shared assets/game/disc/PS3_GAME/USRDIR/EBOOT.ELF","fixture ELF");
    put(root/L"shared assets/game/disc/PS3_GAME/PARAM.SFO","fixture SFO");
    put(root/L"shared assets/local/tools/rpcs3/dev_flash/sys/external/flashMP3.pic","fixture firmware");
    put(root/L"config/input.ini","[Bindings]\nCross=Space\n"); put(root/L"config/graphics.ini","[Graphics]\n");
    const auto original_cwd=fs::current_path(); fs::current_path(root/L"unrelated cwd");
    auto plan=resolve(module,{},{}); std::wstring repair;
    assert(validate(plan,repair).empty() && plan.root==root && plan.shared==root/L"shared assets");
    assert(plan.saves==root/L"local/state/savedata" && plan.cache==root/L"local/cache/rsx-shaders");
    assert(plan.input==root/L"config/input.ini" && plan.graphics==root/L"config/graphics.ini" && plan.mode==L"auto");
    assert(plan.environment.at(L"HAZE_LAUNCHER_PATH")==module.wstring());
    {
        int count=0; const auto text=L"launcher "+plan.steam_arguments;
        auto args=CommandLineToArgvW(text.c_str(),&count); assert(args);
        std::vector<std::wstring> options; for (int i=1;i<count;++i) options.emplace_back(args[i]); LocalFree(args);
        const auto steam=resolve(module,parse(options),{});
        assert(steam.executable==plan.executable && steam.saves==plan.saves && steam.cache==plan.cache && steam.input==plan.input && steam.firmware==plan.firmware);
    }
    const auto config=root/L"bin/launcher.ini";
    put(config,"; Preserve this comment\n[Launcher]\nInputMode=Controller\nRenderResolution=2560x1440\nSavedata=my saves\n[Other]\nValue=preserve\n");
    Values inherited={{L"HAZE_INPUT",L"keyboard"},{L"RSX_RENDER_RESOLUTION",L"1920x1080"},{L"PS3_SAVEDATA_ROOT",(root/L"environment saves").wstring()}};
    auto options=parse({L"--input-mode",L"Auto",L"--render-resolution",L"1280x720"});
    plan=resolve(module,options,inherited);
    assert(plan.mode==L"auto" && plan.resolution==L"1280x720" && plan.saves==root/L"environment saves");
    auto saved=resolve(module,{},{}); assert(saved.mode==L"controller" && saved.resolution==L"2560x1440" && saved.saves==root/L"my saves");
    rejects([] { parse({L"--unknown"}); }); rejects([] { parse({L"--input-mode"}); }); rejects([] { parse({L"--input-mode",L"auto",L"--input-mode",L"auto"}); });
    rejects([&] { auto o=parse({L"--render-resolution",L"99999x10"}); resolve(module,o,{}); });
    rejects([&] { auto o=parse({L"--render-resolution",L"2x4garbage"}); resolve(module,o,{}); });
    rejects([&] { auto o=parse({L"--input-mode",L"unknown"}); resolve(module,o,{}); });
    rejects([&] { auto o=parse({L"--savedata",L"C:relative"}); resolve(module,o,{}); });
    // Quote round-trip is checked against Windows' own argument parser.
    const std::vector<std::wstring> arguments={L"",L"a b",L"ends\\",L"a\\\"quoted\" value",L"\u00e9 folder\\file"};
    std::wstring command=L"fixture"; for (const auto& arg:arguments) command+=L" "+quote(arg);
    int n=0; auto parsed=CommandLineToArgvW(command.c_str(),&n); assert(parsed && n==int(arguments.size())+1);
    for (int i=1;i<n;++i) assert(arguments[i-1]==parsed[i]); LocalFree(parsed);
    std::wstring error;
    assert(save_setting(config,L"Firmware",(root/L"shared assets/local/tools/rpcs3/dev_flash").wstring(),error));
    assert(bytes(config).find("Preserve this comment")!=std::string::npos && bytes(config).find("Value=preserve")!=std::string::npos);
    auto before=bytes(config);
    HANDLE lock=CreateFileW(config.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr); assert(lock!=INVALID_HANDLE_VALUE);
    assert(!save_setting(config,L"InputMode",L"KeyboardMouse",error) && bytes(config)==before); CloseHandle(lock);
    assert(save_setting(config,L"InputMode",L"KeyboardMouse",error)); assert(read_config(config).at(L"InputMode")==L"KeyboardMouse");
    put(config,"[Launcher]\nInputMode=auto\ninputmode=controller\n"); rejects([&] { read_config(config); });
    put(config,"[Launcher]\nTypo=1\n"); rejects([&] { read_config(config); });
    put(config,"[Launcher]\nInputMode=auto\n");
    plan=resolve(module,{},{}); assert(validate(plan,repair).empty());
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; HANDLE sentinel=CreateEventW(&sa,TRUE,FALSE,nullptr); assert(sentinel);
    plan.environment[L"HAZE_TEST_SENTINEL"]=std::to_wstring(uintptr_t(sentinel)); plan.environment[L"HAZE_TEST_HOLD_MS"]=L"150";
    plan.environment[L"PATH"]=L""; // Neither process needs PowerShell or shell lookup.
    const auto start=GetTickCount64(); const auto first=run(plan);
    assert(first.started && !first.exit_code && first.error.empty() && GetTickCount64()-start>=140);
    assert(WaitForSingleObject(sentinel,0)==WAIT_TIMEOUT); CloseHandle(sentinel);
    auto output=bytes(first.session/L"game.stdout.log");
    assert(output.find("ARGC=2")!=std::string::npos && output.find("NO_CONSOLE=1")!=std::string::npos);
    assert(output.find("ARG1="+utf8(plan.elf.wstring()))!=std::string::npos);
    const auto cwd_start=output.find("CWD="); assert(cwd_start!=std::string::npos);
    const auto cwd_end=output.find('\n',cwd_start);
    auto cwd=output.substr(cwd_start+4,cwd_end-cwd_start-4); if (cwd.ends_with('\r')) cwd.pop_back();
    assert(fs::equivalent(fs::path(wide(cwd)),first.session));
    assert(output.find("PS3_SAVEDATA_ROOT="+utf8(plan.saves.wstring()))!=std::string::npos);
    assert(output.find("RSX_SHADER_CACHE_DIR="+utf8(plan.cache.wstring()))!=std::string::npos);
    assert(bytes(first.session/L"game.stderr.log").find("Fixture stderr")!=std::string::npos);
    assert(bytes(first.session/L"launcher.log").find("ExitCode=0")!=std::string::npos);
    plan.environment[L"HAZE_TEST_EXIT"]=L"37"; auto second=run(plan);
    assert(second.started && second.exit_code==37 && !second.error.empty() && second.session!=first.session);
    assert(bytes(second.session/L"game.stdout.log").find("PS3_SAVEDATA_ROOT="+utf8(plan.saves.wstring()))!=std::string::npos);
    plan.environment.erase(L"HAZE_TEST_EXIT"); plan.environment.erase(L"HAZE_TEST_HOLD_MS");
    const auto valid=plan;
    plan.executable=root/L"missing.exe"; assert(!validate(plan,repair).empty() && repair==L"Executable");
    plan=valid; plan.executable=module; assert(!validate(plan,repair).empty() && repair==L"Executable");
    plan=valid; plan.elf=root/L"missing.elf"; assert(!validate(plan,repair).empty() && repair==L"DiscDirectory");
    plan=valid; plan.firmware=root/L"missing firmware"; assert(!validate(plan,repair).empty() && repair==L"Firmware");
    plan=valid; plan.input=root/L"missing.ini"; assert(!validate(plan,repair).empty() && repair.empty());
    // Exercise the real GUI-subsystem launcher with dialogs explicitly disabled.
    assert(launch(module,L"--no-ui --check",root/L"unrelated cwd")==0);
    SetEnvironmentVariableW(L"HAZE_TEST_EXIT",L"37"); assert(launch(module,L"--no-ui",root/L"unrelated cwd")==37); SetEnvironmentVariableW(L"HAZE_TEST_EXIT",nullptr);
    assert(launch(module,L"--no-ui --unknown",root/L"unrelated cwd")==1);
    fs::remove(root/L"bin/HazeLaunchFixtureRuntime.dll"); assert(launch(module,L"--no-ui",root/L"unrelated cwd")==0xc0000135u);
    fs::current_path(original_cwd);
    // Only the per-process fixture below the provided test directory is removed.
    fs::remove_all(root);
    std::puts("Launcher: configuration precedence, cwd independence, UTF-16/spaced paths, quoting, atomic settings, validation, restricted handles, console-free direct process launch, persistent paths, logs, waiting and GUI exit-code propagation passed.");
    } catch (const std::exception& e) { std::fprintf(stderr,"Launcher test exception: %s\n",e.what()); return 1; }
}
