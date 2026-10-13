#include "launch.h"
#include "../haze_window_icon.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <algorithm>
#include <stdexcept>

namespace {
using namespace haze_launch;
HRESULT CALLBACK dialog_icon(HWND window,UINT notification,WPARAM,LPARAM,LONG_PTR) {
    if (notification==TDN_CREATED) haze_window_icon(window);
    return S_OK;
}
int failure(const std::wstring& message,const std::wstring& repair,const fs::path& logs) {
    std::vector<TASKDIALOG_BUTTON> buttons;
    if (!repair.empty()) buttons.push_back({100,L"Choose location..."});
    if (!logs.empty()) buttons.push_back({101,L"Open session logs"});
    TASKDIALOGCONFIG dialog{}; dialog.cbSize=sizeof(dialog); dialog.dwFlags=TDF_SIZE_TO_CONTENT|TDF_ALLOW_DIALOG_CANCELLATION;
    dialog.pfCallback=dialog_icon;
    dialog.pszWindowTitle=L"Haze Recomp"; dialog.pszMainInstruction=L"Haze needs your attention";
    const auto content=message+(logs.empty() ? L"" : L"\n\nSession: "+logs.wstring());
    dialog.pszContent=content.c_str(); dialog.pszMainIcon=TD_ERROR_ICON;
    dialog.dwCommonButtons=TDCBF_CLOSE_BUTTON; dialog.pButtons=buttons.data(); dialog.cButtons=UINT(buttons.size()); dialog.nDefaultButton=IDCLOSE;
    int chosen=IDCLOSE;
    if (FAILED(TaskDialogIndirect(&dialog,&chosen,nullptr,nullptr))) MessageBoxW(nullptr,content.c_str(),L"Haze Recomp",MB_OK|MB_ICONERROR);
    return chosen;
}
fs::path pick(const std::wstring& key) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return {};
    FILEOPENDIALOGOPTIONS flags{}; dialog->GetOptions(&flags);
    flags|=FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR;
    if (key!=L"Executable") flags|=FOS_PICKFOLDERS;
    else {
        flags|=FOS_FILEMUSTEXIST; COMDLG_FILTERSPEC filter{L"Windows executable",L"*.exe"}; dialog->SetFileTypes(1,&filter);
    }
    dialog->SetOptions(flags);
    dialog->SetTitle(key==L"Firmware" ? L"Choose the extracted PS3 dev_flash folder" : key==L"Executable" ? L"Choose HazeRecomp.exe" : L"Choose the Haze disc folder containing PS3_GAME");
    if (FAILED(dialog->Show(nullptr))) return {};
    Microsoft::WRL::ComPtr<IShellItem> item; if (FAILED(dialog->GetResult(&item))) return {};
    PWSTR path=nullptr; if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) return {};
    fs::path result(path); CoTaskMemFree(path); return result;
}
void diagnostic(const std::wstring& message) {
    const auto bytes=utf8(message+L"\n"); const auto out=GetStdHandle(STD_ERROR_HANDLE); DWORD written=0;
    if (out && out!=INVALID_HANDLE_VALUE) WriteFile(out,bytes.data(),DWORD(bytes.size()),&written,nullptr);
}
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    using namespace haze_launch;
    // Native loader errors are reported with exit code and logs instead of a
    // second operating-system fault dialog or an unattended crash reporter.
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct Finish { HRESULT result; ~Finish() { if (SUCCEEDED(result)) CoUninitialize(); } } finish{com};
    Options options;
    try {
        int count=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&count);
        if (!argv) throw std::runtime_error("Cannot read launcher arguments.");
        std::vector<std::wstring> args; for (int i=1;i<count;++i) args.emplace_back(argv[i]); LocalFree(argv);
        // Honor no-ui even when another argument is invalid, for diagnostics.
        options.no_ui=std::find(args.begin(),args.end(),L"--no-ui")!=args.end();
        options=parse(args);
        wchar_t module[32768]{}; const auto length=GetModuleFileNameW(nullptr,module,32768);
        if (!length || length>=32768) throw std::runtime_error("Cannot locate the launcher executable.");
        const auto inherited=environment(); Plan plan;
        for (;;) {
            plan=resolve(fs::path(module),options,inherited);
            std::wstring repair; const auto problem=validate(plan,repair);
            if (problem.empty()) break;
            diagnostic(problem);
            if (options.no_ui || options.check || failure(problem,repair,{})!=100) return 1;
            const auto location=pick(repair); if (location.empty()) return 1;
            auto edited=options; edited.overrides[repair]=location.wstring();
            const auto candidate=resolve(fs::path(module),edited,inherited);
            std::wstring next_repair; const auto next_problem=validate(candidate,next_repair);
            // Never persist a picker selection that leaves this same input bad.
            if (!next_problem.empty() && next_repair==repair) { failure(next_problem,L"",{}); continue; }
            std::wstring error;
            if (!save_setting(plan.config,repair,location.wstring(),error)) { failure(error,L"",{}); return 1; }
            options=std::move(edited);
        }
        if (options.check) {
            diagnostic(L"Launch configuration is valid.\nExecutable: "+plan.executable.wstring()+L"\nSavedata: "+plan.saves.wstring()+L"\nShader cache: "+plan.cache.wstring());
            return 0;
        }
        const auto result=run(plan);
        if (!result.error.empty()) {
            diagnostic(result.error+(result.session.empty() ? L"" : L"\nSession: "+result.session.wstring()));
            if (!options.no_ui && failure(result.error,L"",result.session)==101)
                ShellExecuteW(nullptr,L"open",result.session.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        }
        return int(result.exit_code);
    } catch (const std::exception& e) {
        const auto message=error_text(e.what()); diagnostic(message);
        if (!options.no_ui) failure(message,L"",{}); return 1;
    }
}
