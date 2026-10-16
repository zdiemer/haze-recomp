#include "session.h"

namespace haze::net {
std::string normalize_code(std::string code) {
    std::string out;
    for (unsigned char c:code) {
        if (c=='-' || c==' ') continue;
        if(c>='a' && c<='z') c-=32;
        if(std::string_view("ABCDEFGHJKLMNPQRSTUVWXYZ23456789").find(char(c))==std::string_view::npos) return {};
        out+=char(c);
    }
    return out.size()==12 ? out : std::string{};
}
}
