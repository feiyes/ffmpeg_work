#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <SDL2/SDL.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#ifdef __cplusplus
};
#endif

#include "log.h"

typedef struct {
    int    len;
    Uint8* data;

    int frame_bytes;
    int dst_nb_samples;
    uint8_t **dst_data;
} FF_SwrInfo;

void fill_audio(void *udata, Uint8 *stream, int len)
{
    FF_SwrInfo* pcm = (FF_SwrInfo*)udata;
    if (!pcm)
        return;

    if (pcm->len == 0)
        return;

    SDL_memset(stream, 0, len);
    len = (len > pcm->len ? pcm->len : len);

    SDL_MixAudio(stream, pcm->data, len, SDL_MIX_MAXVOLUME);

    pcm->len  -= len;
    pcm->data += len;
}

void decode_process(AVCodecContext* codec_ctx, AVPacket* packet, AVFrame* frame, struct SwrContext *swr_ctx, FF_SwrInfo* swr_info)
{
    int ret;

    ret = avcodec_send_packet(codec_ctx, packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            continue;
        else if (ret < 0) {
            continue;
        }

        swr_convert(swr_ctx, swr_info->dst_data, swr_info->dst_nb_samples, (const uint8_t **)frame->data, frame->nb_samples);

        while (swr_info->len > 0)
            SDL_Delay(1);

        swr_info->len  = swr_info->frame_bytes;
        swr_info->data = (Uint8 *)(*swr_info->dst_data);
    }
}

int main(int argc, char* argv[])
{
    int ret;
    char* url;
    int audio_index;
    int frame_bytes;
    int dst_linesize;
    int dst_nb_samples;
    int dst_sample_rate;
    int dst_nb_channels;
    AVFrame* frame;
    AVPacket* packet;
    uint8_t **dst_data;
    const AVCodec* codec;
    AVFormatContext* fmt_ctx;
    AVCodecContext* codec_ctx;
    SDL_AudioSpec wanted_spec;
    struct SwrContext *swr_ctx;
    FF_SwrInfo ff_swr_info = {0};
    AVChannelLayout dst_ch_layout;
    enum AVSampleFormat dst_sample_fmt;

    if (argc <= 1) {
        fprintf(stderr, "Usage: %s <input file>\n", argv[0]);
        return 0;
    }

    url = argv[1];

    fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        log_err("avformat_alloc_context failed\n");
        return -1;
    }

    ret = avformat_open_input(&fmt_ctx, url, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input %s failed, error(%s)", url, av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info %s failed, error(%s)", url, av_err2str(ret));
        return -1;
    }

    av_dump_format(fmt_ctx, 0, url, false);

    audio_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (audio_index < 0) {
        log_err("av_find_best_stream %s failed\n", url);
        return -1;
    }

    codec = avcodec_find_decoder(fmt_ctx->streams[audio_index]->codecpar->codec_id);
    if (!codec) {
        log_err("avcodec_find_decoder %s failed\n", avcodec_get_name(fmt_ctx->streams[audio_index]->codecpar->codec_id));
        return -1;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        log_err("avcodec_alloc_context3 failed\n");
        return -1;
    }

    codec_ctx->pkt_timebase = fmt_ctx->streams[audio_index]->time_base;
    ret = avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[audio_index]->codecpar);
    if (ret < 0) {
        log_err("avcodec_parameters_to_context failed");
        return -1;
    }

    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return -1;
    }

    packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    frame = av_frame_alloc();
    if (!frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    // dst buffer configuration
    dst_sample_fmt  = AV_SAMPLE_FMT_S16;
    dst_sample_rate = codec_ctx->sample_rate;
    dst_nb_channels = codec_ctx->ch_layout.nb_channels;

    av_channel_layout_default(&dst_ch_layout, dst_nb_channels);
    av_channel_layout_copy(&dst_ch_layout, &codec_ctx->ch_layout);
    dst_nb_samples = av_rescale_rnd(codec_ctx->frame_size, dst_sample_rate,
                                    codec_ctx->sample_rate, AV_ROUND_UP);

    frame_bytes = av_samples_get_buffer_size(NULL, dst_nb_channels,
                                             dst_nb_samples, dst_sample_fmt, 1);

    ret = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, dst_nb_channels,
                                             dst_nb_samples, dst_sample_fmt, 0);

    ff_swr_info.dst_data = dst_data;
    ff_swr_info.frame_bytes = frame_bytes;
    ff_swr_info.dst_nb_samples = dst_nb_samples;

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        log_err("swr_alloc failed");
        return -1;
    }

    av_opt_set_chlayout  (swr_ctx, "in_chlayout",      &codec_ctx->ch_layout,    0);
    av_opt_set_int       (swr_ctx, "in_sample_rate",    codec_ctx->sample_rate,  0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",     codec_ctx->sample_fmt,   0);
    av_opt_set_chlayout  (swr_ctx, "out_chlayout",     &dst_ch_layout,           0);
    av_opt_set_int       (swr_ctx, "out_sample_rate",   dst_sample_rate,         0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt",    dst_sample_fmt,          0);

    ret = swr_init(swr_ctx);
    if (ret < 0) {
        log_err("swr_init, error(%s)", av_err2str(ret));
        return -1;
    }

    ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
    if (ret) {
        log_err("SDL_Init, error(%s)", SDL_GetError());
        return -1;
    }

    wanted_spec.freq     = dst_sample_rate;
    wanted_spec.format   = AUDIO_S16SYS;
    wanted_spec.channels = dst_nb_channels;
    wanted_spec.silence  = 0;
    wanted_spec.samples  = dst_nb_samples;
    wanted_spec.callback = fill_audio;
    wanted_spec.userdata = &ff_swr_info;

    ret = SDL_OpenAudio(&wanted_spec, NULL);
    if (ret) {
        log_err("SDL_OpenAudio, error(%s)", SDL_GetError());
        return -1;
    }

    SDL_PauseAudio(0);

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index == audio_index) {
            decode_process(codec_ctx, packet, frame, swr_ctx, &ff_swr_info);
        }
    }

    SDL_CloseAudio();
    SDL_Quit();

    if (dst_data)
        av_freep(&dst_data[0]);
    av_freep(&dst_data);

    swr_free(&swr_ctx);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_close(codec_ctx);
    avformat_close_input(&fmt_ctx);

    return 0;
}
