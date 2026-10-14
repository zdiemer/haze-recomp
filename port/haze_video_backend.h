#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    unsigned aa, anisotropy, frame_limit;
    float mip_bias, sharpening, exposure;
    int vsync; /* -1 auto, 0 off, 1 on */
    unsigned overlay, overlay_position; /* 0 off / 1 compact / 2 expanded; TL, TR, BL, BR */
} haze_video_live;
/* Render/window thread only. Prepare leaves all effective values unchanged.
 * Finish immediately, without another renderer call, after persistence succeeds
 * or fails. A failed prepare has nothing to finish. */
int haze_video_prepare(const haze_video_live* settings, char* error, unsigned capacity);
void haze_video_finish(int commit);
/* Stop presenting, bound the final queue wait and relinquish exclusive mode.
 * GPU allocations remain owned until process teardown, including on timeout. */
int haze_video_stop(unsigned timeout_ms);
/* Window/render thread only, outside a quality transaction. Requested tracks
 * focus-loss retry policy; active is the actual DXGI exclusive state. */
int haze_video_display_get(int* requested,int* active,char* error,unsigned capacity);
int haze_video_display_set(int requested,int active,char* error,unsigned capacity);
#ifdef __cplusplus
}
#endif
