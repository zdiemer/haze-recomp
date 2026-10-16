#include "haze_save_backup.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Layout beside the save root (PS3_SAVEDATA_ROOT), hidden from the guest's
// directory list: .haze-saves/<dir>/{lock,stage,previous,current.manifest,
// next.manifest,backup.1..3,damaged.1..3}. The save directory itself keeps the
// SDK format. A commit moves the live directory aside, publishes the staged
// copy and only then retires the old one; recovery finishes or reverses it.
namespace haze_save_backup {
namespace fs=std::filesystem;
constexpr int32_t access_error=int32_t(0x8002b402u);
constexpr int keep=3;
constexpr char header[]="haze-save-manifest 1\n";
constexpr wchar_t manifest_name[]=L"HAZE.MANIFEST";
using Manifest=std::map<std::string,std::pair<uint64_t,uint64_t>>;
struct Paths {
    fs::path live,area,stage,incoming,previous,current,next;
    fs::path slot(const wchar_t* kind,int index) const { return area/(kind+std::to_wstring(index)); }
};
struct Guard {
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~Guard() { if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
struct Transaction {
    Paths p;
    Guard guard;
    Manifest before;
    std::string stage;
    std::set<std::string> damaged; // Known-bad files the guest must fully rewrite.
    bool owner=false,existed=false,failed=false;
    ~Transaction();
};
std::string utf8(const fs::path& path) { const auto text=path.u8string(); return {text.begin(),text.end()}; }
Paths paths(const fs::path& live) {
    Paths p; p.live=live; p.area=live.parent_path()/L".haze-saves"/live.filename();
    p.stage=p.area/L"stage"; p.incoming=p.area/L"incoming"; p.previous=p.area/L"previous";
    p.current=p.area/L"current.manifest"; p.next=p.area/L"next.manifest"; return p;
}
bool note(const Paths& p,const char* format,...) {
    char text[256]; va_list args; va_start(args,format); std::vsnprintf(text,sizeof text,format,args); va_end(args);
    std::fprintf(stderr,"[save-backup] %s: %s\n",utf8(p.live.filename()).c_str(),text); return false;
}
int32_t fail(const Paths& p,const char* reason) {
    note(p,"%s The previous save is retained.",reason); return access_error;
}
DWORD attributes(const fs::path& path) { return GetFileAttributesW(path.c_str()); }
bool present(const fs::path& path) { return attributes(path)!=INVALID_FILE_ATTRIBUTES; }
bool folder(const fs::path& path) {
    const auto a=attributes(path);
    return a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY) && !(a&FILE_ATTRIBUTE_REPARSE_POINT);
}
bool relocate(const fs::path& from,const fs::path& to,bool replace=false) {
    return MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH|(replace ? MOVEFILE_REPLACE_EXISTING : 0))!=0;
}
bool unlink(const fs::path& path) {
    if (!present(path)) return true;
    SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path.c_str())!=0;
}
bool files(const fs::path& directory,std::vector<fs::path>& out,bool backup) {
    std::error_code error;
    for (fs::directory_iterator it(directory,error),end;!error && it!=end;it.increment(error)) {
        const auto a=attributes(it->path());
        if (a==INVALID_FILE_ATTRIBUTES) return false;
        if (!(a&FILE_ATTRIBUTE_DIRECTORY) && !(backup && it->path().filename()==manifest_name)) out.push_back(it->path());
    }
    return !error;
}
bool erase(const fs::path& path) {
    const auto a=attributes(path);
    if (a==INVALID_FILE_ATTRIBUTES) return true;
    if (!(a&FILE_ATTRIBUTE_DIRECTORY)) return unlink(path);
    bool ok=true;
    if (!(a&FILE_ATTRIBUTE_REPARSE_POINT)) {
        std::vector<fs::path> children; std::error_code error;
        for (fs::directory_iterator it(path,error),end;!error && it!=end;it.increment(error)) children.push_back(it->path());
        ok=!error;
        for (const auto& child:children) ok=erase(child) && ok;
    }
    return ok && RemoveDirectoryW(path.c_str());
}
bool slurp(const fs::path& path,std::string& bytes) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; DWORD got=0;
    bool ok=GetFileSizeEx(file,&size) && size.QuadPart>=0 && size.QuadPart<=(1<<20);
    if (ok) { bytes.resize(size_t(size.QuadPart)); ok=bytes.empty() || (ReadFile(file,bytes.data(),DWORD(bytes.size()),&got,nullptr) && got==bytes.size()); }
    CloseHandle(file); return ok;
}
bool digest(const fs::path& path,uint64_t& size,uint64_t& hash) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    static thread_local unsigned char buffer[1<<16];
    DWORD got=0; bool ok; LARGE_INTEGER expected{};
    size=0; hash=1469598103934665603ull;
    while ((ok=ReadFile(file,buffer,sizeof buffer,&got,nullptr)!=0) && got) {
        for (DWORD i=0;i<got;++i) hash=(hash^buffer[i])*1099511628211ull;
        size+=got;
    }
    ok=ok && GetFileSizeEx(file,&expected) && uint64_t(expected.QuadPart)==size;
    CloseHandle(file); return ok;
}
bool write_all(HANDLE file,const void* bytes,size_t size) {
    auto data=static_cast<const char*>(bytes);
    while (size) {
        DWORD wrote=0;
        if (!WriteFile(file,data,DWORD(std::min<size_t>(size,1u<<20)),&wrote,nullptr) || !wrote) return false;
        data+=wrote; size-=wrote;
    }
    return true;
}
bool create(const fs::path& path,const std::string& bytes) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    bool ok=write_all(file,bytes.data(),bytes.size()) && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok=false;
    if (!ok) unlink(path);
    return ok;
}
bool replace(const fs::path& path,const std::string& bytes) {
    auto temporary=path; temporary+=L".tmp";
    if (!unlink(temporary) || !create(temporary,bytes)) return false;
    if (relocate(temporary,path,true)) return true;
    unlink(temporary); return false;
}
bool duplicate(const fs::path& from,const fs::path& to) {
    HANDLE in=CreateFileW(from.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if (in==INVALID_HANDLE_VALUE) return false;
    HANDLE out=CreateFileW(to.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    static thread_local unsigned char buffer[1<<16];
    DWORD got=0; bool ok=out!=INVALID_HANDLE_VALUE;
    while (ok && (ok=ReadFile(in,buffer,sizeof buffer,&got,nullptr)!=0) && got) ok=write_all(out,buffer,got);
    if (out!=INVALID_HANDLE_VALUE) {
        ok=ok && FlushFileBuffers(out);
        if (!CloseHandle(out)) ok=false;
        if (!ok) unlink(to);
    }
    CloseHandle(in); return ok;
}
bool flush(const fs::path& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    bool ok=FlushFileBuffers(file)!=0;
    if (!CloseHandle(file)) ok=false;
    return ok;
}
bool scan(const fs::path& directory,Manifest& manifest,bool backup=false) {
    std::vector<fs::path> list; manifest.clear();
    if (!folder(directory) || !files(directory,list,backup)) return false;
    for (const auto& file:list) {
        uint64_t size,hash;
        if (!digest(file,size,hash)) return false;
        manifest[utf8(file.filename())]={size,hash};
    }
    return true;
}
bool matches(const fs::path& directory,const Manifest& expected,bool backup=false) {
    Manifest actual; return scan(directory,actual,backup) && actual==expected;
}
std::string text(const Manifest& manifest) {
    std::string out=header; char line[48];
    for (const auto& [name,value]:manifest) {
        std::snprintf(line,sizeof line,"%llu %016llx ",(unsigned long long)value.first,(unsigned long long)value.second);
        out+=line; out+=name; out+='\n';
    }
    return out;
}
bool load(const fs::path& path,Manifest& manifest) {
    std::string bytes; manifest.clear();
    if (!slurp(path,bytes) || bytes.compare(0,sizeof header-1,header)) return false;
    for (size_t at=sizeof header-1;at<bytes.size();) {
        const auto end=bytes.find('\n',at);
        if (end==std::string::npos) return false;
        const auto line=bytes.substr(at,end-at); char* cursor=nullptr;
        const auto size=std::strtoull(line.c_str(),&cursor,10);
        if (*cursor!=' ') return false;
        const auto hash=std::strtoull(cursor+1,&cursor,16);
        if (*cursor!=' ' || !cursor[1]) return false;
        manifest[cursor+1]={size,hash}; at=end+1;
    }
    return true;
}
bool backup(const Paths& p,int index,Manifest& manifest) {
    const auto directory=p.slot(L"backup.",index);
    return folder(directory) && load(directory/manifest_name,manifest) && matches(directory,manifest,true);
}
bool rotate(const Paths& p,const wchar_t* kind) {
    if (!erase(p.slot(kind,keep))) return false;
    for (int index=keep-1;index>0;--index)
        if (present(p.slot(kind,index)) && !relocate(p.slot(kind,index),p.slot(kind,index+1))) return false;
    return true;
}
bool populate(const fs::path& from,const fs::path& to,const Manifest& expected,bool backup) {
    std::vector<fs::path> list;
    if (!erase(to) || !CreateDirectoryW(to.c_str(),nullptr) || !files(from,list,backup)) return false;
    for (const auto& file:list) if (!duplicate(file,to/file.filename())) return false;
    return matches(to,expected);
}
bool snapshot(const Paths& p,const Manifest& manifest) {
    if (populate(p.live,p.incoming,manifest,false) && create(p.incoming/manifest_name,text(manifest)) &&
        rotate(p,L"backup.") && relocate(p.incoming,p.slot(L"backup.",1))) return true;
    erase(p.incoming); return note(p,"Could not record a backup of the current save.");
}
// Keep a verified copy of the current committed save as backup.1.
bool protect(const Paths& p) {
    Manifest live,current,candidate;
    if (!folder(p.live)) return true;
    if (!scan(p.live,live)) return note(p,"Cannot read the save to back it up.");
    if (live.empty()) return true;
    const bool known=load(p.current,current);
    if (known && current!=live) return false; // Damage without a valid backup was already reported.
    if (!known && !replace(p.current,text(live))) return note(p,"Cannot record the save manifest.");
    for (int index=1;index<=keep;++index) if (backup(p,index,candidate) && candidate==live) return true;
    return snapshot(p,live);
}
bool restore(const Paths& p,const Manifest& current,bool damaged) {
    Manifest manifest; int chosen=0;
    for (int pass=0;pass<2 && !chosen;++pass)
        for (int index=1;index<=keep && !chosen;++index)
            if (backup(p,index,manifest) && (pass || manifest==current)) chosen=index;
    if (!chosen) {
        note(p,damaged ? "No backup verifies; the damaged save is left in place." : "No backup verifies; continuing without this save.");
        return true;
    }
    if (!populate(p.slot(L"backup.",chosen),p.stage,manifest,true)) { erase(p.stage); return note(p,"Could not copy backup.%d.",chosen); }
    if (damaged && (!rotate(p,L"damaged.") || !relocate(p.live,p.slot(L"damaged.",1)))) {
        erase(p.stage); return note(p,"Could not set the damaged save aside; it is left in place.");
    }
    if (!relocate(p.stage,p.live)) {
        if (damaged) relocate(p.slot(L"damaged.",1),p.live);
        erase(p.stage); return note(p,"Could not restore backup.%d.",chosen);
    }
    if (!replace(p.current,text(manifest))) note(p,"Restored backup.%d but could not record it.",chosen);
    note(p,damaged ? "Restored backup.%d; the damaged copy is in .haze-saves damaged.1." : "Restored missing save from backup.%d.",chosen);
    return true;
}
// Finish or reverse an interrupted commit, then verify the live directory.
// False means the save could not be read or repaired and must not be staged.
bool recover(const Paths& p) {
    Manifest next,current,live,candidate;
    bool pending=load(p.next,next),known=load(p.current,current);
    if (!folder(p.live) && pending && matches(p.stage,next)) {
        if (!relocate(p.stage,p.live)) return note(p,"Cannot complete an interrupted save.");
        note(p,"Completed an interrupted save.");
    }
    if (!folder(p.live) && folder(p.previous)) {
        if (!relocate(p.previous,p.live)) return note(p,"Cannot roll back an interrupted save; it is kept in .haze-saves.");
        note(p,"Rolled back an interrupted save.");
    }
    if (pending) {
        if (folder(p.live) && matches(p.live,next)) {
            if (!relocate(p.next,p.current,true)) return note(p,"Cannot record a completed save.");
            current=next; known=true;
        } else if (!unlink(p.next)) return note(p,"Cannot discard an interrupted save.");
    }
    if (!erase(p.stage) || !erase(p.incoming) || !erase(p.previous) ||
        !unlink(p.current.wstring()+L".tmp") || !unlink(p.next.wstring()+L".tmp"))
        note(p,"Could not remove leftover staging.");
    if (!folder(p.live)) {
        if (!known) return true;
        note(p,"The save directory is missing.");
        return restore(p,current,false);
    }
    if (!scan(p.live,live)) {
        if (!known) return note(p,"Cannot read the save.");
        note(p,"The save cannot be read.");
        return restore(p,current,true);
    }
    if (known && live==current) return true;
    for (int index=1;index<=keep;++index) if (backup(p,index,candidate) && candidate==live) {
        if (!replace(p.current,text(live))) return note(p,"Cannot record the save manifest.");
        if (known) note(p,"Accepted a save restored from backup.%d.",index);
        return true;
    }
    if (!known) return true; // A save written before backups existed; protect() adopts it.
    note(p,"The save does not match its last commit (truncated or modified).");
    return restore(p,current,true);
}
bool lock(const Paths& p,Guard& guard) {
    CreateDirectoryW(p.area.parent_path().c_str(),nullptr); CreateDirectoryW(p.area.c_str(),nullptr);
    guard.handle=CreateFileW((p.area/L"lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
    return guard.handle!=INVALID_HANDLE_VALUE;
}
// Only the lock holder owns the stage; a refused contender must not touch it.
Transaction::~Transaction() { if (owner) erase(p.stage); }
std::string names(const std::set<std::string>& files) {
    std::string out;
    for (const auto& name:files) out+=(out.empty() ? "" : ", ")+name;
    return out;
}
// C-linkage definitions here are the header's global functions.
extern "C" void* haze_backup_begin(const char* directory) {
    try {
        if (!directory || !*directory) return nullptr;
        auto tx=std::make_unique<Transaction>(); tx->p=paths(fs::path(directory));
        const auto& p=tx->p;
        if (!lock(p,tx->guard)) { fail(p,"Another instance is using this save; save refused."); return nullptr; }
        tx->owner=true;
        if (!recover(p)) { fail(p,"Save refused."); return nullptr; }
        protect(p);
        tx->existed=folder(p.live);
        // Recovery restores whatever a backup can. Anything still differing from
        // the last commit is known damage that a commit must never certify.
        Manifest current,live;
        if (load(p.current,current)) {
            if (tx->existed && !scan(p.live,live)) { fail(p,"Cannot read the save."); return nullptr; }
            for (const auto& [name,value]:current) {
                const auto found=live.find(name);
                if (found==live.end() || found->second!=value) tx->damaged.insert(name);
            }
            if (!tx->damaged.empty())
                note(p,"Damaged without a verified backup (%s); only a save that rewrites these files is accepted. See docs/native-saves.md.",names(tx->damaged).c_str());
        }
        if (tx->existed ? !scan(p.live,tx->before) || !populate(p.live,p.stage,tx->before,false)
                        : !erase(p.stage) || !CreateDirectoryW(p.stage.c_str(),nullptr)) {
            fail(p,"Cannot stage the save."); return nullptr;
        }
        tx->stage=p.stage.string(); return tx.release();
    } catch (...) { std::fprintf(stderr,"[save-backup] Cannot prepare save storage.\n"); return nullptr; }
}
extern "C" const char* haze_backup_stage(void* transaction) {
    return static_cast<Transaction*>(transaction)->stage.c_str();
}
// The SDK reports neither metadata nor secure-file bookkeeping failures.
extern "C" void haze_backup_metadata_written(void* transaction) {
    auto& tx=*static_cast<Transaction*>(transaction); std::string bytes; size_t at=0;
    try {
        bool ok=slurp(tx.p.stage/L"PARAM.SFO",bytes) && !bytes.compare(0,6,"TITLE=") && bytes.back()=='\n';
        for (const char* key:{"\nSUB_TITLE=","\nDETAIL=","\nATTRIBUTE=","\nLIST_PARAM="})
            ok=ok && (at=bytes.find(key,at))!=std::string::npos;
        if (ok) { tx.damaged.erase("PARAM.SFO"); return; }
    } catch (...) {}
    tx.failed=true; note(tx.p,"Save metadata was not written completely.");
}
extern "C" void haze_backup_secure_written(void* transaction,const char* name) {
    auto& tx=*static_cast<Transaction*>(transaction); std::string bytes;
    try {
        if (name && slurp(tx.p.stage/L"PARAM.PFD",bytes))
            for (size_t at=0,end;at<bytes.size();at=end+1) {
                end=bytes.find('\n',at); if (end==std::string::npos) end=bytes.size();
                auto line=bytes.substr(at,end-at);
                if (!line.empty() && line.back()=='\r') line.pop_back();
                if (line==name) return;
            }
    } catch (...) {}
    tx.failed=true; note(tx.p,"Secure-file bookkeeping was not written.");
}
extern "C" void haze_backup_file_replaced(void* transaction,const char* name) {
    if (name) static_cast<Transaction*>(transaction)->damaged.erase(name);
}
extern "C" int32_t haze_backup_finish(void* transaction,int32_t result) {
    std::unique_ptr<Transaction> tx(static_cast<Transaction*>(transaction));
    const auto& p=tx->p;
    try {
        if (!result && tx->failed) result=access_error;
        if (!result && !tx->damaged.empty()) {
            note(p,"Refused: this save does not rewrite the damaged %s, which has no verified backup.",names(tx->damaged).c_str());
            result=access_error;
        }
        if (result) {
            if (!tx->existed) RemoveDirectoryW(p.live.c_str()); // Only the empty directory the SDK made.
            note(p,"Save not completed (0x%08x); the previous save is retained.",unsigned(result));
            return result;
        }
        std::vector<fs::path> list; Manifest after;
        if (!files(p.stage,list,false)) return fail(p,"Cannot read the staged save.");
        for (const auto& file:list) if (!flush(file)) return fail(p,"Cannot flush the staged save.");
        if (!scan(p.stage,after)) return fail(p,"Cannot verify the staged save.");
        if (after==tx->before) {
            if (!tx->existed) RemoveDirectoryW(p.live.c_str());
            return 0;
        }
        if (tx->existed ? !matches(p.live,tx->before) : folder(p.live) && !RemoveDirectoryW(p.live.c_str()))
            return fail(p,"The save changed or became unavailable during saving.");
        if (!replace(p.next,text(after))) return fail(p,"Cannot record the new save.");
        if (tx->existed && !relocate(p.live,p.previous)) { unlink(p.next); return fail(p,"Cannot replace the save."); }
        if (!relocate(p.stage,p.live)) {
            unlink(p.next);
            if (tx->existed && !relocate(p.previous,p.live)) note(p,"The previous save is in .haze-saves previous and is reinstated on next access.");
            return fail(p,"Cannot publish the new save.");
        }
        if (!relocate(p.next,p.current,true)) note(p,"Saved; the commit record is completed on next access.");
        if (!erase(p.previous)) note(p,"Could not remove the replaced save.");
        protect(p);
        note(p,"Committed %u files.",unsigned(after.size()));
        return 0;
    } catch (...) { return fail(p,"Could not commit the save."); }
}
extern "C" void haze_backup_recover(const char* directory) {
    try {
        if (!directory || !*directory) return;
        const auto p=paths(fs::path(directory));
        if (!folder(p.area) && !folder(p.live)) return;
        Guard guard;
        if (!lock(p,guard)) { note(p,"Another instance is using this save; loading without recovery."); return; }
        if (recover(p)) protect(p);
    } catch (...) { std::fprintf(stderr,"[save-backup] Save recovery failed.\n"); }
}
extern "C" void haze_backup_recover_all(const char* root) {
    try {
        if (!root || !*root) return;
        const fs::path base(root); std::vector<std::string> names; std::error_code error;
        for (fs::directory_iterator it(base/L".haze-saves",error),end;!error && it!=end;it.increment(error))
            if (folder(it->path())) names.push_back((base/it->path().filename()).string());
        for (const auto& name:names) haze_backup_recover(name.c_str());
    } catch (...) { std::fprintf(stderr,"[save-backup] Save recovery failed.\n"); }
}
}
