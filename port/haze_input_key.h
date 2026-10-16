#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <cstring>
#include <cctype>

// Wheel pulses use unassigned virtual-key codes, sharing the key binding space.
constexpr int HAZE_VK_WHEEL_UP=0x0E, HAZE_VK_WHEEL_DOWN=0x0F;
// One table drives INI parsing, display names and Kenney prompt selection.
// A '#' icon is a readable text glyph for keys without a Kenney PNG.
struct HazeKeyName { int key; const char* name; const char* icon; };
inline const HazeKeyName* haze_key_entry(int key) {
    static const HazeKeyName names[]={
        {0,"None",""},{VK_SPACE,"Space","keyboard_space"},{VK_RETURN,"Enter","keyboard_enter"},
        {VK_ESCAPE,"Escape","keyboard_escape"},{VK_TAB,"Tab","keyboard_tab"},
        {VK_SHIFT,"Shift","keyboard_shift"},{VK_CONTROL,"Ctrl","keyboard_ctrl"},{VK_MENU,"Alt","keyboard_alt"},
        {VK_UP,"Up","keyboard_arrow_up"},{VK_DOWN,"Down","keyboard_arrow_down"},
        {VK_LEFT,"Left","keyboard_arrow_left"},{VK_RIGHT,"Right","keyboard_arrow_right"},
        {VK_LBUTTON,"Mouse1","mouse_left"},{VK_RBUTTON,"Mouse2","mouse_right"},{VK_MBUTTON,"Mouse3","mouse_scroll"},
        {VK_XBUTTON1,"Mouse4","mouse_side_back"},{VK_XBUTTON2,"Mouse5","mouse_side_forward"},
        {HAZE_VK_WHEEL_UP,"WheelUp","mouse_scroll_up"},{HAZE_VK_WHEEL_DOWN,"WheelDown","mouse_scroll_down"},
        {VK_BACK,"Backspace","keyboard_backspace"},{VK_DELETE,"Delete","keyboard_delete"},{VK_INSERT,"Insert","keyboard_insert"},
        {VK_HOME,"Home","keyboard_home"},{VK_END,"End","keyboard_end"},
        {VK_PRIOR,"PageUp","keyboard_page_up"},{VK_NEXT,"PageDown","keyboard_page_down"},
        {VK_NUMPAD0,"Num0","#Num 0"},{VK_NUMPAD1,"Num1","#Num 1"},{VK_NUMPAD2,"Num2","#Num 2"},{VK_NUMPAD3,"Num3","#Num 3"},
        {VK_NUMPAD4,"Num4","#Num 4"},{VK_NUMPAD5,"Num5","#Num 5"},{VK_NUMPAD6,"Num6","#Num 6"},{VK_NUMPAD7,"Num7","#Num 7"},
        {VK_NUMPAD8,"Num8","#Num 8"},{VK_NUMPAD9,"Num9","#Num 9"},{VK_MULTIPLY,"NumMultiply","#Num *"},
        {VK_ADD,"NumAdd","keyboard_numpad_plus"},{VK_SUBTRACT,"NumSubtract","#Num -"},
        {VK_DECIMAL,"NumDecimal","#Num ."},{VK_DIVIDE,"NumDivide","#Num /"},
        {VK_OEM_1,"Semicolon","keyboard_semicolon"},{VK_OEM_PLUS,"Equals","keyboard_equals"},
        {VK_OEM_COMMA,"Comma","keyboard_comma"},{VK_OEM_MINUS,"Minus","keyboard_minus"},
        {VK_OEM_PERIOD,"Period","keyboard_period"},{VK_OEM_2,"Slash","keyboard_slash_forward"},
        {VK_OEM_3,"Grave","keyboard_tilde"},{VK_OEM_4,"LeftBracket","keyboard_bracket_open"},
        {VK_OEM_5,"Backslash","keyboard_slash_back"},{VK_OEM_6,"RightBracket","keyboard_bracket_close"},
        {VK_OEM_7,"Apostrophe","keyboard_apostrophe"}};
    for (const auto& item:names) if (item.key==key) return &item;
    return nullptr;
}
inline std::string haze_key_name(int key) {
    if ((key>='A' && key<='Z') || (key>='0' && key<='9')) return std::string(1,char(key));
    if (const auto* item=haze_key_entry(key)) return item->name;
    if (key>=VK_F2 && key<=VK_F12 && key!=VK_F6 && key!=VK_F7) return "F"+std::to_string(key-VK_F1+1);
    return {};
}
inline std::string haze_key_icon(int key) {
    if ((key>='A' && key<='Z') || (key>='0' && key<='9')) return "keyboard_"+std::string(1,char(std::tolower(key)));
    if (const auto* item=haze_key_entry(key)) return item->icon;
    const auto name=haze_key_name(key);
    return name.empty() ? name : "keyboard_f"+name.substr(1);
}
// Host hotkeys that bindings and the capture dialog cannot take.
inline const char* haze_key_reserved(int key) {
    switch (key) {
    case VK_F1: return "F1 is reserved for mouse capture";
    case VK_F6: return "F6 is reserved for reloading video settings";
    case VK_F7: return "F7 is reserved for the performance overlay";
    case VK_ESCAPE: return "Escape is reserved for pause and menu back";
    }
    return nullptr;
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
