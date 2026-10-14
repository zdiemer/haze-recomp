#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <cstring>
#include <cctype>

inline std::string haze_key_name(int key) {
    if ((key>='A' && key<='Z') || (key>='0' && key<='9')) return std::string(1,char(key));
    const struct { int key; const char* name; } names[]={
        {0,"None"},{VK_SPACE,"Space"},{VK_RETURN,"Enter"},{VK_ESCAPE,"Escape"},{VK_TAB,"Tab"},
        {VK_SHIFT,"Shift"},{VK_CONTROL,"Ctrl"},{VK_MENU,"Alt"},
        {VK_UP,"Up"},{VK_DOWN,"Down"},{VK_LEFT,"Left"},{VK_RIGHT,"Right"},
        {VK_LBUTTON,"Mouse1"},{VK_RBUTTON,"Mouse2"},{VK_MBUTTON,"Mouse3"},{VK_XBUTTON1,"Mouse4"},{VK_XBUTTON2,"Mouse5"},
        {VK_BACK,"Backspace"},{VK_DELETE,"Delete"},{VK_INSERT,"Insert"},{VK_HOME,"Home"},{VK_END,"End"},
        {VK_PRIOR,"PageUp"},{VK_NEXT,"PageDown"}};
    for (const auto& item:names) if (item.key==key) return item.name;
    if (key>=VK_F2 && key<=VK_F12 && key!=VK_F6 && key!=VK_F7) return "F"+std::to_string(key-VK_F1+1);
    return {};
}
inline int haze_key_code(const char* text) {
    if (text[0] && !text[1]) {
        const int key=std::toupper((unsigned char)text[0]);
        return (key>='A' && key<='Z') || (key>='0' && key<='9') ? key : -1;
    }
    for (int key=0;key<256;++key) {
        const auto name=haze_key_name(key);
        if (!name.empty() && !_stricmp(name.c_str(),text)) return key;
    }
    return -1;
}
