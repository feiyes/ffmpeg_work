#include <pthread.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <sys/time.h>
#include <alsa/asoundlib.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <linux/videodev2.h>

#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include "log.h"

#define CHANNELS 2
#define FSIZE 2*CHANNELS

AVFormatContext* outfmt_ctx = NULL;
int video_ready=0,audio_ready=0;
int semid=-1;
union semun sem_val;

union semun{
    int              val;    /* Value for SETVAL */
    struct semid_ds *buf;    /* Buffer for IPC_STAT, IPC_SET */
    unsigned short  *array;  /* Array for GETALL, SETALL */
    struct seminfo  *__buf;  /* Buffer for IPC_INFO(Linux-specific) */
};

int sem_p(int semid)
{
    int ret = -1;
    struct sembuf buf;

    buf.sem_num=0;
    buf.sem_op=-1;
    buf.sem_flg=SEM_UNDO;
    ret = semop(semid,&buf,1);
    if (ret == -1) {
        log_err("semop failed");
        return -1;
    }

    return 0;
}

int sem_v(int semid)
{
    int ret = -1;
    struct sembuf buf;

    buf.sem_num=0;
    buf.sem_op=+1;
    buf.sem_flg=SEM_UNDO;
    ret = semop(semid,&buf,1);
    if (ret == -1) {
        log_err("semop failed");
        return -1;
    }

    return 0;
}

static int pack_process(AVFormatContext* fmt_ctx, AVCodecContext *enc_ctx, AVPacket *packet, AVStream *stream)
{
    int ret = -1;

    packet->pts = av_rescale_q(packet->pts, enc_ctx->time_base, stream->time_base);
    packet->dts = av_rescale_q(packet->dts, enc_ctx->time_base, stream->time_base);
    packet->duration = av_rescale_q(packet->duration, enc_ctx->time_base, stream->time_base);
    packet->stream_index = 1;

    ret = av_interleaved_write_frame(fmt_ctx, packet);
    if (ret < 0) {
        log_err("av_interleaved_write_frame failed, error(%s)", av_err2str(ret));
    }

    return 0;
}

static int encode_audio_process(AVFormatContext* fmt_ctx, AVCodecContext *ctx, AVFrame *frame, AVPacket *packet, AVStream *stream)
{
    int ret = -1;

    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        log_err("avcodec_send_frame failed, error(%s)", av_err2str(ret));
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        } else if (ret < 0) {
            log_err("avcodec_receive_packet failed, error(%s)", av_err2str(ret));
            return -1;
        }

        pack_process(fmt_ctx, ctx, packet, stream);

        av_packet_unref(packet);
    }

    return 0;
}

