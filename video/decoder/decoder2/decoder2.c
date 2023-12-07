#include <getopt.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>

#include "log.h"

/**
 * save yuv420p frame [YUV]
 */
void yuv420p_save(FILE* fp, AVFrame *frame, AVCodecContext *codec_ctx)
{
    int i = 0;

    int width = codec_ctx->width;
    int height = codec_ctx->height;
    int width_half = width / 2;
    int height_half = height / 2;
    int y_wrap = frame->linesize[0];
    int u_wrap = frame->linesize[1];
    int v_wrap = frame->linesize[2];

    unsigned char *y_buf = frame->data[0];
    unsigned char *u_buf = frame->data[1];
    unsigned char *v_buf = frame->data[2];

    // save y
    fprintf(stderr, "=====save Y success=====\n");
    for (i = 0; i < height; i++)
        fwrite(y_buf + i * y_wrap, 1, width, fp);

    // save u
    fprintf(stderr, "=====save U success=====\n");
    for (i = 0; i < height_half; i++)
        fwrite(u_buf + i * u_wrap, 1, width_half, fp);

    // save v
    fprintf(stderr, "=====save V success=====\n");
    for (i = 0; i < height_half; i++)
        fwrite(v_buf + i * v_wrap, 1, width_half, fp);

    fflush(fp);
}

/**
 * save yuv422p frame [YUV]
 */
void yuv422p_save(FILE* fp, AVFrame *frame, AVCodecContext *codec_ctx)
{
    int i = 0;

    int width = codec_ctx->width;
    int height = codec_ctx->height;
    int width_half = width / 2;
    int y_wrap = frame->linesize[0];
    int u_wrap = frame->linesize[1];
    int v_wrap = frame->linesize[2];

    unsigned char *y_buf = frame->data[0];
    unsigned char *u_buf = frame->data[1];
    unsigned char *v_buf = frame->data[2];

    // save y
    fprintf(stderr, "=====save Y success=====\n");
    for (i = 0; i < height; i++)
        fwrite(y_buf + i * y_wrap, 1, width, fp);

    // save u
    fprintf(stderr, "=====save U success=====\n");
    for (i = 0; i < height; i++)
        fwrite(u_buf + i * u_wrap, 1, width_half, fp);

    // save v
    fprintf(stderr, "=====save V success=====\n");
    for (i = 0; i < height; i++)
        fwrite(v_buf + i * v_wrap, 1, width_half, fp);

    fflush(fp);
}

/**
 * save yuv444p frame [YUV]
 */
void yuv444p_save(FILE* fp, AVFrame *frame, AVCodecContext *codec_ctx)
{
    int i = 0;

    int width = codec_ctx->width;
    int height = codec_ctx->height;
    int y_wrap = frame->linesize[0];
    int u_wrap = frame->linesize[1];
    int v_wrap = frame->linesize[2];

    unsigned char *y_buf = frame->data[0];
    unsigned char *u_buf = frame->data[1];
    unsigned char *v_buf = frame->data[2];

    // save y
    fprintf(stderr, "=====save Y success=====\n");
    for (i = 0; i < height; i++)
        fwrite(y_buf + i * y_wrap, 1, width, fp);

    // save u
    fprintf(stderr, "=====save U success=====\n");
    for (i = 0; i < height; i++)
        fwrite(u_buf + i * u_wrap, 1, width, fp);

    // save v
    fprintf(stderr, "=====save V success=====\n");
    for (i = 0; i < height; i++)
        fwrite(v_buf + i * v_wrap, 1, width, fp);

    fflush(fp);
}

/**
 * save rgb24 frame [PPM]
 */
void rgb24_save(FILE* fp, AVFrame *frame, AVCodecContext *codec_ctx)
{
    int i = 0;
    int width = codec_ctx->width;
    int height = codec_ctx->height;

    // write PPM header
    fprintf(fp, "P6n%d %dn255\n", width, height);

    // write pixel data
    for(i = 0; i < height; i++)
        fwrite(frame->data[0] + i * frame->linesize[0], 1, width * 3, fp);

    fflush(fp);
}

static void decode_process(AVCodecContext* codec_ctx, AVFrame* frame, AVPacket* packet, FILE* fp, enum AVMediaType type)
{
    int ret = -1;

    ret = avcodec_send_packet(codec_ctx, packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
            return;
        }

        if (type == AVMEDIA_TYPE_VIDEO) {
            log_info("receive video frame %d, pts = %s dts = %s",
                      codec_ctx->frame_number,
                      av_ts2timestr(frame->pts, &codec_ctx->time_base),
                      av_ts2timestr(frame->pkt_dts, &codec_ctx->time_base));

            if (!fp) continue;

            switch (frame->format) {
                case AV_PIX_FMT_YUV420P:
                case AV_PIX_FMT_YUVJ420P:
                    yuv420p_save(fp, frame, codec_ctx);
                    break;
                case AV_PIX_FMT_RGB24:
                    rgb24_save(fp, frame, codec_ctx);
                    break;
                case AV_PIX_FMT_YUV422P:
                case AV_PIX_FMT_YUVJ422P:
                    yuv422p_save(fp, frame, codec_ctx);
                    break;
                case AV_PIX_FMT_YUV444P:
                case AV_PIX_FMT_YUVJ444P:
                    yuv444p_save(fp, frame, codec_ctx);
                    break;
                default :
                    log_err("unsupport YUV format %d\n", frame->format);
                    break;
            }
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            log_info("receive audio frame %d, pts = %s",
                      codec_ctx->frame_number,
                      av_ts2timestr(frame->pts, &codec_ctx->time_base));

            if (!fp) continue;

            int data_size = av_get_bytes_per_sample(codec_ctx->sample_fmt);
            if (data_size < 0) {
                log_err("av_get_bytes_per_sample failed\n");
                return;
            }

            for (int i = 0; i < frame->nb_samples; i++) {
                for (int ch = 0; ch < codec_ctx->ch_layout.nb_channels; ch++) {
                    fwrite(frame->data[ch] + data_size * i, 1, data_size, fp);
                }
            }
        }
    }
}

