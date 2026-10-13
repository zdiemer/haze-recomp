#pragma once
#ifdef _WIN32
#include <windows.h>
#include <map>
// Window-thread API. Cache owned resource icons by actual dimensions for the
// process lifetime; LR_SHARED's resource-name cache ignores requested size.
inline void haze_window_icon(HWND window,UINT requested_dpi=0) {
    static std::map<std::pair<int,int>,HICON> icons;
    const auto module=GetModuleHandleW(nullptr);
    const auto user32=GetModuleHandleW(L"user32.dll");
    const auto window_dpi=(UINT(WINAPI*)(HWND))GetProcAddress(user32,"GetDpiForWindow");
    const auto metric_dpi=(int(WINAPI*)(int,UINT))GetProcAddress(user32,"GetSystemMetricsForDpi");
    const UINT dpi=requested_dpi ? requested_dpi : window_dpi ? window_dpi(window) : 96;
    const auto metric=[&](int id) { return metric_dpi && dpi ? metric_dpi(id,dpi) : GetSystemMetrics(id); };
    for (int small=0;small<2;++small) {
        const int width=metric(small ? SM_CXSMICON : SM_CXICON);
        const int height=metric(small ? SM_CYSMICON : SM_CYICON);
        auto& icon=icons[{width,height}];
        if (!icon) icon=(HICON)LoadImageW(module,MAKEINTRESOURCEW(101),IMAGE_ICON,width,height,0);
        if (icon) SendMessageW(window,WM_SETICON,small ? ICON_SMALL : ICON_BIG,(LPARAM)icon);
    }
}
#endif
