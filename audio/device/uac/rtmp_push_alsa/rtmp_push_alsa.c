#include <sys/time.h>
#include <alsa/asoundlib.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include "log.h"

#define CHANNELS 1
#define FSIZE    2 * CHANNELS

//#define DUMP_PCM_FILE  1
//#define DUMP_SWS_FILE  1
//#define DUMP_AAC_FILE  1
#define ENC_FRAME_ERR_EOF  -1

static int pack_process(AVFormatContext* fmt_ctx, AVCodecContext *enc_ctx, AVPacket *packet, AVStream *stream)
{
    int ret = -1;

    packet->pts = av_rescale_q(packet->pts, enc_ctx->time_base, stream->time_base);
    packet->dts = av_rescale_q(packet->dts, enc_ctx->time_base, stream->time_base);
    packet->duration = av_rescale_q(packet->duration, enc_ctx->time_base, stream->time_base);

    // log_info("pts %ld dts %ld duration %ld", packet->pts, packet->dts, packet->duration);
    ret = av_interleaved_write_frame(fmt_ctx, packet);
    if (ret < 0) {
        log_err("av_interleaved_write_frame failed, error(%s)", av_err2str(ret));
        return -1;
    }

    return 0;
}

static int encode_process(AVFormatContext* fmt_ctx, AVCodecContext *ctx, AVFrame *frame, AVPacket *packet, AVStream *stream, FILE *fp_aac)
{
    int ret = -1;

    // log_info("frame pts = %ld", frame->pts);
    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, packet);
        if (ret == AVERROR(EAGAIN)) {
            return 0;
        } else if (ret == AVERROR_EOF) {
            log_warn("avcodec_receive_packet EOF");
            return ENC_FRAME_ERR_EOF;
        } else if (ret < 0) {
            log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
            return -1;
        }

        if (packet->pts < 0) {
            log_warn("packet pts = %ld", packet->pts);
            continue;
        }

        pack_process(fmt_ctx, ctx, packet, stream);

#ifdef DUMP_AAC_FILE
        size_t len = fwrite(packet->data, 1, packet->size, fp_aac);
        if (len != packet->size) {
            log_err("fwrite aac data failed\n");
            return -1;
        }
#endif

        av_packet_unref(packet);
    }

    return 0;
}

