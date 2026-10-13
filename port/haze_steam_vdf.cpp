#include "haze_steam.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace steam_vdf {
constexpr size_t limit=16*1024*1024;
constexpr const char* marker="Haze Recomp (managed)";
struct Node {
    unsigned char type=0;
    std::string key,data;
    std::vector<Node> children;
};
std::string lower(std::string value) {
    for (char& c:value) if (c>='A' && c<='Z') c+=32;
    return value;
}
struct Reader {
    const std::string& bytes;
    size_t offset=0,count=0;
    std::string take(size_t n) {
        if (n>bytes.size()-offset) throw std::runtime_error("Truncated Steam shortcuts file.");
        auto result=bytes.substr(offset,n); offset+=n; return result;
    }
    std::string string() {
        const auto end=bytes.find('\0',offset);
        if (end==std::string::npos || end-offset>1024*1024) throw std::runtime_error("Invalid Steam shortcut string.");
        auto result=bytes.substr(offset,end-offset); offset=end+1; return result;
    }
    std::vector<Node> object(unsigned depth) {
        if (depth>32) throw std::runtime_error("Steam shortcut nesting is too deep.");
        std::vector<Node> result; std::unordered_set<std::string> keys;
        for (;;) {
            const auto type=static_cast<unsigned char>(take(1)[0]);
            if (type==8) return result;
            if (++count>100000) throw std::runtime_error("Too many Steam shortcut fields.");
            Node node; node.type=type; node.key=string();
            if (!keys.insert(lower(node.key)).second) throw std::runtime_error("Duplicate Steam shortcut fields; no changes made.");
            if (!type) node.children=object(depth+1);
            else if (type==1) { node.data=string(); node.data+='\0'; }
            else if (type==2 || type==3 || type==4 || type==6) node.data=take(4);
            else if (type==7) node.data=take(8);
            else throw std::runtime_error("Unsupported Steam shortcuts format; no changes made.");
            result.push_back(std::move(node));
        }
    }
};
void write(const std::vector<Node>& nodes,std::string& bytes) {
    for (const auto& node:nodes) {
        bytes+=char(node.type); bytes+=node.key; bytes+='\0';
        if (!node.type) write(node.children,bytes); else bytes+=node.data;
    }
    bytes+=char(8);
}
Node* find(Node& object,const char* key) {
    const auto name=lower(key);
    for (auto& n:object.children) if (lower(n.key)==name) return &n;
    return nullptr;
}
std::string string(Node& object,const char* key) {
    auto n=find(object,key); if (!n) return {};
    if (n->type!=1) throw std::runtime_error("Unexpected Steam shortcut field type.");
    return n->data.substr(0,n->data.size()-1);
}
uint32_t integer(Node& object,const char* key) {
    auto n=find(object,key);
    if (!n || n->type!=2) throw std::runtime_error("Missing or invalid Steam shortcut ID.");
    uint32_t v=0; for (unsigned i=0;i<4;++i) v|=uint32_t(static_cast<unsigned char>(n->data[i]))<<(i*8);
    return v;
}
void set(Node& object,const char* key,unsigned char type,std::string data) {
    if (auto n=find(object,key)) { n->type=type; n->data=std::move(data); n->children.clear(); }
    else object.children.push_back({type,key,std::move(data),{}});
}
void set_string(Node& object,const char* key,std::string value) { value+='\0'; set(object,key,1,std::move(value)); }
void set_integer(Node& object,const char* key,uint32_t value) {
    std::string bytes; for (unsigned i=0;i<4;++i) bytes+=char(value>>(i*8)); set(object,key,2,std::move(bytes));
}
uint32_t crc32(const std::string& bytes) {
    uint32_t crc=0xffffffffu;
    for (unsigned char c:bytes) { crc^=c; for (int i=0;i<8;++i) crc=(crc>>1)^(0xedb88320u&uint32_t(-int(crc&1))); }
    return ~crc;
}
std::string path(const std::filesystem::path& p) {
    if (p.empty() || !p.is_absolute()) throw std::runtime_error("The launcher, working directory and icon must have absolute paths.");
    const auto utf8=p.lexically_normal().u8string(); std::string text(utf8.begin(),utf8.end());
    if (text.find_first_of("\"\r\n")!=std::string::npos || text.find('\0')!=std::string::npos)
        throw std::runtime_error("A launcher path contains unsupported characters.");
    return text;
}
}

