#pragma once
#include <charconv>
#include <string>
#include <string_view>

// Raw joystick sources use SDL mapping notation, prefixed to distinguish them
// from named gamepad controls: raw:b0, raw:+a1, raw:-a1, raw:a1, raw:h0.1.
struct HazeRawSource {
    enum Kind { None, Button, Axis, Hat } kind=None;
    int index=0,sign=0,mask=0;
};
inline HazeRawSource haze_raw_source(std::string_view text) {
    HazeRawSource result;
    if (!text.starts_with("raw:")) return result;
    text.remove_prefix(4);
    if (text.empty()) return result;
    int sign=0;
    if (text.front()=='+' || text.front()=='-') { sign=text.front()=='+' ? 1 : -1; text.remove_prefix(1); }
    if (text.empty()) return result;
    const char type=text.front(); text.remove_prefix(1);
    int index=0;
    auto parsed=std::from_chars(text.data(),text.data()+text.size(),index);
    if (parsed.ec!=std::errc{} || index<0 || index>255) return result;
    if (type=='h' && !sign && parsed.ptr!=text.data()+text.size() && *parsed.ptr=='.') {
        int mask=0; auto tail=std::from_chars(parsed.ptr+1,text.data()+text.size(),mask);
        if (tail.ec!=std::errc{} || tail.ptr!=text.data()+text.size() ||
            (mask!=1 && mask!=2 && mask!=4 && mask!=8)) return result;
        return {HazeRawSource::Hat,index,0,mask};
    }
    if (parsed.ptr!=text.data()+text.size()) return result;
    if (type=='b' && !sign) return {HazeRawSource::Button,index,0,0};
    if (type=='a') return {HazeRawSource::Axis,index,sign,0};
    return result;
}
