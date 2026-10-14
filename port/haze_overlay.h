#pragma once
#include <stdint.h>
#include <stddef.h>

/* Same recent window as rsx_live_draw_get_present_fps. Called at 4 Hz using
 * renderer-owned successful-present history, never a guessed simulation tick. */
typedef struct { uint64_t id; int64_t ticks; uint32_t guest; } haze_overlay_sample;
typedef haze_overlay_sample (*haze_overlay_read)(uint64_t id,void* context);
typedef struct { double fps, interval_ms, guest_flips; int ready; } haze_overlay_stats;
static haze_overlay_stats haze_overlay_measure(uint64_t total,unsigned capacity,int64_t frequency,
    haze_overlay_read read,void* context) {
    haze_overlay_stats out={0};
    if (total<2 || frequency<=0) return out;
    const haze_overlay_sample newest=read(total,context);
    if (newest.id!=total) return out;
    haze_overlay_sample oldest=newest;
    uint64_t intervals=0,available=total<capacity ? total : capacity;
    for (uint64_t back=1;back<available;++back) {
        const haze_overlay_sample candidate=read(total-back,context);
        if (candidate.id!=total-back) break;
        oldest=candidate; intervals=back;
        const double elapsed=(double)(newest.ticks-oldest.ticks)/(double)frequency;
        if (elapsed>=2.0 || (elapsed>=1.0 && intervals>=10)) break;
    }
    const int64_t ticks=newest.ticks-oldest.ticks;
    if (!intervals || ticks<=0) return out;
    const double seconds=(double)ticks/(double)frequency;
    out.fps=(double)intervals/seconds;
    out.interval_ms=1000.0*seconds/(double)intervals;
    out.guest_flips=(double)(uint32_t)(newest.guest-oldest.guest)/seconds;
    out.ready=1;
    return out;
}

typedef struct { float x,y,width,height,pixel; } haze_overlay_rect;
static haze_overlay_rect haze_overlay_layout(unsigned width,unsigned height,unsigned display_height,
    unsigned mode,unsigned corner,unsigned columns) {
    haze_overlay_rect r={0};
    if (!width || !height) return r;
    /* Compact targets a Steam-style 10.5-pixel readout in the final window;
     * expanded targets 14 pixels. Keep at least one render pixel per font dot
     * when a low-resolution backbuffer is stretched onto a large display. */
    float pixel=(mode==2 ? 2.0f : 1.5f)*(float)height/(display_height ? display_height : height);
    if (pixel<1) pixel=1;
    float fit=(float)width/268.0f;
    if (fit>(float)height/82.0f) fit=(float)height/82.0f;
    if (pixel>fit) pixel=fit;
    if (columns>40) columns=40;
    r.pixel=pixel; r.width=(mode==2 ? 252 : columns*6+3)*pixel; r.height=(mode==2 ? 66 : 10)*pixel;
    const float margin=(mode==2 ? 4 : 2)*pixel;
    r.x=(corner&1) ? width-r.width-margin : margin;
    r.y=(corner&2) ? height-r.height-margin : margin;
    if (r.y<0) r.y=0;
    return r;
}
