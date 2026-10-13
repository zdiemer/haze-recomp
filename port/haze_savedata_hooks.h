// Included after the pinned SDK save-data implementation. The patch changes
// only three definitions to *_original; all existing callers use these hooks.
#ifdef __cplusplus
static thread_local void* haze_profile_pending;
#else
static _Thread_local void* haze_profile_pending;
#endif
static s32 process_file_op(const char* path,CellSaveDataFileSet* set) {
    if (!haze_profile_pending) return process_file_op_original(path,set);
    if (!set || set->fileOperation!=CELL_SAVEDATA_FILEOP_WRITE || set->fileOffset ||
        set->fileType!=CELL_SAVEDATA_FILETYPE_NORMALFILE || !set->fileName || strcmp(set->fileName,"_PROFILE.DAT") ||
        set->fileBufSize<set->fileSize) return CELL_SAVEDATA_ERROR_PARAM;
    return haze_profile_stage(haze_profile_pending,set->fileBuf,set->fileSize);
}
static void write_param_sfo(const char* path,const CellSaveDataSystemFileParam* param) {
    write_param_sfo_original(haze_profile_pending ? haze_profile_metadata_directory(haze_profile_pending) : path,param);
}
static s32 savedata_execute(const char* directory,int saving,CellSaveDataSetBuf* buffers,
    CellSaveDataStatCallback stat,CellSaveDataFileCallback file,void* userdata) {
    if (!saving || !haze_save_is_settings())
        return savedata_execute_original(directory,saving,buffers,stat,file,userdata);
    // This path is audited for Haze's Audio Apply, not checkpoint/new-profile
    // creation. Refuse an unexpected callback rather than publishing half a save.
    if (!directory || (uintptr_t)stat!=0xe71a58 || !file || haze_profile_pending)
        return CELL_SAVEDATA_ERROR_PARAM;
    char path[1024];
    build_save_path(path,sizeof(path),directory);
    void* transaction=haze_profile_begin(path);
    if (!transaction) return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
    haze_profile_pending=transaction;
    s32 result=savedata_execute_original(directory,saving,buffers,stat,file,userdata);
    haze_profile_pending=NULL;
    return haze_profile_finish(transaction,result);
}
