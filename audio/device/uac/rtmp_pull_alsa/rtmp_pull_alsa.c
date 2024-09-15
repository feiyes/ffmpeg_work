#include <alsa/asoundlib.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include "log.h"

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define FSIZE 2*CHANNELS

void decode_process(AVCodecContext* codec_ctx, AVPacket* packet, AVFrame* frame)
{
    int ret;

    ret = avcodec_send_packet(codec_ctx, packet);
    if (ret < 0) {
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            continue;
        else if (ret < 0) {
            continue;
        }
    }
}

int main(int argc, char *argv[])
{
    int fd;
    int ret;
    int size;
    int dir;
    char *buffer;
    char *inFile;
    unsigned int val;
    snd_pcm_uframes_t frames;

    inFile = "output.raw";

    fd = open(inFile,O_RDONLY);

    snd_pcm_t *handle;
    ret = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        log_err("snd_pcm_open %s failed", snd_strerror(ret));
        return -1;
    }

    snd_pcm_hw_params_t *params;

    snd_pcm_hw_params_malloc(&params);

    ret = snd_pcm_hw_params_any(handle, params);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_any failed, error(%s)", snd_strerror(ret));
        return -1;
    }

    ret = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_access failed, error(%s)", snd_strerror(ret));
        return -1;
    }

    ret = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_format failed, error(%s)", snd_strerror(ret));
        return -1;
    }

    ret = snd_pcm_hw_params_set_channels(handle, params, CHANNELS);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_channels failed, error(%s)", snd_strerror(ret));
        return -1;
    }

    val = SAMPLE_RATE;
    ret = snd_pcm_hw_params_set_rate_near(handle, params, &val, &dir);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_rate_near failed, error(%s)", snd_strerror(ret));
        return -1;
    }

    unsigned int buffer_time,period_time;
    snd_pcm_hw_params_get_buffer_time_max(params, &buffer_time, 0);
    if ( buffer_time >500000)
        buffer_time = 500000;

    ret = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, 0);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_buffer_time_near failed, error(%s)", snd_strerror(ret));
        return -1;
    }

    period_time = 26315;
    ret = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, 0);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_period_time_near failed, error(%s)", snd_strerror(ret));
        return -1;
    }

    ret = snd_pcm_hw_params(handle, params);
    if (ret < 0) {
        log_err("snd_pcm_hw_params failed, error(%s)", snd_strerror(ret));
        return -1;
    }

    snd_pcm_hw_params_get_period_size(params, &frames, &dir);
    // 1 frame = channels * sample_size.
    size = frames * FSIZE; /* 2 bytes/sample, 1 channels */
    buffer = (char *) malloc(size);

    char *in_name = "rtmp://127.0.0.1/live/stream";

    AVFormatContext* fmt_ctx = NULL;
    ret = avformat_open_input(&fmt_ctx, in_name, NULL, NULL);
    if (ret != 0) {
        log_err("avformat_open_input failed");
        return -1;
    }

    avformat_find_stream_info(fmt_ctx, NULL);

    int audio_index = -1;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_index = i;
            break;
        }
    }

    if (audio_index == -1) {
        log_err("input video stream not exist\n");
        return -1;
    }

    const AVCodec* codec = avcodec_find_decoder(fmt_ctx->streams[audio_index]->codecpar->codec_id);
    if (!codec) {
        log_err("avcodec_find_decoder failed");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVCodecContext* decodec_ctx = avcodec_alloc_context3(codec);
    if (!decodec_ctx) {
        log_err("avcodec_alloc_context3 failed\n");
        return -1;
    }

    ret = avcodec_open2(decodec_ctx, codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return -1;
    }

    av_dump_format(fmt_ctx, 0, in_name,0);

    AVPacket *dec_pkt = av_packet_alloc();

    AVFrame *pcm_frame;
    pcm_frame = av_frame_alloc();

    pcm_frame->format = AV_SAMPLE_FMT_S16;
    pcm_frame->sample_rate = 48000;
    pcm_frame->nb_samples = frames;
    av_channel_layout_copy(&pcm_frame->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
    av_frame_get_buffer(pcm_frame, 0);

    AVFrame *aac_frame;
    aac_frame = av_frame_alloc();

    aac_frame->format = AV_SAMPLE_FMT_FLTP;
    aac_frame->sample_rate = 44100;
    aac_frame->nb_samples = 1024;
    av_channel_layout_copy(&aac_frame->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
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
        log_err("swr_init failed, error(%s)", av_err2str(ret));
        return -1;
    }

    while (1) {
        ret = av_read_frame(fmt_ctx,dec_pkt);
        if (ret != 0) {
            log_err("av_read_frame failed, error(%s)", av_err2str(ret));
            break;
        }

        decode_process(decodec_ctx, dec_pkt, aac_frame);

        ret = swr_convert(swr_ctx, pcm_frame->data, pcm_frame->nb_samples, 
                          (const uint8_t **)aac_frame->data, aac_frame->nb_samples);
        if (ret <= 0) {
            log_err("swr_convert, error(%s)", av_err2str(ret));
            continue;
        }

        memcpy(buffer, pcm_frame->data[0], size);

        ret = snd_pcm_writei(handle, buffer, frames);
        if (ret == -EPIPE) {
            ret = snd_pcm_prepare(handle);
            if (ret < 0) {
                log_err("snd_pcm_prepare failed, error(%s)", snd_strerror(ret));
            }
        } else if (ret < 0) {
            log_err("snd_pcm_writei failed, error(%s)", snd_strerror(ret));
        } else if (ret != (int)frames) {
            log_err("snd_pcm_writei failed");
        }
    }

    av_packet_free(&dec_pkt);
    avformat_close_input(&fmt_ctx);
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    free(buffer);
    close(fd);

    return 0;
}
