#include <pthread.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <sys/time.h>
#include <alsa/asoundlib.h>
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include "log.h"

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

int main()
{
    int ret;

    avformat_network_init();
    avdevice_register_all();

    char *in_name="rtmp://127.0.0.1/live/stream";

    AVFormatContext* infmt_ctx=NULL;
    ret = avformat_open_input(&infmt_ctx, in_name, NULL, NULL);
    if (ret != 0) {
        log_err("avformat_open_input failed, error(%s)", av_err2str(ret));
        return -1;
    }

    avformat_find_stream_info(infmt_ctx, NULL);

    int videoindex = -1, audioindex = -1;
    for (int i = 0; i < infmt_ctx->nb_streams; i++)
    {
        if (infmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoindex=i;
        }
        else if (infmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audioindex=i;
        }
    }

    log_info("videoindex:%d,audioindex:%d\n", videoindex, audioindex);

    if (videoindex == -1)
    {
        log_err("input video stream not exist\n");
        return -1;
    }
    if (audioindex == -1)
    {
        log_err("input audio stream not exist\n");
        return -1;
    }

    const AVCodec* decodec = avcodec_find_decoder(infmt_ctx->streams[videoindex]->codecpar->codec_id);
    if (!decodec) {
        log_err("avcodec_find_decoder failed");
        avformat_close_input(&infmt_ctx);
        return -1;
    }

    AVCodecContext* decodec_ctx = NULL;
    decodec_ctx = avcodec_alloc_context3(decodec);
    if (!decodec_ctx) {
        log_err("avcodec_alloc_context3 failed\n");
        return -1;
    }

    ret = avcodec_open2(decodec_ctx, decodec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return -1;
    }

    const AVCodec* codec = avcodec_find_decoder(infmt_ctx->streams[videoindex]->codecpar->codec_id);
    if (!codec) {
        log_err("avcodec_find_decoder failed");
        avformat_close_input(&infmt_ctx);
        return -1;
    }

    AVCodecContext* dec_ctx = NULL;
    dec_ctx = avcodec_alloc_context3(codec);
    if (!dec_ctx) {
        log_err("avcodec_alloc_context3 failed\n");
        return -1;
    }

    ret = avcodec_open2(dec_ctx, codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return -1;
    }

    av_dump_format(infmt_ctx, 0, in_name,0);

    AVFrame *pFrame,*rgb_frame;
    pFrame = av_frame_alloc();
    rgb_frame = av_frame_alloc();
    rgb_frame->format = AV_PIX_FMT_RGB24;
    rgb_frame->width = 640;
    rgb_frame->height = 480;
    ret = av_frame_get_buffer(rgb_frame, 1);
    if (ret != 0) {
        log_err("av_frame_get_buffer failed, error(%s)", av_err2str(ret));
    }

    struct SwsContext *rgb_convert_ctx  = NULL;
    sws_getCachedContext(rgb_convert_ctx, 
                         decodec_ctx->width, 
                         decodec_ctx->height,
                         decodec_ctx->pix_fmt,
                         640,
                         480, 
                         AV_PIX_FMT_RGB24, 
                         SWS_BICUBIC, 0, 0, 0);
    if (!rgb_convert_ctx) {
        log_err("sws_getCachedContext failed");
    }

    struct SwrContext *swr_ctx = swr_alloc();
    if (!swr_ctx) {
        log_err("swr_alloc failed");
        return -1;
    }

    //swr_alloc_set_opts(pcm_convert_ctx,
    //                   AV_CH_LAYOUT_STEREO,
    //                   AV_SAMPLE_FMT_S16,
    //                   44100,
    //                   AV_CH_LAYOUT_STEREO,
    //                   AV_SAMPLE_FMT_FLTP,
    //                   44100,
    //                   0,
    //                   NULL);

    ret = swr_init(swr_ctx);
    if (ret < 0) {
        log_err("swr_init failed, error(%s)", av_err2str(ret));
        return -1;
    }

    AVPacket *dec_pkt = av_packet_alloc();

    int rc,err,dir;
    unsigned int val;

    snd_pcm_t *handle;
    rc = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        log_err("snd_pcm_open failed, %s", snd_strerror(rc));
        exit(1);
    }

    snd_pcm_hw_params_t *params;

    snd_pcm_hw_params_malloc(&params);

    err = snd_pcm_hw_params_any(handle, params);
    if (err < 0) {
        log_err("snd_pcm_hw_params_any failed %s\n", snd_strerror(err));
        exit(1);
    }

    err = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_access failed %s\n", snd_strerror(err));
        exit(1);
    }

    err = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_format failed %s\n", snd_strerror(err));
        exit(1);
    }

    err = snd_pcm_hw_params_set_channels(handle, params, CHANNELS);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_channels failed %s\n", snd_strerror(err));
        exit(1);
    }

    val = 44100;
    err = snd_pcm_hw_params_set_rate_near(handle, params, &val, &dir);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_rate_near failed %s\n", snd_strerror(err));
        exit(1);
    }

    unsigned int buffer_time,period_time;
    snd_pcm_hw_params_get_buffer_time_max(params, &buffer_time, 0);
    if ( buffer_time >500000)
        buffer_time = 500000;

    err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, 0);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_buffer_time_near failed %s\n", snd_strerror(err));
        exit(1);
    }

    period_time = 50000;
    err = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, 0);
    if (err < 0) {
        log_err("snd_pcm_hw_params_set_period_time_near failed %s\n", snd_strerror(err));
        exit(1);
    }

    rc = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        log_err("snd_pcm_hw_params failed %s\n", snd_strerror(rc));
        exit(1);
    }
    
    snd_pcm_uframes_t frames;
    snd_pcm_hw_params_get_period_size(params, &frames,&dir);
    // 1 frame = channels * sample_size.

    char *buffer;
    int size = frames * FSIZE; /* 2 bytes/sample, 1 channels */

    buffer = (char *) malloc(size);

    AVFrame *pcm_frame;
    pcm_frame = av_frame_alloc();

    pcm_frame->format = AV_SAMPLE_FMT_S16;
    pcm_frame->sample_rate = 44100;
    pcm_frame->nb_samples = frames;
    av_channel_layout_copy(&pcm_frame->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
    av_frame_get_buffer(pcm_frame, 0);
    
    AVFrame *aac_frame;
    aac_frame = av_frame_alloc();

    int samples=0;
    
    while (1)
    {
        ret = av_read_frame(infmt_ctx, dec_pkt);
        if (ret != 0) {
            log_err("av_read_frame failed");
            break;
        }

        if (dec_pkt->stream_index==videoindex)
        {
            decode_process(decodec_ctx, dec_pkt, pFrame);

            ret = sws_scale(rgb_convert_ctx, (const uint8_t* const*)pFrame->data,
                            pFrame->linesize, 0, rgb_frame->height, rgb_frame->data, rgb_frame->linesize);
            if (ret <= 0) {
                log_err("sws_scale failed");
                continue;
            }
            
            //这是qt里面信号操作
            //emit img_signal(rgb_frame);
            
            //图片操作的话需要你们自己写。
        }
        else if (dec_pkt->stream_index==audioindex)
        {
            decode_process(decodec_ctx, dec_pkt, aac_frame);

            samples=swr_convert(swr_ctx, pcm_frame->data, pcm_frame->nb_samples,
                                (const uint8_t **)aac_frame->data, aac_frame->nb_samples);
            if (ret <= 0) {
                log_err("swr_convert failed");
                continue;
            }

            rc = snd_pcm_writei(handle, pcm_frame->data[0], samples);
            if (rc == -EPIPE) {
                err = snd_pcm_prepare(handle);
                if (err < 0) {
                    log_err("snd_pcm_prepare failed %s\n", snd_strerror(err));
                }
            } else if (rc < 0) {
                log_err("snd_pcm_writei failed %s\n", snd_strerror(rc));
            } else if (rc != (int)samples) {
                log_err("snd_pcm_writei failed\n");
            }
        }

        av_packet_free(&dec_pkt);
    }

    avformat_close_input(&infmt_ctx);
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    free(buffer);

    return 0;
}

