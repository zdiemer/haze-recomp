#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <cstdint>
namespace haze_movie {
struct VideoFrame { double pts=0; int width=0,height=0; std::vector<uint8_t> rgba; };
struct AudioFrame { double pts=0; std::vector<float> stereo; };
class Decoder {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    Decoder(const char* path, std::function<void(VideoFrame&&)> video,
            std::function<void(AudioFrame&&)> audio);
    ~Decoder();
    bool step(); // one bounded PES packet; flushes delayed frames once at EOF
    double duration() const;
    bool has_audio() const;
};
}
