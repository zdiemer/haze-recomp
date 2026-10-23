#ifndef HAZE_JOB13_NATIVE_UPDATE_H
#define HAZE_JOB13_NATIVE_UPDATE_H

#include <stdbool.h>
#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Attempt the complete image-32 particle-loop operation at entry 09A00.
 * False means no context/LS/MXCSR mutation and the caller must execute the
 * original translated loop. True means LS object/particle outputs are
 * committed and the caller should continue at 09BC4. */
bool haze_job13_native_update_try(spu_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
