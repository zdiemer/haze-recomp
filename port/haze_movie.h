#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int haze_movie_start(uint32_t player, const char* uri);
void haze_movie_set_root(const char* root);
void haze_movie_stop(uint32_t player);
int haze_movie_finished(uint32_t player);
void haze_movie_pause(uint32_t player, int paused);
void haze_movie_shutdown(void);
void haze_movie_render_mode(int on);
void haze_movie_render_frame(const uint8_t* rgba,uint32_t width,uint32_t height);
int haze_movie_present_tick(void);
#ifdef __cplusplus
}
#endif
