#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "../port/haze_audio_limiter.h"
#include <cassert>
#include <cstdio>
#include <limits>

static constexpr unsigned samples = HAZE_AUDIO_LIMITER_FRAMES * 2;
static void bounded(const float* block) {
    for (unsigned i = 0; i < samples; ++i)
        assert(isfinite(block[i]) && fabsf(block[i]) <= HAZE_AUDIO_LIMITER_CEILING + 0.000001f);
}

int main(int argc, char** argv) {
    HazeAudioLimiter limiter;
    haze_audio_limiter_reset(&limiter);
    float block[samples], original[samples];
    for (unsigned i = 0; i < samples; ++i) original[i] = .4f * sinf((float)i);
    memcpy(block, original, sizeof(block));
    haze_audio_limiter_process(&limiter, block);
    for (float sample : block) assert(sample == 0); // Exactly one block of delay.
    memset(block, 0, sizeof(block));
    haze_audio_limiter_process(&limiter, block);
    assert(memcmp(block, original, sizeof(block)) == 0); // No quiet-audio gain change.

    // A new transient ramps down the preceding quiet block, without a gain
    // step at its start. This distinguishes lookahead from a block normalizer.
    haze_audio_limiter_reset(&limiter);
    for (float& sample : block) sample = .2f;
    haze_audio_limiter_process(&limiter, block);
    for (float& sample : block) sample = 8.f;
    haze_audio_limiter_process(&limiter, block);
    float previous = .2f;
    for (unsigned frame = 0; frame < HAZE_AUDIO_LIMITER_FRAMES; ++frame) {
        assert(block[frame * 2] < previous);
        assert(previous - block[frame * 2] < .001f);
        previous = block[frame * 2];
    }

    // Overloads at every offset, including either side of a block boundary.
    // Shared gain must preserve the 4:1 channel ratio under attack/release.
    for (unsigned offset = 0; offset < HAZE_AUDIO_LIMITER_FRAMES; ++offset) {
        haze_audio_limiter_reset(&limiter);
        for (unsigned b = 0; b < 6; ++b) {
            for (unsigned frame = 0; frame < HAZE_AUDIO_LIMITER_FRAMES; ++frame) {
                const float value = (b == 1 || b == 2) && frame == offset ? -8.0f : .2f;
                block[frame * 2] = value;
                block[frame * 2 + 1] = value * .25f;
            }
            haze_audio_limiter_process(&limiter, block);
            bounded(block);
            for (unsigned frame = 0; frame < HAZE_AUDIO_LIMITER_FRAMES; ++frame)
                assert(block[frame * 2 + 1] == block[frame * 2] * .25f);
        }
    }
    // Dense, overlapping signals with abrupt level changes must not flat-top.
    haze_audio_limiter_reset(&limiter);
    unsigned random = 17;
    for (unsigned b = 0; b < 1000; ++b) {
        for (unsigned i = 0; i < samples; ++i) {
            random = random * 1664525u + 1013904223u;
            block[i] = ((int)(random >> 16) - 32768) / 32768.f * (b % 5 ? 4.f : .2f);
        }
        haze_audio_limiter_process(&limiter, block);
        bounded(block);
    }
    // Recovery is gradual, reaches unity, and cannot amplify quiet audio.
    float prior = limiter.gain;
    for (unsigned b = 0; b < 400; ++b) {
        for (float& sample : block) sample = .2f;
        haze_audio_limiter_process(&limiter, block);
        bounded(block);
        assert(limiter.gain >= prior && limiter.gain <= 1.f);
        prior = limiter.gain;
    }
    assert(limiter.gain == 1.f);
    for (float sample : block) assert(sample == .2f);

    // Invalid samples cannot poison gain/history, and reinit discards history.
    block[0] = std::numeric_limits<float>::quiet_NaN();
    block[1] = std::numeric_limits<float>::infinity();
    haze_audio_limiter_process(&limiter, block);
    memset(block, 0, sizeof(block));
    haze_audio_limiter_process(&limiter, block);
    bounded(block);
    assert(block[0] == 0 && block[1] == 0);
    haze_audio_limiter_reset(&limiter);
    memset(block, 0, sizeof(block));
    haze_audio_limiter_process(&limiter, block);
    for (float sample : block) assert(sample == 0);

    // Optional replay of PRIVATE raw pre-clamp captures through the same code.
    if (argc == 3) {
        FILE* input = fopen(argv[1], "rb");
        FILE* output = fopen(argv[2], "wb");
        assert(input && output);
        haze_audio_limiter_reset(&limiter);
        while (fread(block, sizeof(float), samples, input) == samples) {
            haze_audio_limiter_process(&limiter, block);
            bounded(block);
            assert(fwrite(block, sizeof(float), samples, output) == samples);
        }
        assert(!ferror(input));
        // Flush the final delayed block for sample-aligned capture comparisons.
        memset(block, 0, sizeof(block));
        haze_audio_limiter_process(&limiter, block);
        assert(fwrite(block, sizeof(float), samples, output) == samples);
        fclose(input);
        assert(fclose(output) == 0);
    }
    puts("Audio limiter: unity passthrough, bounded peaks, stereo linking, recovery and reset passed.");
}
