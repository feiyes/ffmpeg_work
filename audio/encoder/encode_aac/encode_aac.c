#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/common.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>

#include "log.h"

//#define USE_LIBFDK_AAC 1

#ifndef USE_LIBFDK_AAC
static void get_adts_header(AVCodecContext *ctx, uint8_t *adts_header, int aac_length)
{
    uint8_t freq_idx = 0;

    switch (ctx->sample_rate) {
        case 96000: freq_idx = 0;  break;
        case 88200: freq_idx = 1;  break;
        case 64000: freq_idx = 2;  break;
        case 48000: freq_idx = 3;  break;
        case 44100: freq_idx = 4;  break;
        case 32000: freq_idx = 5;  break;
        case 24000: freq_idx = 6;  break;
        case 22050: freq_idx = 7;  break;
        case 16000: freq_idx = 8;  break;
        case 12000: freq_idx = 9;  break;
        case 11025: freq_idx = 10; break;
        case 8000:  freq_idx = 11; break;
        case 7350:  freq_idx = 12; break;
        default:    freq_idx = 4;  break;
    }

    uint8_t chanCfg = ctx->ch_layout.nb_channels;
    uint32_t frame_length = aac_length + 7;

    adts_header[0] = 0xFF;
    adts_header[1] = 0xF1;
    adts_header[2] = ((ctx->profile) << 6) + (freq_idx << 2) + (chanCfg >> 2);
    adts_header[3] = (((chanCfg & 3) << 6) + (frame_length  >> 11));
    adts_header[4] = ((frame_length & 0x7FF) >> 3);
    adts_header[5] = (((frame_length & 7) << 5) + 0x1F);
    adts_header[6] = 0xFC;
}
#endif

static int encode_process(AVCodecContext *ctx, AVFrame *frame, AVPacket *packet, FILE *output)
{
    int ret;
    size_t len = 0;

    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        } else if (ret < 0) {
            log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
            return -1;
        }

#ifndef USE_LIBFDK_AAC
        uint8_t aac_header[7];
        get_adts_header(ctx, aac_header, packet->size);

        len = fwrite(aac_header, 1, 7, output);
        if (len != 7) {
            log_err("fwrite aac_header failed\n");
            return -1;
        }
#endif

        len = fwrite(packet->data, 1, packet->size, output);
        if (len != packet->size) {
            log_err("fwrite aac data failed\n");
            return -1;
        }

        av_packet_unref(packet);
    }

    return 0;
}

#ifndef USE_LIBFDK_AAC
void f32le_convert_to_fltp(float *f32le, float *fltp, int nb_samples)
{
    float *fltp_l = fltp;
    float *fltp_r = fltp + nb_samples;

    for (int i = 0; i < nb_samples; i++) {
        fltp_l[i] = f32le[i*2];
        fltp_r[i] = f32le[i*2+1];
    }
}
#endif

