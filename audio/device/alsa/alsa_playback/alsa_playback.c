#include <alsa/asoundlib.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswresample/swresample.h>

#include "log.h"

#define SWS_DEBUG 1

static snd_pcm_format_t format = SND_PCM_FORMAT_S16;    /* sample format */
static unsigned int rate = 44100;            /* stream rate */
static unsigned int channels = 2;            /* count of channels */
static unsigned int buffer_time = 500000;        /* ring buffer length in us */
static unsigned int period_time = 100000;        /* period time in us */
static int resample = 1;                /* enable alsa-lib resampling */
static snd_pcm_sframes_t buffer_size;
static snd_pcm_uframes_t period_size;

static int set_hwparams(snd_pcm_t *handle)
{
    int err, dir;
    snd_pcm_uframes_t size;
    snd_pcm_hw_params_t *params;

    err = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        log_err("snd_pcm_open failed, error(%s)", snd_strerror(err));
        return 0;
    }

    snd_pcm_hw_params_alloca(&params);

    err = snd_pcm_hw_params_any(handle, params);
    if (err < 0) {
        log_err("snd_pcm_hw_params_any failed, error(%s)\n", snd_strerror(err));
        return err;
    }

    err = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_access failed, error(%s)\n", snd_strerror(err));
        return err;
    }

    err = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_format failed, error(%s)\n", snd_strerror(err));
        return err;
    }

    err = snd_pcm_hw_params_set_channels(handle, params, channels);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_channels failed, error(%s)\n", snd_strerror(err));
        return err;
    }

    err = snd_pcm_hw_params_set_rate_near(handle, params, &rate, &dir);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_rate_near failed, rate(%u), error(%s)\n", rate, snd_strerror(err));
        return err;
    }

    //err = snd_pcm_hw_params_set_rate_resample(handle, params, resample);
    //if (err < 0) {
    //    log_err("snd_pcm_hw_params_set_rate_resample failed, error(%s)\n", snd_strerror(err));
    //    return err;
    //}

    //err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, &dir); //us
    //if (err < 0) {
    //    log_err("snd_pcm_hw_params_set_buffer_time_near failed, buffer_time(%dus), error(%s)\n", buffer_time, snd_strerror(err));
    //    return err;
    //}

    //err = snd_pcm_hw_params_get_buffer_size(params, &size);
    //if (err < 0) {
    //    log_err("snd_pcm_hw_params_get_buffer_size failed, error(%s)\n", snd_strerror(err));
    //    return err;
    //}

    //buffer_size = size;
    //log_info("buffer_size=%ld",buffer_size);

    //err = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, &dir);
    //if (err < 0) {
    //    log_err("snd_pcm_hw_params_set_period_time_near failed, period_time(%dus) error(%s)\n", period_time, snd_strerror(err));
    //    return err;
    //}

    err = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        log_err("snd_pcm_hw_params failed, error(%s)\n", snd_strerror(err));
        return err;
    }

    err = snd_pcm_hw_params_get_period_size(params, &period_size, &dir);
    if (err < 0) {
        log_err("snd_pcm_hw_params_get_period_size failed, error(%s)\n", snd_strerror(err));
        return err;
    }

    log_info("period_size = %ld", period_size);

    return 0;
}

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
    int rc;
    int err;
    int ret;
    int size;
    snd_pcm_t *handle = NULL;
    snd_output_t *log = NULL;

    log_info("Stream parameters are %uHz, %s, %u channels", rate, snd_pcm_format_name(format), channels);

    err = snd_output_stdio_attach(&log, stderr, 0);
    if (err < 0) {
        log_err("snd_output_stdio_attach failed, error(%s)", snd_strerror(err));
        return 0;
    }

    err = set_hwparams(handle);
    if (err < 0) {
        return 0;
    }

    //period_size大概是采样点数/帧——4410点/帧
    //s16位代表两个字节，再加上双声道
    //size公式=period_size*channels*16/8
    size = (period_size * channels * snd_pcm_format_physical_width(format)) / 8; /* 2 bytes/sample, 1 channels */
    log_info("size:%d",size);

    char* buffer = (char *)malloc(size);
    memset(buffer,0,size);

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        log_err("avformat_alloc_context failed\n");
        return -1;
    }

    char *in_name="../../../../stream/audio/aurora.wav";
    ret = avformat_open_input(&fmt_ctx, in_name, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input failed\n");
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
        log_err("input audio stream not exist\n");
        return -1;
    }

    const AVCodecParameters *par = fmt_ctx->streams[audio_index]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        log_err("avcodec_find_decoder %d failed", par->codec_id);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return -1;
    }

    ret = avcodec_parameters_to_context(codec_ctx, par);
    if (ret < 0) {
        log_err("avcodec_parameters_to_context failed");
        return -1;
    }

    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 %s failed, error(%s)", codec->name, av_err2str(ret));
        return -1;
    }

    av_dump_format(fmt_ctx, 0, in_name,0);

    AVPacket *packet = av_packet_alloc();
    AVFrame* audio_frame = av_frame_alloc();

#ifdef SWS_DEBUG
    AVFrame* pcm_frame = av_frame_alloc();

    pcm_frame->format = AV_SAMPLE_FMT_S16;
    pcm_frame->sample_rate = rate;
    pcm_frame->nb_samples = period_size;
    av_channel_layout_copy(&pcm_frame->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);

    av_frame_get_buffer(pcm_frame, 0);

    struct SwrContext *swr_ctx = swr_alloc();
    if (!swr_ctx) {
        log_err("Could not allocate resampler context\n");
        return -1;
    }

    av_opt_set_chlayout  (swr_ctx, "in_chlayout",      &codec_ctx->ch_layout,    0);
    av_opt_set_int       (swr_ctx, "in_sample_rate",    codec_ctx->sample_rate,  0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",     codec_ctx->sample_fmt,   0);
    av_opt_set_chlayout  (swr_ctx, "out_chlayout",     &codec_ctx->ch_layout,           0);
    av_opt_set_int       (swr_ctx, "out_sample_rate",   codec_ctx->sample_rate,         0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt",    AV_SAMPLE_FMT_S16,          0);

    ret = swr_init(swr_ctx);
    if (ret < 0) {
        log_err("swr_init failed\n");
        return -1;
    }
#endif

    while (1)
    {
        ret = av_read_frame(fmt_ctx, packet);
        if (ret < 0) {
            log_err("av_read_frame failed, error(%s)\n", av_err2str(ret));
            break;
        }

        if (packet->stream_index == audio_index) {
            decode_process(codec_ctx, packet, audio_frame);
        }

#ifdef SWS_DEBUG
        ret = swr_convert(swr_ctx, pcm_frame->data, pcm_frame->nb_samples,
                          (const uint8_t **)audio_frame->data, audio_frame->nb_samples);
        if (ret < 0) {
            continue;
        }
#endif

        rc = snd_pcm_writei(handle, pcm_frame->data[0], pcm_frame->nb_samples);
        if (rc == -EPIPE) {
            err = snd_pcm_prepare(handle);
            if (err < 0) {
                log_err("snd_pcm_prepare failed, error(%s)", snd_strerror(err));
            }
        } else if (rc < 0) {
            log_err("error from writei: %s\n", snd_strerror(rc));
        } else if (rc != (int)pcm_frame->nb_samples) {
            log_err("short write, write %d frames\n", rc);
        }
    }

    free(buffer);

    snd_pcm_dump(handle, log);
    snd_output_close(log);
    snd_pcm_drain(handle);
    snd_pcm_close(handle);

    return 0;
}