void *thread_func(void *arg)
{
    int ret=0;
    snd_pcm_t *handle;
    ret = snd_pcm_open(&handle, "default", SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0) {
        log_err("snd_pcm_open failed");
        return NULL;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_malloc(&params);
    ret = snd_pcm_hw_params_any(handle, params);
    ret = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_access failed");
        return NULL;
    }

    ret = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_format failed");
        return NULL;
    }

    ret = snd_pcm_hw_params_set_channels(handle, params, CHANNELS);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_channels failed");
        return NULL;
    }

    int dir;
    unsigned int val=44100;
    ret = snd_pcm_hw_params_set_rate_near(handle, params, &val, &dir);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_rate_near failed");
        return NULL;
    }

    unsigned int buffer_time,period_time;
    snd_pcm_hw_params_get_buffer_time_max(params, &buffer_time, 0);
    if ( buffer_time >500000)
        buffer_time = 500000;

    ret = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, 0);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_buffer_time_near failed");
        return NULL;
    }

    //设置周期时间，设置为37帧/s，1/37=0.023219
    period_time = 23219;
    ret = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, 0);
    if (ret < 0) {
        log_err("snd_pcm_hw_params_set_period_time_near failed");
        return NULL;
    }

    ret = snd_pcm_hw_params(handle, params);
    if (ret < 0) {
        log_err("snd_pcm_hw_params failed");
        return NULL;
    }

    snd_pcm_uframes_t frames;
    snd_pcm_hw_params_get_period_size(params, &frames, &dir);
    log_info("period_size:%ld", frames);
    int size;
    // 1 frame = channels * sample_size.
    size = frames * FSIZE; /* 2 bytes/sample, 1 channels */
    log_info("size:%d", size);
    char* buffer = (char *)malloc(size);

    AVFrame *pcm_frame;
    pcm_frame = av_frame_alloc();

    pcm_frame->format = AV_SAMPLE_FMT_S16;
    pcm_frame->sample_rate = 44100;
    pcm_frame->nb_samples = frames;
    av_channel_layout_copy(&pcm_frame->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO);
    av_frame_get_buffer(pcm_frame, 0);

    AVFrame *aac_frame;
    aac_frame = av_frame_alloc();

    aac_frame->format = AV_SAMPLE_FMT_S16;
    aac_frame->sample_rate = 44100;
    aac_frame->nb_samples = 1024;
    av_channel_layout_copy(&aac_frame->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO);
    av_frame_get_buffer(aac_frame, 0);

    struct SwrContext *swr_ctx = swr_alloc();
    if (!swr_ctx) {
        log_err("swr_alloc failed");
        return NULL;
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
        return NULL;
    }

    const AVCodec *encodec = avcodec_find_encoder_by_name("libfdk_aac");
    if (!encodec) {
        log_err("avcodec_find_encoder_by_name failed");
        return NULL;
    }

    AVCodecContext* enc_ctx = avcodec_alloc_context3(encodec);
    if (!enc_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return NULL;
    }

    enc_ctx->codec_id = encodec->id;
    enc_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
    enc_ctx->sample_fmt  = AV_SAMPLE_FMT_S16;
    enc_ctx->bit_rate    = 64000;
    enc_ctx->sample_rate = 44100;
    av_channel_layout_copy(&enc_ctx->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO);

    ret = avcodec_open2(enc_ctx, encodec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return NULL;
    }

    char *out_name="rtmp://127.0.0.1/live/stream";

    AVStream *out_stream = NULL;
    out_stream = avformat_new_stream(outfmt_ctx, NULL);
    if (!out_stream) {
        log_err("avformat_new_stream failed");
        return NULL;
    }

    avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);

    av_dump_format(outfmt_ctx, 0, out_name, 1);

    AVPacket *enc_pkt = av_packet_alloc();

    int vpts=0;
    struct timeval start, end;
    gettimeofday(&start, NULL);

    audio_ready = 1;
    while (!video_ready)
    {

    }

    while (1)
    {
        ret = snd_pcm_readi(handle, buffer, frames);
        if (ret == -EPIPE) {
            ret = snd_pcm_prepare(handle);
            if (ret <0) {
                log_err("snd_pcm_prepare failed");
                return NULL;
            }
        } else if (ret < 0) {
            log_err("snd_pcm_readi failed, error(%s)", snd_strerror(ret));
            return NULL;
        } else if (ret != (int)frames) {
            log_err("snd_pcm_readi failed, size = %d", ret);
        }

        memcpy(pcm_frame->data[0], buffer, size);

        ret = swr_convert(swr_ctx, aac_frame->data, aac_frame->nb_samples,
                          (const uint8_t **)pcm_frame->data, pcm_frame->nb_samples);

        encode_audio_process(outfmt_ctx, enc_ctx, aac_frame, enc_pkt, out_stream);

        ret = sem_p(semid);
        if (ret == -1) {
            log_err("sem_p failed");
            break;
        }

        ret = sem_v(semid);
        if (ret == -1) {
            log_err("sem_v failed");
            break;
        }

        aac_frame->pts = vpts;
        vpts+=aac_frame->nb_samples;

        gettimeofday(&end, NULL);
        printf("%ld", end.tv_sec-start.tv_sec);
        printf("\r\033[k");
        fflush(stdout);
    }

    log_info("audio off\n");
    av_packet_free(&enc_pkt);
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    free(buffer);

    return 0;
}

