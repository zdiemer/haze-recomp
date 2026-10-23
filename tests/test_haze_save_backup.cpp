// Real Win32 files and the production staged-save layer. Every mutating file
// call can fail once (a transient disk-full or permission error) or stop all
// later calls (the process was terminated), including during recovery.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include "cellSaveData.h"
static int budget=-1,fault_at=-1,calls;
static bool full;
static bool allowed() {
    const int call=calls++;
    if (budget==0 || call==fault_at) { SetLastError(ERROR_ACCESS_DENIED); return false; }
    if (budget>0) --budget;
    return true;
}
static BOOL test_write(HANDLE h,LPCVOID b,DWORD n,LPDWORD wrote,LPOVERLAPPED o) {
    if (full) { SetLastError(ERROR_DISK_FULL); return FALSE; }
    return allowed() ? WriteFile(h,b,n,wrote,o) : FALSE;
}
static BOOL test_flush(HANDLE h) { return allowed() ? FlushFileBuffers(h) : FALSE; }
static BOOL test_move(LPCWSTR from,LPCWSTR to,DWORD flags) { return allowed() ? MoveFileExW(from,to,flags) : FALSE; }
static BOOL test_delete(LPCWSTR path) { return allowed() ? DeleteFileW(path) : FALSE; }
static BOOL test_rmdir(LPCWSTR path) { return allowed() ? RemoveDirectoryW(path) : FALSE; }
static BOOL test_mkdir(LPCWSTR path,LPSECURITY_ATTRIBUTES a) { return allowed() ? CreateDirectoryW(path,a) : FALSE; }
#define WriteFile test_write
#define FlushFileBuffers test_flush
#define MoveFileExW test_move
#define DeleteFileW test_delete
#define RemoveDirectoryW test_rmdir
#define CreateDirectoryW test_mkdir
#include "../port/haze_save_backup.cpp"
#undef WriteFile
#undef FlushFileBuffers
#undef MoveFileExW
#undef DeleteFileW
#undef RemoveDirectoryW
#undef CreateDirectoryW
namespace disk=std::filesystem;
using Files=std::map<std::string,std::string>;
static disk::path root,live,area;
static std::string path_text;
static void put(const disk::path& p,const std::string& data) {
    std::ofstream f(p,std::ios::binary|std::ios::trunc); f.write(data.data(),data.size()); f.close(); assert(f);
}
static std::string read(const disk::path& p) {
    std::ifstream f(p,std::ios::binary); assert(f); return {std::istreambuf_iterator<char>(f),{}};
}
static Files contents(const disk::path& directory) {
    Files out;
    for (const auto& file:disk::directory_iterator(directory)) {
        std::ifstream f(file.path(),std::ios::binary); assert(f);
        out[file.path().filename().string()]={std::istreambuf_iterator<char>(f),{}};
    }
    return out;
}
static void place(const disk::path& directory,const Files& files) {
    disk::remove_all(directory); disk::create_directories(directory);
    for (const auto& [name,data]:files) put(directory/name,data);
}
static Files version(int n) {
    // A multi-file save whose payload and metadata both change between versions.
    return {{"PARAM.SFO","TITLE=HAZE\nSUB_TITLE=Chapter "+std::to_string(n)+"\nDETAIL=\nATTRIBUTE=0\nLIST_PARAM=\n"},
            {"PARAM.PFD","DATA.BIN\n"},{"DATA.BIN",std::string(70000+n,char('a'+n))},{"ICON0.PNG",std::string(16,char(n))}};
}
// Guest writes as the hooks report them: whole-file rewrites and deletes.
static void update(void* tx,const Files& files) {
    const disk::path stage=haze_backup_stage(tx);
    for (const auto& [name,data]:files) { put(stage/name,data); haze_backup_file_replaced(tx,name.c_str()); }
}
static void write_stage(void* tx,const Files& files) {
    const disk::path stage=haze_backup_stage(tx);
    for (const auto& entry:disk::directory_iterator(stage)) if (!files.count(entry.path().filename().string())) {
        disk::remove(entry.path()); haze_backup_file_replaced(tx,entry.path().filename().string().c_str());
    }
    update(tx,files);
}
static int save(const Files& files,int result=0) {
    void* tx=haze_backup_begin(path_text.c_str());
    if (!tx) return -1;
    write_stage(tx,files);
    return haze_backup_finish(tx,result);
}
static void recover() { haze_backup_recover(path_text.c_str()); }
static bool exists(const char* name) { return disk::exists(area/name); }
static void tidy() {
    for (const char* name:{"stage","incoming","previous","next.manifest","lock","current.manifest.tmp","next.manifest.tmp"}) assert(!exists(name));
    assert(!exists("backup.4") && !exists("damaged.4"));
}
static void history(int newest) {
    // Begin from a committed save whose backups hold the last three versions.
    disk::remove_all(live); disk::remove_all(area); place(live,version(newest-2));
    assert(save(version(newest-1))==0 && save(version(newest))==0);
    assert(contents(live)==version(newest)); tidy();
}
static void fresh() {
    // history(14), copied from a template instead of committed again each time.
    static bool made;
    const auto saved=root/"template";
    if (!made) {
        history(14); disk::create_directory(saved);
        disk::copy(live,saved/"live",disk::copy_options::recursive); disk::copy(area,saved/"area",disk::copy_options::recursive); made=true;
    }
    disk::remove_all(live); disk::remove_all(area);
    disk::copy(saved/"live",live,disk::copy_options::recursive); disk::copy(saved/"area",area,disk::copy_options::recursive);
}
static void either(const Files& before,const Files& after,int result) {
    recover();
    const auto now=contents(live);
    assert(now==before || now==after);
    if (!result) assert(now==after); // A reported success is durable.
    tidy();
    assert(contents(area/"backup.1")!=Files{}); // The current save keeps a verified backup.
}
int main() {
    wchar_t temp[MAX_PATH]; assert(GetTempPathW(MAX_PATH,temp));
    root=disk::path(temp)/(L"haze-save-backup-test-"+std::to_wstring(GetCurrentProcessId()));
    live=root/"BLUS30010-SAVE"; area=root/".haze-saves/BLUS30010-SAVE"; path_text=live.string();
    assert(disk::create_directory(root));

    // A save from before backups existed is adopted and kept as backup.2.
    place(live,version(1)); assert(save(version(2))==0);
    assert(contents(live)==version(2)); tidy();
    auto with_manifest=[](Files files) { files.erase("HAZE.MANIFEST"); return files; };
    assert(with_manifest(contents(area/"backup.1"))==version(2) && with_manifest(contents(area/"backup.2"))==version(1));
    // An unchanged save neither commits nor rotates history.
    assert(save(version(2))==0 && !exists("backup.3"));
    // Retention stays bounded: the newest three committed saves.
    for (int n=3;n<=7;++n) assert(save(version(n))==0);
    assert(contents(live)==version(7)); tidy();
    for (int n=1;n<=3;++n) assert(with_manifest(contents(area/("backup."+std::to_string(n))))==version(8-n));

    // A guest or SDK write failure publishes nothing and keeps no staging.
    assert(save(version(8),CELL_SAVEDATA_ERROR_ACCESS_ERROR)==CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    assert(contents(live)==version(7)); tidy();
    // Disk full while committing.
    { void* tx=haze_backup_begin(path_text.c_str()); assert(tx); write_stage(tx,version(8)); full=true;
      assert(haze_backup_finish(tx,0)==haze_save_backup::access_error); full=false; assert(contents(live)==version(7)); tidy(); }
    // Disk full while staging refuses the save before the guest writes.
    full=true; assert(!haze_backup_begin(path_text.c_str())); full=false; assert(contents(live)==version(7)); tidy();
    // Incomplete metadata or secure-file bookkeeping is a failed save.
    { void* tx=haze_backup_begin(path_text.c_str()); assert(tx); auto files=version(8); files["PARAM.SFO"]="TITLE=HAZE\nSUB_TI";
      write_stage(tx,files); haze_backup_metadata_written(tx); assert(haze_backup_finish(tx,0)==haze_save_backup::access_error);
      assert(contents(live)==version(7)); }
    { void* tx=haze_backup_begin(path_text.c_str()); assert(tx); auto files=version(8); files["PARAM.SFO"]="TITLE=HAZE\nSUB_TITLE=Two\nlines\nDETAIL=\nATTRIBUTE=0\nLIST_PARAM=\n";
      write_stage(tx,files); haze_backup_metadata_written(tx); haze_backup_secure_written(tx,"DATA.BIN");
      haze_backup_secure_written(tx,"OTHER.BIN"); assert(haze_backup_finish(tx,0)==haze_save_backup::access_error); assert(contents(live)==version(7)); }
    // Another instance holding the save cannot write it, and loads stay read-only.
    { HANDLE other=CreateFileW((area/L"lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_DELETE_ON_CLOSE,nullptr);
      assert(other!=INVALID_HANDLE_VALUE); assert(save(version(8))==-1); disk::remove_all(live); recover();
      assert(!disk::exists(live)); assert(CloseHandle(other)); recover(); assert(contents(live)==version(7)); tidy(); }
    // An external edit during the save is kept rather than overwritten.
    { void* tx=haze_backup_begin(path_text.c_str()); assert(tx); write_stage(tx,version(8)); put(live/"DATA.BIN","edited");
      assert(haze_backup_finish(tx,0)==haze_save_backup::access_error); assert(contents(live).at("DATA.BIN")=="edited"); }

    // Truncated, corrupted and missing saves come back from the newest backup.
    history(9); { std::ofstream f(live/"DATA.BIN",std::ios::binary|std::ios::trunc); f << "short"; }
    recover(); assert(contents(live)==version(9)); assert(contents(area/"damaged.1").at("DATA.BIN")=="short"); tidy();
    put(live/"ICON0.PNG",std::string(16,'x')); recover(); assert(contents(live)==version(9)); tidy();
    disk::remove_all(live); recover(); assert(contents(live)==version(9)); tidy();
    disk::remove(live/"PARAM.SFO"); recover(); assert(contents(live)==version(9)); tidy();
    // A damaged newest backup falls through to the next one that verifies.
    put(area/"backup.1/DATA.BIN","bad"); put(live/"DATA.BIN","bad"); recover();
    assert(contents(live)==version(8)); tidy();
    // Saving after a restore keeps working, and damaged copies are bounded.
    for (int n=0;n<4;++n) { put(live/"DATA.BIN","bad"+std::to_string(n)); recover(); }
    assert(exists("damaged.3") && save(version(10))==0 && contents(live)==version(10)); tidy();
    // A manual restore (copying a backup's files back) is accepted as is.
    place(live,with_manifest(contents(area/"backup.3"))); recover(); assert(contents(live)==version(8) && !exists("damaged.4")); tidy();
    assert(save(version(11))==0 && contents(live)==version(11));
    // A brand-new save, and a failed one that leaves no empty directory.
    disk::remove_all(live); disk::remove_all(area);
    assert(save(version(1),CELL_SAVEDATA_ERROR_CBRESULT)==CELL_SAVEDATA_ERROR_CBRESULT && !disk::exists(live));
    { void* tx=haze_backup_begin(path_text.c_str()); assert(tx); disk::create_directory(live);
      assert(haze_backup_finish(tx,CELL_SAVEDATA_ERROR_ACCESS_ERROR)==CELL_SAVEDATA_ERROR_ACCESS_ERROR && !disk::exists(live)); }
    assert(save(version(1))==0 && contents(live)==version(1)); tidy();

    // Terminate the process at every mutating call of a commit, then again at
    // every call of the following recovery. The prior or the complete new save
    // survives; staging never leaks; reported success is never lost.
    int terminated=0;
    for (int stop=0;;++stop) {
        fresh(); const auto before=contents(live),after=version(15);
        void* tx=haze_backup_begin(path_text.c_str()); assert(tx); write_stage(tx,after);
        budget=stop; const int result=haze_backup_finish(tx,0); const bool finished=budget!=0; budget=-1;
        if (finished) { assert(!result); either(before,after,result); break; }
        // Copy the interrupted state so each recovery crash starts from it.
        const auto interrupted=root/"interrupted"; disk::remove_all(interrupted);
        disk::create_directory(interrupted);
        if (disk::exists(live)) disk::copy(live,interrupted/"live",disk::copy_options::recursive);
        disk::copy(area,interrupted/"area",disk::copy_options::recursive);
        for (int again=0;;++again) {
            disk::remove_all(live); disk::remove_all(area);
            if (disk::exists(interrupted/"live")) disk::copy(interrupted/"live",live,disk::copy_options::recursive);
            disk::copy(interrupted/"area",area,disk::copy_options::recursive);
            budget=again; recover(); const bool recovered=budget!=0; budget=-1;
            either(before,after,result); ++terminated;
            if (recovered) break;
        }
        disk::remove_all(interrupted);
    }
    // Terminate during begin: the existing save is untouched.
    for (int stop=0;;++stop) {
        fresh(); budget=stop; void* tx=haze_backup_begin(path_text.c_str()); const bool finished=budget!=0; budget=-1;
        if (tx) { write_stage(tx,version(15)); haze_backup_finish(tx,CELL_SAVEDATA_ERROR_CBRESULT); }
        recover(); assert(contents(live)==version(14)); tidy();
        if (finished) break;
    }
    // A single transient failure at any call while the process keeps running.
    int transient=0;
    for (int at=0;;++at) {
        fresh(); void* tx=haze_backup_begin(path_text.c_str()); assert(tx); write_stage(tx,version(15));
        calls=0; fault_at=at; const int result=haze_backup_finish(tx,0); const bool reached=calls>at; fault_at=-1;
        const auto now=contents(live);
        assert(now==(result ? version(14) : version(15))); // In-process rollback is immediate.
        either(version(14),version(15),result); ++transient;
        if (!reached) break;
    }
    // A contender refused the lock leaves the holder's populated stage alone,
    // as do load and boot recovery; the holder's partial update keeps the rest.
    { disk::remove_all(live); disk::remove_all(area);
      place(live,{{"PROFILE.DAT","profile 1"},{"CHECKPOINT.DAT",std::string(5000,'c')},{"PARAM.SFO","TITLE=HAZE\n"}});
      void* a=haze_backup_begin(path_text.c_str()); assert(a);
      const auto staged=contents(haze_backup_stage(a)); assert(staged==contents(live));
      assert(!haze_backup_begin(path_text.c_str()));
      recover(); haze_backup_recover_all(root.string().c_str());
      assert(contents(haze_backup_stage(a))==staged);
      update(a,{{"PROFILE.DAT","profile 2"}}); assert(haze_backup_finish(a,0)==0);
      auto expected=staged; expected["PROFILE.DAT"]="profile 2";
      assert(contents(live)==expected); recover(); assert(contents(live)==expected); tidy(); }
    // Damage that no backup repairs is never certified by a partial save, and
    // stays flagged until every damaged file is rewritten (or the manifest is
    // deleted by hand as an explicit decision).
    for (const bool remove_backups:{true,false}) {
        disk::remove_all(live); disk::remove_all(area);
        const Files good={{"PROFILE.DAT","profile 1"},{"CHECKPOINT.DAT",std::string(5000,'c')},{"PARAM.SFO","TITLE=HAZE\n"}};
        place(live,good); assert(save(good)==0); // Adopts and backs it up.
        { void* tx=haze_backup_begin(path_text.c_str()); assert(tx); update(tx,{{"PROFILE.DAT","profile 2"}}); assert(haze_backup_finish(tx,0)==0); }
        for (int n=1;n<=3;++n) {
            const auto backup=area/("backup."+std::to_string(n));
            if (!disk::exists(backup)) continue;
            if (remove_backups) disk::remove_all(backup); else put(backup/"CHECKPOINT.DAT","bad backup");
        }
        const auto manifest=read(area/"current.manifest");
        put(live/"CHECKPOINT.DAT","cut");
        recover(); assert(contents(live).at("CHECKPOINT.DAT")=="cut");
        void* tx=haze_backup_begin(path_text.c_str()); assert(tx);
        update(tx,{{"PROFILE.DAT","profile 3"}});
        assert(haze_backup_finish(tx,0)==haze_save_backup::access_error);
        assert(contents(live).at("PROFILE.DAT")=="profile 2" && contents(live).at("CHECKPOINT.DAT")=="cut");
        assert(read(area/"current.manifest")==manifest);
        // Still admitted, so a save that rewrites every damaged file repairs it.
        { void* again=haze_backup_begin(path_text.c_str()); assert(again);
          update(again,{{"PROFILE.DAT","profile 3"},{"CHECKPOINT.DAT",std::string(5000,'d')}});
          assert(haze_backup_finish(again,0)==0); }
        assert(contents(live).at("CHECKPOINT.DAT")==std::string(5000,'d')); recover(); tidy();
        assert(contents(area/"backup.1").at("CHECKPOINT.DAT")==std::string(5000,'d'));
    }
    // Deleting current.manifest by hand accepts the damaged save as it is.
    put(live/"CHECKPOINT.DAT","cut"); for (int n=1;n<=3;++n) disk::remove_all(area/("backup."+std::to_string(n)));
    disk::remove(area/"current.manifest");
    { void* tx=haze_backup_begin(path_text.c_str()); assert(tx); update(tx,{{"PROFILE.DAT","profile 4"}});
      assert(haze_backup_finish(tx,0)==0); assert(contents(live).at("CHECKPOINT.DAT")=="cut"); tidy(); }
    // Startup recovery finds interrupted commits for every save directory.
    fresh(); disk::rename(live,area/"previous"); haze_backup_recover_all(root.string().c_str());
    assert(contents(live)==version(14)); tidy();

    disk::remove_all(root);
    std::printf("Save backups: legacy adoption, bounded rotation, write/disk-full/metadata/secure-file failures, contention, "
        "external edits, truncated/corrupt/missing restore, manual restore, %d termination and %d transient-fault points passed.\n",
        terminated,transient);
}
