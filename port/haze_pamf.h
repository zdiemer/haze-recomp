#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace haze_movie {
inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
inline uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
struct Packet {
    int id = 0;
    int64_t pts = -1, dts = -1;
    std::vector<uint8_t> data;
};
// The supported disc uses one PAMF group, MPEG-2 video and ATRAC3+ tracks.
// Read bounded PES packets; never load the complete movie into memory.
class Pamf {
    FILE* file = nullptr;
    uint64_t remaining = 0;
    void read(void* out, size_t size) {
        require(size <= remaining, "Truncated PAMF packet");
        require(fread(out, 1, size, file) == size, "PAMF read failed");
        remaining -= size;
    }
    static int64_t timestamp(const uint8_t* p) {
        require((p[0]&1) && (p[2]&1) && (p[4]&1), "Invalid PES timestamp");
        return int64_t((p[0]>>1)&7)<<30 | int64_t(p[1])<<22 |
            int64_t(p[2]>>1)<<15 | int64_t(p[3])<<7 | (p[4]>>1);
    }
public:
    int video_id = -1, audio_id = -1, channels = 0;
    int64_t start = 0, end = 0;
    explicit Pamf(const char* path) {
        file = fopen(path, "rb");
        require(file != nullptr, "Cannot open movie");
        try {
            uint8_t h[2048];
            require(fread(h, 1, sizeof(h), file) == sizeof(h), "Truncated PAMF header");
            require(!memcmp(h, "PAMF004", 7) && (h[7]=='0' || h[7]=='1'), "Unsupported PAMF version");
            const uint64_t offset = uint64_t(be32(h+8))*2048;
            remaining = uint64_t(be32(h+12))*2048;
            require(offset >= 2048 && remaining > 0, "Invalid PAMF extent");
            _fseeki64(file, 0, SEEK_END);
            const auto size = _ftelli64(file);
            require(size >= 0 && offset <= uint64_t(size) && remaining <= uint64_t(size)-offset,
                    "Truncated PAMF data");
            require(_fseeki64(file, offset, SEEK_SET) == 0, "PAMF seek failed");
            start = int64_t(h[0x56])<<40 | int64_t(h[0x57])<<32 | be32(h+0x58);
            end = int64_t(h[0x5c])<<40 | int64_t(h[0x5d])<<32 | be32(h+0x5e);
            require(end > start && h[0x6f]==1 && h[0x81]==1, "Unsupported PAMF grouping");
            const unsigned count = h[0x87];
            require(count && 0x88+count*48 <= sizeof(h), "Invalid PAMF stream table");
            for (unsigned i=0; i<count; ++i) {
                const auto* s=h+0x88+i*48;
                if (s[0]==2 && video_id<0) video_id=s[4];
                if (s[0]==0xdc && audio_id<0) {
                    audio_id=s[5]; channels=s[18];
                    require(s[19]==1 && (channels==1 || channels==2 || channels==6 || channels==8),
                            "Unsupported PAMF audio format");
                }
            }
            require(video_id>=0, "PAMF has no supported video stream");
        } catch (...) { fclose(file); file=nullptr; throw; }
    }
    ~Pamf() { if (file) fclose(file); }
    Pamf(const Pamf&) = delete;
    bool next(Packet& packet) {
        while (remaining) {
            uint8_t h[14]; read(h,4);
            require(h[0]==0 && h[1]==0 && h[2]==1, "Invalid PAMF packet boundary");
            const int id=h[3];
            if (id==0xb9) { remaining=0; return false; }
            if (id==0xba) {
                read(h+4,10); uint8_t stuffing[7]; read(stuffing,h[13]&7); continue;
            }
            read(h+4,2);
            const size_t size=(unsigned(h[4])<<8)|h[5];
            packet.data.resize(size); read(packet.data.data(),size);
            if (id!=video_id && id!=0xbd) continue;
            auto& b=packet.data;
            require(size>=3 && (b[0]&0xc0)==0x80 && size>=3u+b[2], "Invalid PES header");
            packet.pts=packet.dts=-1;
            if (b[1]&0x80) {
                require(b[2]>=5, "Truncated PES PTS"); packet.pts=timestamp(b.data()+3);
                packet.dts=packet.pts;
            }
            if (b[1]&0x40) {
                require((b[1]&0x80) && b[2]>=10, "Truncated PES DTS");
                packet.dts=timestamp(b.data()+8);
            }
            size_t skip=3+b[2];
            if (id==0xbd) {
                require(size>=skip+4, "Truncated private PES header");
                if (b[skip]!=audio_id) continue;
                skip+=4;
            }
            b.erase(b.begin(),b.begin()+skip); packet.id=id;
            return true;
        }
        return false;
    }
};
}