static void encode_video_process(struct SwsContext *sws_ctx, AVCodecContext* enc_ctx, AVFrame* dec_frame, AVFrame *yuv_frame, AVPacket *enc_pkt, AVStream *out_stream)
{
    int ret;

    ret = sws_scale(sws_ctx, (const uint8_t* const*)dec_frame->data,
                    dec_frame->linesize, 0, enc_ctx->height, yuv_frame->data, yuv_frame->linesize);
    printf("wangf, FILE = %s, FUNCTION = %s, LINE = %d %d %d %d %d %d %d %d %d %d\n", __FILE__, __func__, __LINE__,
            dec_frame->linesize[0], dec_frame->linesize[1],dec_frame->linesize[2],dec_frame->linesize[3], enc_ctx->height,
            yuv_frame->linesize[0], yuv_frame->linesize[1],yuv_frame->linesize[2],yuv_frame->linesize[3]);
    if (ret <= 0) {
        log_err("sws_scale failed, error(%s)", av_err2str(ret));
    }

    ret = avcodec_send_frame(enc_ctx, yuv_frame);
    if (ret != 0) {
        log_err("avcodec_send_frame faild, error(%s)", av_err2str(ret));
    }

    ret = avcodec_receive_packet(enc_ctx, enc_pkt);
    if (ret != 0 || enc_pkt->size > 0) {
        log_err("avcodec_receive_packet faild, error(%s)", av_err2str(ret));
    } else {
    }

    enc_pkt->pts = av_rescale_q(enc_pkt->pts, enc_ctx->time_base, out_stream->time_base);
    enc_pkt->dts = av_rescale_q(enc_pkt->dts, enc_ctx->time_base, out_stream->time_base);
    enc_pkt->duration = av_rescale_q(enc_pkt->duration, enc_ctx->time_base, out_stream->time_base);

    ret = av_interleaved_write_frame(outfmt_ctx, enc_pkt);
    if (ret < 0) {
        log_err("av_interleaved_write_frame failed, error(%s)", av_err2str(ret));
    }

    av_packet_unref(enc_pkt);

}

void decode_process(AVCodecContext* codec_ctx, struct SwsContext *sws_ctx, AVCodecContext* enc_ctx,
                    AVPacket* packet, AVPacket *enc_pkt, AVFrame* frame, AVFrame *yuv_frame, AVStream *out_stream)
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

        encode_video_process(sws_ctx, enc_ctx, frame, yuv_frame, enc_pkt, out_stream);
    }
}

int main()
{
    int ret = -1;
    avformat_network_init();
    avdevice_register_all();
    AVFormatContext* ifmt_ctx = NULL;

    av_log_set_level(AV_LOG_INFO);

#if 0
    const AVInputFormat* ifmt = av_find_input_format("v4l2");

    char *in_filename  = "/dev/video0";

    char *out_filename = "rtmp://127.0.0.1/live/stream";

    ifmt_ctx = avformat_alloc_context();
    AVDictionary* opt = NULL;
    //av_dict_set(&opt, "list_formats", "3", 0);
    av_dict_set(&opt, "video_size", "640x480", 0);
    av_dict_set(&opt, "input_format", "mjpeg", 0);
    ret = avformat_open_input(&ifmt_ctx, in_filename, ifmt, &opt);
    if (ret < 0) {
        log_err("avformat_open_input failed, error(%s)", av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info failed, error(%s)", av_err2str(ret));
        avformat_close_input(&ifmt_ctx);
        return -1;
    }

    av_dict_free(&opt);

    int videoindex=-1;

    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoindex = i;
        } else {
            break;
        }
    }

    if (videoindex == -1) {
        log_err("input video stream not exist\n");
        return -1;
    }

    const AVCodec* encodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encodec) {
        log_err("avcodec_find_encoder failed");
        avformat_close_input(&ifmt_ctx);
        return -1;
    }

    const AVCodec* dec_codec = avcodec_find_decoder(ifmt_ctx->streams[videoindex]->codecpar->codec_id);
    if (!dec_codec) {
        log_err("avcodec_find_decoder failed");
        avformat_close_input(&ifmt_ctx);
        return -1;
    }

    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec_codec);
    if (!dec_ctx) {
        log_err("avcodec_alloc_context3 failed\n");
        return -1;
    }

    AVCodecContext* enc_ctx = avcodec_alloc_context3(encodec);
    if (!enc_ctx) {
        log_err("avcodec_alloc_context3 failed\n");
        avformat_close_input(&ifmt_ctx);
        return -1;
    }

    ret = avcodec_parameters_to_context(dec_ctx, ifmt_ctx->streams[videoindex]->codecpar);
    if (ret < 0) {
        log_err("avcodec_parameters_to_context failed");
        return -1;
    }

    ret = avcodec_open2(dec_ctx, dec_codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, codec(%s), error(%s)", dec_codec->name, av_err2str(ret));
        return -1;
    }

    enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    enc_ctx->codec_id = encodec->id;
    enc_ctx->bit_rate = 400000;
    enc_ctx->width = 640;
    enc_ctx->height = 480;
    enc_ctx->time_base = (AVRational) {1, 30};    //5是编多少帧就发送，可根据编码速度改变
    enc_ctx->framerate = (AVRational) {30, 1};
    enc_ctx->gop_size = 15;
    enc_ctx->max_b_frames = 0;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    av_opt_set(enc_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(enc_ctx->priv_data, "tune", "zerolatency", 0);
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "profile", "baseline", 0);
    //av_opt_set(enc_ctx->priv_data, "crf", "18", 0);

    ret = avcodec_open2(enc_ctx, encodec, &opts);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return -1;
    }

    ret = avformat_alloc_output_context2(&outfmt_ctx, NULL, "flv", out_filename);
    if (ret != 0) {
        log_err("avformat_alloc_output_context2 failed, error(%s)", av_err2str(ret));
        avformat_close_input(&ifmt_ctx);
        return -1;
    }

    AVStream *out_stream = avformat_new_stream(outfmt_ctx, NULL);
    if (!out_stream) {
        log_err("avformat_new_stream failed");
        avformat_close_input(&ifmt_ctx);
        avformat_close_input(&outfmt_ctx);
        return -1;
    }

    out_stream->codecpar->codec_tag = 0;
    avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);

    av_dump_format(outfmt_ctx, 0, out_filename, 1);
