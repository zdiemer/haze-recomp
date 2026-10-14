#ifndef HAZE_AUDIO_LIMITER_H
#define HAZE_AUDIO_LIMITER_H

#include <math.h>
#include <string.h>

/* The native mixer produces 256 stereo frames at 48 kHz. Retain one block
 * (5.33 ms) so gain can fall BEFORE an overloaded gunshot, instead of chopping
 * its waveform at full scale. Both channels share gain to preserve direction.
 * There is no makeup gain: ordinary audio passes through at its original level.
 */
#define HAZE_AUDIO_LIMITER_FRAMES 256
#define HAZE_AUDIO_LIMITER_CEILING 0.95f
typedef struct HazeAudioLimiter {
    float pending[HAZE_AUDIO_LIMITER_FRAMES * 2];
    float peak;
    float gain;
} HazeAudioLimiter;

static void haze_audio_limiter_reset(HazeAudioLimiter* limiter)
{
    memset(limiter, 0, sizeof(*limiter));
    limiter->gain = 1.0f;
}

static void haze_audio_limiter_process(HazeAudioLimiter* limiter, float* stereo)
{
    float peak = 0.0f;
    for (unsigned i = 0; i < HAZE_AUDIO_LIMITER_FRAMES * 2; ++i) {
        if (!isfinite(stereo[i])) stereo[i] = 0.0f;
        const float amplitude = fabsf(stereo[i]);
        if (amplitude > peak) peak = amplitude;
    }
    /* The previous call already prepared gain for pending's peak. Using both
     * peaks here keeps release safe and prepares the following block as well. */
    const float maximum = peak > limiter->peak ? peak : limiter->peak;
    const float target = maximum > HAZE_AUDIO_LIMITER_CEILING
        ? HAZE_AUDIO_LIMITER_CEILING / maximum : 1.0f;
    const float start = limiter->gain;
    float gain = start;
    for (unsigned frame = 0; frame < HAZE_AUDIO_LIMITER_FRAMES; ++frame) {
        if (target < start) {
            const float fraction = (frame + 1) / (float)HAZE_AUDIO_LIMITER_FRAMES;
            gain = start * (1.0f - fraction) + target * fraction;
        } else {
            /* 100 ms release time constant: 1 - exp(-1 / (48000 * .100)). */
            gain += (target - gain) * 0.00020831163f;
            if (1.0f - gain < 0.0002f && target == 1.0f) gain = 1.0f;
        }
        for (unsigned channel = 0; channel < 2; ++channel) {
            const unsigned i = frame * 2 + channel;
            const float input = stereo[i];
            stereo[i] = limiter->pending[i] * gain;
            limiter->pending[i] = input;
        }
    }
    limiter->peak = peak;
    limiter->gain = gain;
}

#endif
