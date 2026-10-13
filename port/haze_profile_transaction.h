#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Existing Audio profile only. Stage one complete payload; publish only after
// the guest's final save callback succeeds and metadata remains unchanged.
void* haze_profile_begin(const char* directory);
const char* haze_profile_metadata_directory(void* transaction);
int32_t haze_profile_stage(void* transaction,const void* bytes,uint32_t size);
int32_t haze_profile_finish(void* transaction,int32_t callback_result);
int haze_save_is_settings(void);
#ifdef __cplusplus
}
#endif