#endif

    pthread_t tid;
    ret = pthread_create(&tid, NULL, thread_func, NULL);
    if (ret < 0) {
        log_err("pthread_create failed");
        return -1;
    }

#if 0
    //等待音频推流初始化
    while (!audio_ready)
    {

    }

    ret = avio_open(&outfmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
    if (ret != 0) {
        log_err("avio_open failed, error(%s)", av_err2str(ret));
        avformat_close_input(&ifmt_ctx);
        avformat_close_input(&outfmt_ctx);
        return -1;
    }

    ret = avformat_write_header(outfmt_ctx, NULL);
    if (ret != 0) {
        log_err("avformat_write_header failed, error(%s)", av_err2str(ret));
        avio_close(outfmt_ctx->pb);
        avformat_close_input(&ifmt_ctx);
        avformat_close_input(&outfmt_ctx);
        return -1;
    }

    AVPacket *dec_pkt = av_packet_alloc();
    AVPacket *enc_pkt = av_packet_alloc();

    log_info("decoder info: width %d height %d", dec_ctx->width, dec_ctx->height);
    log_info("encoder info: width %d height %d", enc_ctx->width, enc_ctx->height);
    struct SwsContext *sws_ctx = NULL;
    sws_ctx = sws_getCachedContext(sws_ctx, dec_ctx->width, dec_ctx->height,
                                   dec_ctx->pix_fmt, enc_ctx->width, enc_ctx->height,
                                   AV_PIX_FMT_YUV420P, SWS_BICUBIC, 0, 0, 0);
    if (!sws_ctx) {
        log_err("sws_getCachedContext failed");
        return -1;
    }

    AVFrame *yuv_frame;
    AVFrame *dec_frame;
    dec_frame = av_frame_alloc();
    yuv_frame = av_frame_alloc();

    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = 640;
    yuv_frame->height = 480;
    yuv_frame->pts = 0;

    ret = av_frame_get_buffer(yuv_frame, 1);
    if (ret != 0) {
        log_err("av_frame_get_buffer failed, error(%s)", av_err2str(ret));
        return -1;
    }

    int vpts = 0;

    semid = semget(1234, 1, 0666 | IPC_CREAT);
    if (semid == -1) {
        log_err("create sem failed");
        return -1;
    }

    sem_val.val = 1;
    ret = semctl(semid, 0, SETVAL, sem_val);
    if (ret == -1) {
        log_err("semctl failed");
        return -1;
    }

    video_ready = 1;

    while (1)
    {
        yuv_frame->pts = vpts;
        vpts += 1;
        ret = av_read_frame(ifmt_ctx, dec_pkt);
        if (ret != 0) {
            log_err("av_read_frame failed, error(%s)", av_err2str(ret));
            break;
        }

        decode_process(dec_ctx, sws_ctx, enc_ctx, dec_pkt, enc_pkt, dec_frame, yuv_frame, out_stream);

        ret = sem_p(semid);
        if (ret == -1) {
            log_err("sem_p failed");
            break;
        }

        ret = sem_v(semid);
        if (ret == -1) {
            log_err("sem_v failed");
            break;
        }
    }

    avio_close(outfmt_ctx->pb);
    avformat_close_input(&ifmt_ctx);
    avformat_close_input(&outfmt_ctx);
#endif

    while (1) {
        sleep(1);
    }

    return 0;
}

