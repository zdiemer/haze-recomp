// Real Win32 files and production transaction/hooks. Fault seams affect only
// the staging write; the save callback seam exercises delayed publication.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdio>
#include "cellSaveData.h"
// The staged-save layer underneath uses real file APIs; faults target Audio.
#include "../port/haze_save_backup.cpp"
enum class Fault { None, ShortWrite, Flush, Close, Replace };
static Fault fault;
static bool fail_close;
static BOOL test_write(HANDLE h,LPCVOID b,DWORD n,LPDWORD wrote,LPOVERLAPPED o) {
    fail_close=fault==Fault::Close;
    return WriteFile(h,b,fault==Fault::ShortWrite ? n/2 : n,wrote,o);
}
static BOOL test_flush(HANDLE h) { return fault==Fault::Flush ? FALSE : FlushFileBuffers(h); }
static BOOL test_close(HANDLE h) {
    const auto result=CloseHandle(h);
    if (fail_close) { fail_close=false; return FALSE; }
    return result;
}
static BOOL test_replace(LPCWSTR from,LPCWSTR to,DWORD flags) {
    return fault==Fault::Replace ? FALSE : MoveFileExW(from,to,flags);
}
#define WriteFile test_write
#define FlushFileBuffers test_flush
#define CloseHandle test_close
#define MoveFileExW test_replace
#include "../port/haze_profile_transaction.cpp"
#undef WriteFile
#undef FlushFileBuffers
#undef CloseHandle
#undef MoveFileExW
namespace disk=std::filesystem;
static disk::path fixture;
static const std::string old_bytes(1024*1024,'A'),new_bytes(1024*1024,'B');
static const std::string metadata="TITLE=HAZE\r\nSUB_TITLE=Profile \r\nDETAIL=\r\nATTRIBUTE=0\r\nLIST_PARAM=\r\n";
static void put(const disk::path& p,const std::string& data) {
    std::ofstream f(p,std::ios::binary|std::ios::trunc); f.write(data.data(),data.size()); f.close(); assert(f);
}
static std::string get(const disk::path& p) {
    std::ifstream f(p,std::ios::binary); assert(f); return {std::istreambuf_iterator<char>(f),{}};
}
static bool settings=true,extra_write,change_metadata,skip_metadata,external_change;
static int callback_result,legacy_calls;
extern "C" int haze_save_is_settings(void) { return settings; }
static s32 process_file_op(const char*,CellSaveDataFileSet*);
static void write_param_sfo(const char*,const CellSaveDataSystemFileParam*);
static void build_save_path(char* out,size_t count,const char* name) {
    const auto path=(fixture/name).string(); assert(path.size()<count); strcpy_s(out,count,path.c_str());
}
static s32 process_file_op_original(const char*,CellSaveDataFileSet*) { ++legacy_calls; return 123; }
static void write_param_sfo_original(const char* path,const CellSaveDataSystemFileParam*) {
    if (!skip_metadata) put(disk::path(path)/"PARAM.SFO",change_metadata ? "changed" : metadata);
}
static s32 savedata_execute_original(const char* directory,int saving,CellSaveDataSetBuf*,
    CellSaveDataStatCallback,CellSaveDataFileCallback,void*) {
    if (!saving || !settings) { ++legacy_calls; return 456; }
    char path[1024]; build_save_path(path,sizeof(path),directory);
    CellSaveDataSystemFileParam param{}; write_param_sfo(path,&param);
    CellSaveDataFileSet set{}; char name[]="_PROFILE.DAT";
    set.fileName=name; set.fileOperation=CELL_SAVEDATA_FILEOP_WRITE;
    set.fileType=CELL_SAVEDATA_FILETYPE_NORMALFILE;
    set.fileSize=set.fileBufSize=(u32)new_bytes.size(); set.fileBuf=(void*)new_bytes.data();
    auto result=process_file_op(path,&set);
    // The real SDK issues its final callback after this write returns.
    assert(get(disk::path(path)/"_PROFILE.DAT")==old_bytes);
    assert(get(disk::path(path)/"PARAM.SFO")==metadata);
    if (result<0) return result;
    assert(result==(s32)new_bytes.size());
    if (extra_write) return process_file_op(path,&set);
    if (external_change) put(disk::path(path)/"_PROFILE.DAT","external edit");
    return callback_result;
}
#include "../port/haze_savedata_hooks.h"
static int run() {
    return savedata_execute("profile",1,nullptr,(CellSaveDataStatCallback)uintptr_t(0xe71a58),
        (CellSaveDataFileCallback)uintptr_t(0xe71a60),nullptr);
}
static void reset() {
    fault=Fault::None; callback_result=0; settings=true;
    extra_write=change_metadata=skip_metadata=external_change=false;
    disk::remove_all(fixture/".haze-saves");
    put(fixture/"profile/_PROFILE.DAT",old_bytes); put(fixture/"profile/PARAM.SFO",metadata);
}
static void clean() {
    assert(!haze_profile_pending && !haze_backup_pending);
    for (const auto& file:disk::directory_iterator(fixture))
        assert(file.path().filename()=="profile" || file.path().filename()==".haze-saves");
    if (disk::exists(fixture/".haze-saves")) for (const auto& file:disk::directory_iterator(fixture/".haze-saves/profile")) {
        const auto name=file.path().filename().wstring();
        assert(name==L"current.manifest" || name.rfind(L"backup.",0)==0);
    }
}
static void unchanged() {
    assert(get(fixture/"profile/_PROFILE.DAT")==old_bytes);
    assert(get(fixture/"profile/PARAM.SFO")==metadata); clean();
}
int main() {
    wchar_t temp[MAX_PATH]; assert(GetTempPathW(MAX_PATH,temp));
    fixture=disk::path(temp)/(L"haze-profile-test-"+std::to_wstring(GetCurrentProcessId()));
    assert(disk::create_directory(fixture)); assert(disk::create_directory(fixture/"profile"));
    for (const auto error:{Fault::ShortWrite,Fault::Flush,Fault::Close,Fault::Replace}) {
        reset(); fault=error; assert(run()==CELL_SAVEDATA_ERROR_ACCESS_ERROR); unchanged();
    }
    reset(); callback_result=CELL_SAVEDATA_ERROR_CBRESULT; assert(run()==callback_result); unchanged();
    reset(); extra_write=true; assert(run()==CELL_SAVEDATA_ERROR_ACCESS_ERROR); unchanged();
    reset(); change_metadata=true; assert(run()==CELL_SAVEDATA_ERROR_ACCESS_ERROR); unchanged();
    reset(); skip_metadata=true; assert(run()==CELL_SAVEDATA_ERROR_ACCESS_ERROR); unchanged();
    reset(); external_change=true; assert(run()==CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    assert(get(fixture/"profile/_PROFILE.DAT")=="external edit"); clean();
    reset(); {
        HANDLE locked=CreateFileW((fixture/"profile/_PROFILE.DAT").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
        assert(locked!=INVALID_HANDLE_VALUE); assert(run()==CELL_SAVEDATA_ERROR_ACCESS_ERROR);
        assert(CloseHandle(locked)); unchanged();
    }
    reset(); {
        auto path=(fixture/"profile").string(); void* first=haze_profile_begin(path.c_str()); assert(first);
        assert(!haze_profile_begin(path.c_str())); assert(haze_profile_finish(first,-99)==-99); unchanged();
    }
    reset(); assert(run()==0); assert(get(fixture/"profile/_PROFILE.DAT")==new_bytes);
    assert(get(fixture/"profile/PARAM.SFO")==metadata); clean();
    // The committed Audio profile is also the newest backup; the old one is next.
    assert(get(fixture/".haze-saves/profile/backup.1/_PROFILE.DAT")==new_bytes);
    assert(get(fixture/".haze-saves/profile/backup.2/_PROFILE.DAT")==old_bytes);
    reset(); settings=false; assert(run()==456 && legacy_calls==1); unchanged();
    settings=true; assert(savedata_execute("profile",0,nullptr,nullptr,nullptr,nullptr)==456 && legacy_calls==2);
    assert(savedata_execute("profile",1,nullptr,nullptr,nullptr,nullptr)==CELL_SAVEDATA_ERROR_PARAM);
    assert(process_file_op("unused",nullptr)==123 && legacy_calls==3); unchanged();
    // Remove only the exact fixture files and directories this test created.
    assert(disk::remove(fixture/"profile/_PROFILE.DAT")); assert(disk::remove(fixture/"profile/PARAM.SFO"));
    assert(disk::remove(fixture/"profile")); disk::remove_all(fixture/".haze-saves"); assert(disk::remove(fixture));
    puts("Audio profile transaction: real-file short write, flush, close, replace, callback, metadata, contention, external edit, commit and legacy routing passed.");
}
