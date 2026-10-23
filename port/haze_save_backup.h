#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Every native save is staged as a complete copy of its directory and swapped
// in only after the guest's callbacks succeed. The last committed saves are
// kept under <root>/.haze-saves/<dir>/backup.N, and a missing, truncated or
// unreadable save is replaced by the newest backup that verifies. Damage no
// backup can repair blocks commits until the guest rewrites those files.
void* haze_backup_begin(const char* directory);
const char* haze_backup_stage(void* transaction);
void haze_backup_metadata_written(void* transaction);
void haze_backup_secure_written(void* transaction,const char* name);
// A truncating write at offset 0 or a delete: the file no longer holds old bytes.
void haze_backup_file_replaced(void* transaction,const char* name);
int32_t haze_backup_finish(void* transaction,int32_t result);
void haze_backup_recover(const char* directory);
void haze_backup_recover_all(const char* root);
#ifdef __cplusplus
}
#endif