int main()
{
    int ret = 0;
    FILE *fp_aac = NULL;
    char* device = "hw:Camera";

#ifdef DUMP_PCM_FILE
    char* pcm_file = "output.pcm";
    FILE *fp_pcm = fopen(pcm_file, "wb");
    if (!fp_pcm) {
        log_err("fopen %s failed", pcm_file);
        return -1;
    }
#endif

#ifdef DUMP_SWS_FILE
    char* sws_file = "output.raw";
    FILE *fp_sws = fopen(sws_file, "wb");
    if (!fp_sws) {
        log_err("fopen %s failed", pcm_file);
        return -1;
    }
#endif

#ifdef DUMP_AAC_FILE
    char *aac_file = "output.aac";
    fp_aac = fopen(aac_file, "wb");
    if (!fp_aac) {
        log_err("fopen %s failed", aac_file);
        return -1;
    }
#endif

    snd_pcm_t *handle;
    ret = snd_pcm_open(&handle, device, SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0) {
        log_err("snd_pcm_open failed");
        return -1;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_malloc(&params);
    ret = snd_pcm_hw_params_any(handle, params);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_any failed");
        return -1;
    }

    ret = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_access failed");
        return -1;
    }

    ret = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_format to 16-bit signed failed");
        return -1;
    }

    ret = snd_pcm_hw_params_set_channels(handle, params, CHANNELS);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_channels failed");
        return -1;
    }

    int dir;
    unsigned int val = 48000;
    ret = snd_pcm_hw_params_set_rate_near(handle, params, &val, &dir);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_rate_near failed");
        return -1;
    }

    unsigned int buffer_time, period_time;
    //set buffer_time 500000us = 0.5s
    snd_pcm_hw_params_get_buffer_time_max(params, &buffer_time, 0);
    if (buffer_time > 500000)
        buffer_time = 500000;

    ret = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, 0);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_buffer_time_near failed");
        return -1;
    }

    period_time = 26315;
    ret = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, 0);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_period_time_near failed");
        return -1;
    }

    ret = snd_pcm_hw_params(handle, params);
    if (ret < 0) {
        log_err("snd_pcm_hw_params failed");
        return -1;
    }

    int size;
    snd_pcm_uframes_t frames;
    snd_pcm_hw_params_get_period_size(params, &frames, &dir);
    log_info("period_size:%ld",frames);
    // 1 frame = channels * sample_size.
    size = frames * FSIZE; /* 2 bytes/sample, 1 channels */
    log_info("size:%d",size);
    char* buffer = (char *) malloc(size);

    AVFrame* pcm_frame = av_frame_alloc();
    if (!pcm_frame) {
        log_err("av_frame_alloc failed");
        return -1;
    }

    pcm_frame->format = AV_SAMPLE_FMT_S16;
    pcm_frame->sample_rate = 48000;
    pcm_frame->nb_samples = frames;
    av_channel_layout_copy(&pcm_frame->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO);
    av_frame_get_buffer(pcm_frame, 0);

    AVFrame* aac_frame = av_frame_alloc();
    if (!aac_frame) {
        log_err("av_frame_alloc failed");
        return -1;
    }

    aac_frame->format = AV_SAMPLE_FMT_S16;
    aac_frame->sample_rate = 48000;
    aac_frame->nb_samples = 1024;
    av_channel_layout_copy(&aac_frame->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO);
    av_frame_get_buffer(aac_frame, 0);

    struct SwrContext *swr_ctx = swr_alloc();
    if (!swr_ctx) {
        log_err("swr_alloc failed");
        return -1;
    }

    av_opt_set_chlayout  (swr_ctx, "in_chlayout",     &pcm_frame->ch_layout,   0);
    av_opt_set_int       (swr_ctx, "in_sample_rate",   pcm_frame->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",    pcm_frame->format,      0);
    av_opt_set_chlayout  (swr_ctx, "out_chlayout",    &aac_frame->ch_layout,   0);
    av_opt_set_int       (swr_ctx, "out_sample_rate",  aac_frame->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt",   aac_frame->format,      0);

    ret = swr_init(swr_ctx);
    if (ret < 0) {
        log_err("swr_init failed");
        return -1;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name("libfdk_aac");
    if (!codec) {
        log_err("avcodec_find_encoder failed");
        return -1;
    }

    AVCodecContext* enc_ctx = avcodec_alloc_context3(codec);
    if (!enc_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return -1;
    }

    enc_ctx->codec_id    = codec->id;
    enc_ctx->codec_type  = AVMEDIA_TYPE_AUDIO;
    enc_ctx->sample_fmt  = AV_SAMPLE_FMT_S16;
    enc_ctx->bit_rate    = 64000;
    enc_ctx->sample_rate = 48000;
    av_channel_layout_copy(&enc_ctx->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO);

    AVFormatContext* fmt_ctx = NULL;
    char *url ="rtmp://127.0.0.1/live/stream";
    ret = avformat_alloc_output_context2(&fmt_ctx, NULL, "flv", url);
    if (ret != 0) {
        log_err("avformat_alloc_output_context2 failed");
        return -1;;
    }

    if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(enc_ctx, codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return -1;
    }

    AVStream *stream = avformat_new_stream(fmt_ctx,NULL);
    if (!stream) {
        log_err("avformat_new_stream failed");
        return -1;
    }

    avcodec_parameters_from_context(stream->codecpar, enc_ctx);

    av_dump_format(fmt_ctx, 0, url, 1);

    ret = avio_open(&fmt_ctx->pb, url, AVIO_FLAG_WRITE);
    if (ret != 0) {
        log_err("avio_open failed");
        return -1;
    }

    ret = avformat_write_header(fmt_ctx, NULL);
    if (ret != 0) {
        log_err("avformat_write_header failed");
        avio_close(fmt_ctx->pb);
        return -1;
    }

    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) {
        log_err("av_packet_alloc failed");
        return -1;
    }

    memset(enc_pkt, 0, sizeof(AVPacket));

    int apts = 0;
    // struct timeval start, end;
    // gettimeofday(&start, NULL);

    while (1)
    {
        ret = snd_pcm_readi(handle, buffer, frames);
        if (ret == -EPIPE) {
            ret = snd_pcm_prepare(handle);
            if (ret < 0) {
                log_err("snd_pcm_prepare failed");
                return -1;
            }
        } else if (ret < 0) {
            log_err("snd_pcm_readi failed, error(%s)",snd_strerror(ret));
            return -1;
        } else if (ret != (int)frames) {
            log_err("short read, read %d frames\n", ret);
        }

#ifdef DUMP_PCM_FILE
        fwrite(buffer, frames, sizeof(short), fp_pcm);
#endif

        memcpy(pcm_frame->data[0], buffer, size);

        ret = swr_convert(swr_ctx, aac_frame->data, aac_frame->nb_samples, 
                          (const uint8_t **)pcm_frame->data, pcm_frame->nb_samples);
        if (ret < 0) {
            log_err("swr_convert failed, error(%s)", av_err2str(ret));
            break;
        }

#ifdef DUMP_SWS_FILE
        int dst_linesize;
        int dst_nb_channels = 1;
        int dst_sample_fmt = AV_SAMPLE_FMT_S16;
        int dst_bufsize = av_samples_get_buffer_size(&dst_linesize, dst_nb_channels,
                                         ret, dst_sample_fmt, 1);
        fwrite(aac_frame->data[0], 1, dst_bufsize, fp_sws);
#endif

        aac_frame->pts = apts;
        apts += aac_frame->nb_samples;

        ret = encode_process(fmt_ctx, enc_ctx, aac_frame, enc_pkt, stream, fp_aac);
        if (ret == ENC_FRAME_ERR_EOF) {
            break;
        } else if (ret < 0) {
            break;
        }

        // gettimeofday(&end, NULL);
        // log_info("%ld", end.tv_sec - start.tv_sec);
    }

#ifdef DUMP_PCM_FILE
    fclose(fp_pcm);
#endif

#ifdef DUMP_SWS_FILE
    fclose(fp_sws);
#endif

#ifdef DUMP_AAC_FILE
    fclose(fp_aac);
#endif

    av_packet_free(&enc_pkt);

    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    free(buffer);

    return 0;
}