static struct option long_options[] = {
    {"input",         required_argument, NULL, 'i'},
    {"output",        required_argument, NULL, 'o'},
    {NULL,            0,                 NULL,   0}
};

int main(int argc, char* argv[])
{
    int c = 0;
    int ret = -1;
    int video_index = -1;
    int audio_index = -1;
    FILE* fp_vout = NULL;
    FILE* fp_aout = NULL;
    AVFrame* frame = NULL;
    AVPacket* packet = NULL;
    char out_file[64] = {'\0'};
    char audio_file[80] = {'\0'};
    char video_file[80] = {'\0'};
    char stream_file[80] = {'\0'};
    AVFormatContext* fmt_ctx = NULL;
    AVCodecContext* video_ctx = NULL;
    AVCodecContext* audio_ctx = NULL;

    while ((c = getopt_long(argc, argv, ":i:o:a", long_options, NULL)) != -1) {
        switch (c) {
        case 'i':
            strncpy(stream_file, optarg, strlen(optarg));
            break;
        case 'o':
            strncpy(out_file, optarg, strlen(optarg));
            break;
        default:
            return -1;
        }
    }

    if (strcmp(stream_file, "\0") == 0) {
        log_err("invalid stream %s file\n", stream_file);
        return -1;
    }

    if (strcmp(out_file, "\0") == 0) {
        sprintf(video_file, "%s.%s", out_file, "yuv");
        fp_vout = fopen(video_file, "wb+");
        if (!fp_vout) {
            log_err("fopen %s failed\n", video_file);
            return -1;
        }

        sprintf(audio_file, "%s.%s", out_file, "pcm");
        fp_aout = fopen(audio_file, "wb+");
        if (!fp_aout) {
            log_err("fopen %s failed\n", audio_file);
            return -1;
        }
    }

    ret = avformat_open_input(&fmt_ctx, stream_file, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input %s failed, error(%s)", stream_file, av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info %s failed, error(%s)", stream_file, av_err2str(ret));
        return -1;
    }

    av_dump_format(fmt_ctx, 0, stream_file, 0);

    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* stream = fmt_ctx->streams[i];
        AVCodecParameters* codecpar = stream->codecpar;

        if (codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }

        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            log_err("avcodec_find_decoder %s failed\n", avcodec_get_name(codecpar->codec_id));
            return -1;
        }

        AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            log_err("avcodec_alloc_context3 failed\n");
            return -1;
        }

        ret = avcodec_parameters_to_context(codec_ctx, codecpar);
        if (ret < 0) {
            log_err("avcodec_parameters_to_context failed");
            return -1;
        }

        ret = avcodec_open2(codec_ctx, codec, NULL);
        if (ret < 0) {
            log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
            return -1;
        }

        codec_ctx->time_base = stream->time_base;

        if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_index = i;
            audio_ctx = codec_ctx;
            log_info("audio codec %s", avcodec_get_name(codecpar->codec_id));
        } else if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_index = i;
            video_ctx = codec_ctx;
            log_info("video codec %s, delay %d", avcodec_get_name(codecpar->codec_id), codecpar->video_delay);
        }
    }

    if ((video_index == -1) && (audio_index == -1)) {
        log_err("video or audio stream index invalid\n");
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

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_index) {
            log_info("send video packet, pts = (%.2f %s) dts = %s",
                      packet->pts * av_q2d(video_ctx->time_base),
                      av_ts2timestr(packet->pts, &video_ctx->time_base),
                      av_ts2timestr(packet->dts, &video_ctx->time_base));
            decode_process(video_ctx, frame, packet, fp_vout, AVMEDIA_TYPE_VIDEO);
        } else if (packet->stream_index == audio_index) {
            log_info("send audio packet, pts = %s",
                      av_ts2timestr(packet->pts, &audio_ctx->time_base));
            decode_process(audio_ctx, frame, packet, fp_aout, AVMEDIA_TYPE_AUDIO);
        }

        av_packet_unref(packet);
    }

    if (video_ctx) decode_process(video_ctx, frame, NULL, fp_vout, AVMEDIA_TYPE_VIDEO);
    if (audio_ctx) decode_process(audio_ctx, frame, NULL, fp_aout, AVMEDIA_TYPE_AUDIO);

    log_info("demuxing completed");

    if (fp_vout) fclose(fp_vout);
    if (fp_aout) fclose(fp_aout);

    if (video_ctx) avcodec_free_context(&video_ctx);
    if (audio_ctx) avcodec_free_context(&audio_ctx);

    av_frame_free(&frame);
    av_packet_free(&packet);

    return 0;
}
