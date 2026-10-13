#include "haze_steam.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <charconv>
#include <map>
#include <stdexcept>
#include <string_view>

namespace steam_names {
constexpr size_t max_input=2*1024*1024;
struct Node { std::string key,value; std::vector<Node> children; bool object=false; };
std::string lower(std::string value) {
    for (auto& c:value) if (c>='A' && c<='Z') c+=32;
    return value;
}
struct Reader {
    std::string_view input;
    size_t offset=0,count=0;
    void whitespace() {
        for (;;) {
            while (offset<input.size() && (input[offset]==' ' || input[offset]=='\t' || input[offset]=='\r' || input[offset]=='\n')) ++offset;
            if (input.substr(offset,2)!="//") return;
            while (offset<input.size() && input[offset]!='\n') ++offset;
        }
    }
    std::string token() {
        whitespace();
        if (offset==input.size()) throw std::runtime_error("Incomplete metadata");
        std::string value;
        if (input[offset]=='\"') {
            ++offset; bool closed=false;
            while (offset<input.size()) {
                char c=input[offset++];
                if (c=='\"') { closed=true; break; }
                if (c=='\\') {
                    if (offset==input.size()) throw std::runtime_error("Incomplete escape");
                    c=input[offset++];
                    if (c=='n') c='\n'; else if (c=='r') c='\r'; else if (c=='t') c='\t';
                    else if (c!='\\' && c!='\"') throw std::runtime_error("Unsupported escape");
                }
                value+=c;
                if (value.size()>16384) throw std::runtime_error("Metadata token too long");
            }
            if (!closed) throw std::runtime_error("Unclosed metadata string");
        } else {
            while (offset<input.size()) {
                const char c=input[offset];
                if (c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='{' || c=='}') break;
                if (c=='\"' || c=='[' || c==']' || c=='#') throw std::runtime_error("Unsupported metadata syntax");
                value+=c; ++offset;
                if (value.size()>16384) throw std::runtime_error("Metadata token too long");
            }
            if (value.empty()) throw std::runtime_error("Missing metadata token");
        }
        return value;
    }
    std::vector<Node> object(unsigned depth) {
        if (depth>16) throw std::runtime_error("Metadata nested too deeply");
        std::vector<Node> nodes;
        for (;;) {
            whitespace();
            if (offset==input.size()) { if (depth) throw std::runtime_error("Incomplete metadata object"); return nodes; }
            if (input[offset]=='}') {
                if (!depth) throw std::runtime_error("Unexpected closing brace");
                ++offset; return nodes;
            }
            if (++count>10000) throw std::runtime_error("Too many metadata fields");
            Node node; node.key=token(); whitespace();
            if (offset<input.size() && input[offset]=='{') { ++offset; node.object=true; node.children=object(depth+1); }
            else node.value=token();
            nodes.push_back(std::move(node));
        }
    }
};
const Node* unique(const std::vector<Node>& nodes,const std::string& key) {
    const Node* found=nullptr;
    for (const auto& node:nodes) if (lower(node.key)==key) {
        if (found) throw std::runtime_error("Ambiguous metadata field");
        found=&node;
    }
    return found;
}
std::string label(const std::string& text) {
    // Keep display text on one line and bounded without splitting UTF-8. Names
    // are advisory; the unmodified numeric account ID is always shown first.
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if (!size) return {};
    std::wstring wide(size,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),wide.data(),size);
    std::wstring clean; unsigned points=0;
    for (size_t i=0;i<wide.size();++i) {
        wchar_t c=wide[i];
        if (c<32 || (c>=127 && c<=159) || c==0x2028 || c==0x2029) c=L' ';
        if ((c>=0x202a && c<=0x202e) || (c>=0x2066 && c<=0x2069) || c==0x200e || c==0x200f || c==0x061c) continue;
        if (points++==64) { clean+=L"..."; break; }
        clean+=c;
        if (c>=0xd800 && c<=0xdbff && i+1<wide.size()) clean+=wide[++i];
    }
    const auto first=clean.find_first_not_of(L' '); if (first==clean.npos) return {};
    clean=clean.substr(first,clean.find_last_not_of(L' ')-first+1);
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,clean.data(),int(clean.size()),nullptr,0,nullptr,nullptr);
    if (!bytes) return {};
    std::string result(bytes,'\0'); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,clean.data(),int(clean.size()),result.data(),bytes,nullptr,nullptr); return result;
}
}

bool haze_steam_profile_names(const std::string& input,std::vector<HazeSteamProfile>& profiles) {
    using namespace steam_names;
    for (auto& profile:profiles) profile.persona_name.clear();
    try {
        if (input.empty() || input.size()>max_input || input.find('\0')!=input.npos) return false;
        std::string_view text=input; if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
        Reader reader{text}; const auto roots=reader.object(0); const auto users=unique(roots,"users");
        if (!users || !users->object) return false;
        std::map<std::string,std::string> names;
        for (const auto& user:users->children) {
            if (!user.object) continue;
            uint64_t id=0; const auto parsed=std::from_chars(user.key.data(),user.key.data()+user.key.size(),id);
            if (parsed.ec!=std::errc{} || parsed.ptr!=user.key.data()+user.key.size() || std::to_string(id)!=user.key) continue;
            // CSteamID: public universe 1, individual type 1, desktop instance 1,
            // low 32 bits are the userdata account ID. Reject other ID classes.
            if ((id>>32)!=0x01100001ull || !uint32_t(id)) continue;
            const auto name=unique(user.children,"personaname");
            const auto account=std::to_string(uint32_t(id));
            if (!names.emplace(account,name && !name->object ? label(name->value) : std::string{}).second)
                throw std::runtime_error("Ambiguous Steam account metadata");
        }
        for (auto& profile:profiles) if (auto found=names.find(profile.account_id);found!=names.end()) profile.persona_name=found->second;
        return true;
    } catch (const std::exception&) { return false; }
}
