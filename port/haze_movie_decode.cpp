#include "haze_movie_decode.h"
#include "haze_pamf.h"
#include <algorithm>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
namespace haze_movie {
struct Decoder::Impl {
    Pamf file;
    AVCodecContext *video=nullptr,*audio=nullptr;
    AVCodecParserContext* parser=nullptr;
    AVFrame* frame=nullptr;
    AVPacket* packet=nullptr;
    SwsContext* scale=nullptr;
    SwrContext* resample=nullptr;
    std::vector<uint8_t> audio_bytes;
    int64_t audio_samples=0;
    double audio_origin=0;
    double next_video_pts=0;
    bool audio_started=false, audio_open=false, eof=false;
    std::function<void(VideoFrame&&)> on_video;
    std::function<void(AudioFrame&&)> on_audio;
    Impl(const char* path, std::function<void(VideoFrame&&)> v, std::function<void(AudioFrame&&)> a)
        : file(path), on_video(std::move(v)),on_audio(std::move(a)) {
        try {
            video=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO));
            require(video!=nullptr,"Cannot allocate movie video decoder");
            video->pkt_timebase={1,90000}; video->thread_count=2;
            require(avcodec_open2(video,video->codec,nullptr)>=0,"Cannot open MPEG-2 decoder");
            parser=av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
            frame=av_frame_alloc(); packet=av_packet_alloc();
            require(parser && frame && packet,"Cannot allocate movie decode buffers");
            if (file.audio_id>=0) {
                audio=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_ATRAC3P));
                require(audio!=nullptr,"Cannot allocate ATRAC3+ decoder");
                audio->sample_rate=48000;
                av_channel_layout_default(&audio->ch_layout,file.channels);
            }
        } catch (...) { cleanup(); throw; }
    }
    void cleanup() {
        swr_free(&resample); sws_freeContext(scale);
        av_packet_free(&packet); av_frame_free(&frame);
        if(parser) av_parser_close(parser);
        avcodec_free_context(&audio); avcodec_free_context(&video);
    }
    ~Impl() { cleanup(); }
    void receive(AVCodecContext* codec) {
        for (;;) {
            const int result=avcodec_receive_frame(codec,frame);
            if(result==AVERROR(EAGAIN) || result==AVERROR_EOF) return;
            require(result>=0,"Movie frame decoding failed");
            if (codec==video) {
                require(frame->width>0 && frame->height>0 && frame->width<=4096 && frame->height<=2160,
                        "Unsupported movie dimensions");
                VideoFrame out;
                // PAMF can timestamp only the first picture in a PES. The
                // decoder returns display order, so missing picture times use
                // the MPEG sequence's frame rate after B-frame reordering.
                out.pts=frame->best_effort_timestamp==AV_NOPTS_VALUE ? next_video_pts :
                    (frame->best_effort_timestamp-file.start)/90000.0;
                require(video->framerate.num>0 && video->framerate.den>0,"Movie lacks frame rate");
                next_video_pts=out.pts+av_q2d(av_inv_q(video->framerate));
                out.width=frame->width; out.height=frame->height;
                out.rgba.resize(size_t(out.width)*out.height*4);
                scale=sws_getCachedContext(scale,out.width,out.height,(AVPixelFormat)frame->format,
                    out.width,out.height,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
                require(scale!=nullptr,"Cannot convert movie video");
                const int* coefficients=sws_getCoefficients(frame->colorspace==AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
                sws_setColorspaceDetails(scale,coefficients,frame->color_range==AVCOL_RANGE_JPEG,
                                        coefficients,1,0,1<<16,1<<16);
                uint8_t* dst[]={out.rgba.data()}; int stride[]={out.width*4};
                require(sws_scale(scale,frame->data,frame->linesize,0,out.height,dst,stride)==out.height,
                        "Movie color conversion failed");
                on_video(std::move(out));
            } else {
                if (!resample) {
                    AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
                    require(swr_alloc_set_opts2(&resample,&stereo,AV_SAMPLE_FMT_FLT,48000,
                        &frame->ch_layout,(AVSampleFormat)frame->format,frame->sample_rate,0,nullptr)>=0 &&
                        swr_init(resample)>=0,"Cannot convert movie audio");
                }
                AudioFrame out; out.pts=audio_origin+audio_samples/48000.0;
                out.stereo.resize(size_t(frame->nb_samples)*2);
                uint8_t* dst[]={reinterpret_cast<uint8_t*>(out.stereo.data())};
                const int count=swr_convert(resample,dst,frame->nb_samples,
                    const_cast<const uint8_t**>(frame->extended_data),frame->nb_samples);
                require(count>=0,"Movie audio conversion failed");
                out.stereo.resize(size_t(count)*2); audio_samples+=count;
                on_audio(std::move(out));
            }
            av_frame_unref(frame);
        }
    }
    void send(AVCodecContext* codec,const uint8_t* data,int size,int64_t pts,int64_t dts) {
        av_packet_unref(packet);
        require(av_new_packet(packet,size)>=0,"Cannot allocate movie packet");
        memcpy(packet->data,data,size); packet->pts=pts; packet->dts=dts;
        int result=avcodec_send_packet(codec,packet);
        if(result==AVERROR(EAGAIN)) { receive(codec); result=avcodec_send_packet(codec,packet); }
        require(result>=0,"Movie packet decoding failed"); receive(codec);
    }
    bool step() {
        if(eof) return false;
        Packet p;
        if(!file.next(p)) {
            uint8_t* out=nullptr; int size=0;
            av_parser_parse2(parser,video,&out,&size,nullptr,0,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
            if(size) send(video,out,size,parser->pts,parser->dts);
            require(avcodec_send_packet(video,nullptr)>=0,"Cannot drain video decoder"); receive(video);
            if(audio) {
                require(audio_open,"PAMF audio stream has no access units");
                require(audio_bytes.empty(),"Truncated ATRAC3+ frame");
                require(avcodec_send_packet(audio,nullptr)>=0,"Cannot drain audio decoder"); receive(audio);
            }
            eof=true; return false;
        }
        if(p.id==file.video_id) {
            size_t left=p.data.size(); p.data.resize(left+AV_INPUT_BUFFER_PADDING_SIZE,0);
            auto* data=p.data.data();
            int64_t pts=p.pts<0?AV_NOPTS_VALUE:p.pts, dts=p.dts<0?AV_NOPTS_VALUE:p.dts;
            while(left) {
                uint8_t* out=nullptr; int size=0;
                int used=av_parser_parse2(parser,video,&out,&size,data,int(left),pts,dts,-1);
                require(used>=0 && (used || size),"Invalid MPEG-2 packet");
                if(size) send(video,out,size,parser->pts,parser->dts);
                data+=used; left-=used; pts=dts=AV_NOPTS_VALUE;
            }
        } else if(audio) {
            if(!audio_started) {
                require(p.pts>=0,"Audio stream lacks PTS");
                audio_origin=(p.pts-file.start)/90000.0; audio_started=true;
            }
            audio_bytes.insert(audio_bytes.end(),p.data.begin(),p.data.end());
            size_t used=0;
            while(audio_bytes.size()-used>=8) {
                const auto* h=audio_bytes.data()+used;
                require(h[0]==0x0f && h[1]==0xd0,"Invalid ATRAC3+ access unit");
                const size_t size=(((h[2]&3)<<8)|h[3])*8+8;
                if(audio_bytes.size()-used<size+8) break;
                if(!audio_open) {
                    audio->block_align=int(size);
                    require(avcodec_open2(audio,audio->codec,nullptr)>=0,"Cannot open ATRAC3+ decoder");
                    audio_open=true;
                }
                require(audio->block_align==int(size),"ATRAC3+ frame size changed");
                send(audio,h+8,int(size),AV_NOPTS_VALUE,AV_NOPTS_VALUE); used+=size+8;
            }
            audio_bytes.erase(audio_bytes.begin(),audio_bytes.begin()+used);
            require(audio_bytes.size()<8208,"Oversized ATRAC3+ packet");
        }
        return true;
    }
};
Decoder::Decoder(const char* path,std::function<void(VideoFrame&&)> video,std::function<void(AudioFrame&&)> audio)
    : impl(std::make_unique<Impl>(path,std::move(video),std::move(audio))) {}
Decoder::~Decoder() = default;
bool Decoder::step() { return impl->step(); }
double Decoder::duration() const { return (impl->file.end-impl->file.start)/90000.0; }
bool Decoder::has_audio() const { return impl->file.audio_id>=0; }
}
