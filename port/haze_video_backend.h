#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    unsigned aa, anisotropy, frame_limit;
    float mip_bias, sharpening, exposure;
    int vsync; /* -1 auto, 0 off, 1 on */
} haze_video_live;
/* Render/window thread only. Prepare leaves all effective values unchanged.
 * Finish immediately, without another renderer call, after persistence succeeds
 * or fails. A failed prepare has nothing to finish. */
int haze_video_prepare(const haze_video_live* settings, char* error, unsigned capacity);
void haze_video_finish(int commit);
#ifdef __cplusplus
}
#endif
