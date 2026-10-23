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
    if (!logs.empty()) buttons.push_back({102,L"Export diagnostics..."});
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
    dialog->SetTitle(key==L"Firmware" ? L"Choose the extracted PS3 dev_flash folder" : key==L"Executable" ? L"Choose HazeRecomp.exe" :
        key==L"Diagnostics" ? L"Choose a folder for the diagnostics report" : L"Choose the Haze disc folder containing PS3_GAME");
    if (FAILED(dialog->Show(nullptr))) return {};
    Microsoft::WRL::ComPtr<IShellItem> item; if (FAILED(dialog->GetResult(&item))) return {};
    PWSTR path=nullptr; if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) return {};
    fs::path result(path); CoTaskMemFree(path); return result;
}
void diagnostic(const std::wstring& message) {
    const auto bytes=utf8(message+L"\n"); const auto out=GetStdHandle(STD_ERROR_HANDLE); DWORD written=0;
    if (out && out!=INVALID_HANDLE_VALUE) WriteFile(out,bytes.data(),DWORD(bytes.size()),&written,nullptr);
}
// Folder picker, export and a result dialog; collection lives in diagnostics.cpp.
void export_ui(const Plan& plan,const fs::path& session,const Values& inherited) {
    for (;;) {
        const auto folder=pick(L"Diagnostics"); if (folder.empty()) return;
        std::wstring error; const auto report=export_diagnostics(plan,session,folder,inherited,error);
        if (report.empty()) { diagnostic(error); if (failure(error+L"\n\nEarlier reports and the session logs were not changed.",L"Diagnostics",{})==100) continue; return; }
        diagnostic(L"Diagnostics report: "+report.wstring());
        TASKDIALOG_BUTTON show{100,L"Show in folder"};
        TASKDIALOGCONFIG dialog{}; dialog.cbSize=sizeof(dialog); dialog.dwFlags=TDF_SIZE_TO_CONTENT|TDF_ALLOW_DIALOG_CANCELLATION; dialog.pfCallback=dialog_icon;
        dialog.pszWindowTitle=L"Haze Recomp"; dialog.pszMainInstruction=L"Diagnostics report saved"; dialog.pszMainIcon=TD_INFORMATION_ICON;
        const auto content=L"Review the report before attaching it to an issue. Nothing was uploaded. "
            L"Personal paths and names were replaced where they could be identified.\n\n"+report.wstring();
        dialog.pszContent=content.c_str(); dialog.dwCommonButtons=TDCBF_CLOSE_BUTTON; dialog.pButtons=&show; dialog.cButtons=1; dialog.nDefaultButton=100;
        int chosen=IDCLOSE;
        if (FAILED(TaskDialogIndirect(&dialog,&chosen,nullptr,nullptr))) MessageBoxW(nullptr,content.c_str(),L"Haze Recomp",MB_OK|MB_ICONINFORMATION);
        if (chosen==100) ShellExecuteW(nullptr,L"open",L"explorer.exe",(L"/select,"+quote(report.wstring())).c_str(),nullptr,SW_SHOWNORMAL);
        return;
    }
}
// Manual export for problems that did not end in a crash dialog.
int diagnostics_ui(const Plan& plan,const std::wstring& name,const Values& inherited) {
    auto found=sessions(plan.sessions,10);
    if (!name.empty()) { const auto folder=find_session(plan,name); found={session(folder)}; }
    if (found.empty()) { failure(L"No session logs were found. Launch Haze once, then export diagnostics.\n\n"+plan.sessions.wstring(),L"",{}); return 1; }
    std::vector<std::wstring> labels; std::vector<TASKDIALOG_BUTTON> choices;
    for (const auto& item:found) labels.push_back(item.name+L" \u2014 "+wide(describe(item)));
    for (size_t i=0;i<labels.size();++i) choices.push_back({int(200+i),labels[i].c_str()});
    TASKDIALOG_BUTTON button{102,L"Export diagnostics..."};
    TASKDIALOGCONFIG dialog{}; dialog.cbSize=sizeof(dialog); dialog.dwFlags=TDF_SIZE_TO_CONTENT|TDF_ALLOW_DIALOG_CANCELLATION; dialog.pfCallback=dialog_icon;
    dialog.pszWindowTitle=L"Haze Recomp"; dialog.pszMainInstruction=L"Export diagnostics";
    dialog.pszContent=L"Choose a session. Newest sessions are listed first; names are UTC launch times. The report contains bounded logs, "
        L"settings, build and system information, and excludes game files, saves and firmware.";
    dialog.pRadioButtons=choices.data(); dialog.cRadioButtons=UINT(choices.size()); dialog.nDefaultRadioButton=200;
    dialog.pButtons=&button; dialog.cButtons=1; dialog.nDefaultButton=102; dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;
    int chosen=IDCANCEL,radio=200;
    if (FAILED(TaskDialogIndirect(&dialog,&chosen,&radio,nullptr)) || chosen!=102) return 1;
    const size_t index=size_t(radio-200); if (index>=found.size()) return 1;
    export_ui(plan,found[index].path,inherited); return 0;
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
        if (options.diagnostics || !options.export_to.empty()) {
            // Exporting does not require a launchable configuration.
            plan=resolve(fs::path(module),options,inherited,true);
            if (options.export_to.empty()) {
                if (options.no_ui) { diagnostic(L"--diagnostics shows dialogs. Use --export-diagnostics FOLDER with --no-ui."); return 1; }
                return diagnostics_ui(plan,options.session,inherited);
            }
            std::wstring error; const auto report=export_diagnostics(plan,find_session(plan,options.session),options.export_to,inherited,error);
            if (report.empty()) { diagnostic(error); if (!options.no_ui) failure(error,L"",{}); return 1; }
            diagnostic(L"Diagnostics report: "+report.wstring()); return 0;
        }
        for (;;) {
            plan=resolve(fs::path(module),options,inherited);
            std::wstring repair; const auto problem=validate(plan,repair);
            if (problem.empty()) break;
            diagnostic(problem);
            if (options.no_ui || options.check) return 1;
            const int choice=failure(problem,repair,{});
            if (choice!=100) return 1;
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
            const int choice=options.no_ui ? IDCLOSE : failure(result.error,L"",result.session);
            if (choice==101) ShellExecuteW(nullptr,L"open",result.session.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            if (choice==102) export_ui(plan,result.session,inherited);
        }
        return int(result.exit_code);
    } catch (const std::exception& e) {
        const auto message=error_text(e.what()); diagnostic(message);
        if (!options.no_ui) failure(message,L"",{}); return 1;
    }
}
