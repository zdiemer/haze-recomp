// Included after the pinned SDK save-data implementation. The patch changes
// only three definitions to *_original; all existing callers use these hooks.
// Every save runs against a staged copy (haze_save_backup.h); loads first
// repair or restore the live directory.
#include "haze_save_backup.h"
#ifdef __cplusplus
static thread_local void* haze_profile_pending;
static thread_local void* haze_backup_pending;
#else
static _Thread_local void* haze_profile_pending;
static _Thread_local void* haze_backup_pending;
#endif
static const char* haze_backup_file_name(const CellSaveDataFileSet* set) {
    switch (set->fileType) {
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON0: return "ICON0.PNG";
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON1: return "ICON1.PAM";
    case CELL_SAVEDATA_FILETYPE_CONTENT_PIC1: return "PIC1.PNG";
    case CELL_SAVEDATA_FILETYPE_CONTENT_SND0: return "SND0.AT3";
    default: return set->fileName;
    }
}
static s32 process_file_op(const char* path,CellSaveDataFileSet* set) {
    if (haze_profile_pending) {
        if (!set || set->fileOperation!=CELL_SAVEDATA_FILEOP_WRITE || set->fileOffset ||
            set->fileType!=CELL_SAVEDATA_FILETYPE_NORMALFILE || !set->fileName || strcmp(set->fileName,"_PROFILE.DAT") ||
            set->fileBufSize<set->fileSize) return CELL_SAVEDATA_ERROR_PARAM;
        return haze_profile_stage(haze_profile_pending,set->fileBuf,set->fileSize);
    }
    if (!haze_backup_pending) return process_file_op_original(path,set);
    s32 result=process_file_op_original(haze_backup_stage(haze_backup_pending),set);
    if (result>=0 && set->fileType==CELL_SAVEDATA_FILETYPE_SECUREFILE &&
        (set->fileOperation==CELL_SAVEDATA_FILEOP_WRITE || set->fileOperation==CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC))
        haze_backup_secure_written(haze_backup_pending,set->fileName);
    if (result>=0 && (set->fileOperation==CELL_SAVEDATA_FILEOP_DELETE ||
        (set->fileOperation==CELL_SAVEDATA_FILEOP_WRITE && !set->fileOffset)))
        haze_backup_file_replaced(haze_backup_pending,haze_backup_file_name(set));
    return result;
}
static void write_param_sfo(const char* path,const CellSaveDataSystemFileParam* param) {
    if (haze_profile_pending) { write_param_sfo_original(haze_profile_metadata_directory(haze_profile_pending),param); return; }
    if (!haze_backup_pending) { write_param_sfo_original(path,param); return; }
    write_param_sfo_original(haze_backup_stage(haze_backup_pending),param);
    if (param) haze_backup_metadata_written(haze_backup_pending);
}
static s32 savedata_execute(const char* directory,int saving,CellSaveDataSetBuf* buffers,
    CellSaveDataStatCallback stat,CellSaveDataFileCallback file,void* userdata) {
    if (!directory) return savedata_execute_original(directory,saving,buffers,stat,file,userdata);
    char path[1024];
    build_save_path(path,sizeof(path),directory);
    if (!saving) {
        haze_backup_recover(path);
        return savedata_execute_original(directory,saving,buffers,stat,file,userdata);
    }
    const int settings=haze_save_is_settings();
    // The settings path is audited for Haze's Audio Apply, not checkpoint/new-profile
    // creation. Refuse an unexpected callback rather than publishing half a save.
    if (haze_backup_pending || haze_profile_pending || (settings && ((uintptr_t)stat!=0xe71a58 || !file)))
        return CELL_SAVEDATA_ERROR_PARAM;
    void* backup=haze_backup_begin(path);
    if (!backup) return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
    void* profile=NULL;
    if (settings && !(profile=haze_profile_begin(haze_backup_stage(backup))))
        return haze_backup_finish(backup,CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    haze_backup_pending=backup; haze_profile_pending=profile;
    s32 result=savedata_execute_original(directory,saving,buffers,stat,file,userdata);
    haze_profile_pending=NULL; haze_backup_pending=NULL;
    if (profile && !(result=haze_profile_finish(profile,result))) haze_backup_file_replaced(backup,"_PROFILE.DAT");
    return haze_backup_finish(backup,result);
}