bool haze_steam_plan(const std::string& input,const HazeSteamTarget& target,HazeSteamPlan& plan,std::string& message) {
    using namespace steam_vdf;
    plan={}; message.clear();
    try {
        if (input.size()>limit) throw std::runtime_error("Steam shortcuts file exceeds the supported size.");
        const std::string exe='"'+path(target.launcher)+'"',dir='"'+path(target.working_directory)+'"',icon=path(target.icon);
        if (target.arguments.size()>65536 || target.arguments.find('\0')!=std::string::npos || target.arguments.find_first_of("\r\n")!=std::string::npos)
            throw std::runtime_error("Invalid launcher arguments.");
        std::vector<Node> roots;
        if (input.empty()) roots.push_back({0,"shortcuts",{}, {}});
        else {
            Reader reader{input}; roots=reader.object(0);
            if (reader.offset!=input.size()) throw std::runtime_error("Extra data follows the Steam shortcuts file.");
        }
        if (roots.size()!=1 || roots[0].type || lower(roots[0].key)!="shortcuts")
            throw std::runtime_error("Not a Steam shortcuts file.");
        auto& shortcuts=roots[0].children;
        Node* managed=nullptr; bool unowned_match=false;
        std::unordered_map<uint32_t,unsigned> ids;
        std::unordered_set<uint32_t> indices;
        for (auto& shortcut:shortcuts) {
            uint32_t index=0;
            auto parsed=std::from_chars(shortcut.key.data(),shortcut.key.data()+shortcut.key.size(),index);
            if (shortcut.type || parsed.ec!=std::errc{} || parsed.ptr!=shortcut.key.data()+shortcut.key.size() || !indices.insert(index).second)
                throw std::runtime_error("Invalid Steam shortcut entry index.");
            if (find(shortcut,"appid")) {
                ++ids[integer(shortcut,"appid")];
            }
            bool owns=false;
            if (auto tags=find(shortcut,"tags")) {
                if (tags->type) throw std::runtime_error("Invalid Steam shortcut tags.");
                for (const auto& tag:tags->children)
                    if (tag.type==1 && tag.data==std::string(marker)+'\0') owns=true;
            }
            if (owns) {
                if (managed) throw std::runtime_error("More than one managed Haze shortcut exists; remove the duplicate in Steam first.");
                managed=&shortcut;
            } else if (lower(string(shortcut,"AppName"))=="haze recomp" || lower(string(shortcut,"Exe"))==lower(exe)) unowned_match=true;
        }
        if (unowned_match) throw std::runtime_error("An existing Haze shortcut is not managed by this menu. Remove or rename that entry in Steam before retrying; it was left unchanged.");
        const bool created=!managed;
        uint32_t appid=0;
        if (created) {
            appid=crc32(exe+"Haze Recomp")|0x80000000u;
            if (ids.contains(appid)) throw std::runtime_error("The new shortcut ID collides with another game; no changes made.");
            uint32_t index=0; while (indices.contains(index)) ++index;
            shortcuts.push_back({0,std::to_string(index),{}, {}}); managed=&shortcuts.back();
            set_integer(*managed,"appid",appid); set_string(*managed,"AppName","Haze Recomp");
            set_string(*managed,"ShortcutPath",""); set_integer(*managed,"IsHidden",0);
            set_integer(*managed,"AllowDesktopConfig",1); set_integer(*managed,"AllowOverlay",1);
            set_integer(*managed,"OpenVR",0); set_integer(*managed,"Devkit",0);
            set_string(*managed,"DevkitGameID",""); set_integer(*managed,"DevkitOverrideAppID",0);
            set_integer(*managed,"LastPlayTime",0); set_string(*managed,"FlatpakAppID","");
            Node tags{0,"tags",{}, {}}; set_string(tags,"0",marker); managed->children.push_back(std::move(tags));
        } else {
            appid=integer(*managed,"appid");
            if (!(appid&0x80000000u)) throw std::runtime_error("The managed shortcut has an invalid non-Steam ID.");
            if (ids[appid]!=1) throw std::runtime_error("The Haze shortcut shares an ID with another entry. Resolve that collision in Steam before retrying.");
        }
        const auto previous_icon=string(*managed,"icon"),previous_exe=string(*managed,"Exe");
        // Steam categories, display name, history, unknown fields and custom icon
        // survive repair. The original executable-as-icon follows relocation.
        if (previous_icon.empty() || previous_icon==previous_exe || '"'+previous_icon+'"'==previous_exe)
            set_string(*managed,"icon",icon);
        set_string(*managed,"Exe",exe); set_string(*managed,"StartDir",dir);
        set_string(*managed,"LaunchOptions",target.arguments);
        std::string output; write(roots,output);
        if (output.size()>limit) throw std::runtime_error("Updated shortcuts file exceeds the supported size.");
        plan={input,std::move(output),appid,created};
        return true;
    } catch (const std::exception& error) { message=error.what(); return false; }
}