// format s16: ffmpeg -i s16.aac -ar 48000 -ac 2 -f s16le 48000_2_s16le.pcm
// format flt: ffmpeg -i f32.aac -ar 48000 -ac 2 -f f32le 48000_2_f32le.pcm
int main(int argc, char **argv)
{
    int ret = -1;
    int64_t pts = 0;
    const char* pcm_file = NULL;
#ifdef USE_LIBFDK_AAC
    const char* aac_file = "s16.aac";
#else
    const char* aac_file = "f32.aac";
#endif

    if (argc <= 1) {
        fprintf(stderr, "Usage: %s <input file>\n", argv[0]);
        return 0;
    }

    pcm_file = argv[1];

    FILE *fp_in = fopen(pcm_file, "rb");
    if (!fp_in) {
        log_err("fopen %s failed\n", pcm_file);
        return -1;
    }

    FILE *fp_out = fopen(aac_file, "wb");
    if (!fp_out) {
        log_err("fopen %s failed\n", aac_file);
        return -1;
    }

#ifdef USE_LIBFDK_AAC
    const AVCodec *codec = avcodec_find_encoder_by_name("libfdk_aac");
#else
    const AVCodec *codec = avcodec_find_encoder_by_name("aac");
#endif
    if (!codec) {
        log_err("avcodec_find_encoder failed\n");
        return -1;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        log_err("avcodec_alloc_context3 failed\n");
        return -1;
    }

    codec_ctx->codec_id    = AV_CODEC_ID_AAC;
    codec_ctx->codec_type  = AVMEDIA_TYPE_AUDIO;
    codec_ctx->bit_rate    = 128*1024;
    codec_ctx->sample_rate = 48000;
    codec_ctx->profile     = FF_PROFILE_AAC_LOW;
#ifdef USE_LIBFDK_AAC
    codec_ctx->sample_fmt  = AV_SAMPLE_FMT_S16;
#else
    codec_ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
#endif
    av_channel_layout_copy(&codec_ctx->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);

    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    frame->format     = codec_ctx->sample_fmt;
    frame->nb_samples = codec_ctx->frame_size;
    ret = av_channel_layout_copy(&frame->ch_layout, &codec_ctx->ch_layout);
    if (ret < 0) {
        log_err("av_channel_layout_copy failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        log_err("av_frame_get_buffer failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    int frame_bytes = av_samples_get_buffer_size(NULL, frame->ch_layout.nb_channels,
                                                 frame->nb_samples, frame->format, 1);

    uint8_t *pcm_buf = (uint8_t *)malloc(frame_bytes);
    if (!pcm_buf) {
        log_err("malloc pcm buffer failed\n");
        return -1;
    }

    memset(pcm_buf, 0, frame_bytes);

#ifndef USE_LIBFDK_AAC
    uint8_t *pcm_fltp_buf = (uint8_t *)malloc(frame_bytes);
    if (!pcm_fltp_buf) {
        log_err("malloc pcm buffer failed\n");
        return -1;
    }

    memset(pcm_fltp_buf, 0, frame_bytes);

    //struct SwrContext *swr_ctx = swr_alloc();
    //if (!swr_ctx) {
    //    log_err("swr_alloc failed");
    //    return -1;
    //}

    //av_opt_set_chlayout  (swr_ctx, "in_chlayout",      &codec_ctx->ch_layout,    0);
    //av_opt_set_int       (swr_ctx, "in_sample_rate",    codec_ctx->sample_rate,  0);
    //av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",     codec_ctx->sample_fmt,   0);
    //av_opt_set_chlayout  (swr_ctx, "out_chlayout",     &codec_ctx->ch_layout,    0);
    //av_opt_set_int       (swr_ctx, "out_sample_rate",   codec_ctx->sample_rate,  0);
    //av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt",    codec_ctx->sample_fmt,   0);

    //ret = swr_init(swr_ctx);
    //if (ret < 0) {
    //    log_err("swr_init, error(%s)", av_err2str(ret));
    //    return -1;
    //}
#endif

    log_info("start encoder");

    for (;;) {
        size_t read_bytes = fread(pcm_buf, 1, frame_bytes, fp_in);
        if (read_bytes <= 0) {
            break;
        }

        ret = av_frame_make_writable(frame);
        if (ret != 0) {
            log_err("av_frame_make_writable failed, ret = %d\n", ret);
            break;
        }

#ifdef USE_LIBFDK_AAC
        ret = av_samples_fill_arrays(frame->data, frame->linesize,
                                     pcm_buf, frame->ch_layout.nb_channels,
                                     frame->nb_samples, frame->format, 0);
#else
        f32le_convert_to_fltp((float *)pcm_buf, (float *)pcm_fltp_buf, frame->nb_samples);

        ret = av_samples_fill_arrays(frame->data, frame->linesize,
                                     pcm_fltp_buf, frame->ch_layout.nb_channels,
                                     frame->nb_samples, frame->format, 0);
#endif

        frame->pts = pts;
        ret = encode_process(codec_ctx, frame, packet, fp_out);
        if (ret < 0) {
            log_err("encode failed\n");
            break;
        }

        pts += frame->nb_samples;
    }

    encode_process(codec_ctx, NULL, packet, fp_out);

    log_info("end encoder");

    if (fp_in)   fclose(fp_in);
    if (fp_out)  fclose(fp_out);
    if (pcm_buf) free(pcm_buf);

#ifndef USE_LIBFDK_AAC
    if (pcm_fltp_buf) free(pcm_fltp_buf);
#endif

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);

    return 0;
}
