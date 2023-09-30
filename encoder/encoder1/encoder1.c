#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>

#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include "log.h"

static int send_frame_count = 0;
static int get_frame_count  = 0;

int64_t get_time_ms()
{
    return av_gettime_relative() / 1000;
}

static int encode_process(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt, FILE *fp_out)
{
    int ret;

    if (frame)
        log_info("send %d frame, pts %3"PRId64, send_frame_count, frame->pts);

    ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0) {
        log_err("avcodec_send_frame, error(%s)\n", av_err2str(ret));
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        } else if (ret < 0) {
            log_err("avcodec_receive_packet, error(%s)\n", av_err2str(ret));
            return -1;
        }

        if (pkt->flags & AV_PKT_FLAG_KEY)
            log_info("receive %d frame(key), pts:%3"PRId64" dts:%3"PRId64" size:%d", get_frame_count, pkt->pts, pkt->dts, pkt->size);

        if (!pkt->flags)
            log_info("receive %d frame(non-key), pts:%3"PRId64" dts:%3"PRId64" size:%d", get_frame_count, pkt->pts, pkt->dts, pkt->size);

        get_frame_count++;
        fwrite(pkt->data, 1, pkt->size, fp_out);
    }

    return 0;
}

static struct option long_options[] = {
    {"input",         required_argument, NULL, 'i'},
    {"output",        required_argument, NULL, 'o'},
    {"fps",           required_argument, NULL, 'f'},
    {"filter",        required_argument, NULL, 'F'},
    {"gop",           required_argument, NULL, 'g'},
    {"codec",         required_argument, NULL, 'c'},
    {"pixel_src",     required_argument, NULL, 'p'},
    {"width_src",     required_argument, NULL, 'w'},
    {"height_src",    required_argument, NULL, 'h'},
    {"pixel_dst",     required_argument, NULL, 'P'},
    {"width_dst",     required_argument, NULL, 'W'},
    {"height_dst",    required_argument, NULL, 'H'},
    {"lowdelay",      required_argument, NULL, 'l'},
    {"Help",          no_argument,       NULL, 'H'},
    {NULL,            0,                 NULL,   0}
};

static void help_message(char* program)
{
    fprintf(stderr, "%s -c libx264 -l 0 -g 20 -f 30 -p pix_fmt yuv420p -i 1280_720_yuv420p.yuv -o 1280x720_yuv420p.h264\n", program);
}

int main(int argc, char **argv)
{
    int c = 0;
    int ret = -1;
    int gop = 10;
    int fps = 25;
    int64_t pts = 0;
    int lowdelay = 0;
    int src_w = 1280;
    int src_h = 720;
    int dst_w = 1280;
    int dst_h = 720;
    int need_size = -1;
    FILE *fp_in  = NULL;
    FILE *fp_out = NULL;
    AVFrame *enc_frame = NULL;
    AVFrame *sws_frame = NULL;
    bool enable_filter = false;
    uint8_t *yuv_buf = NULL;
    int64_t start_time = -1;
    int64_t end_time = -1;
    int64_t start_frame_time = -1;
    int64_t end_frame_time = -1;
    char codec_name[128] = {'\0'};
    char yuv_file[128] = {'\0'};
    char stream_file[128] = {'\0'};
    struct SwsContext* sws_ctx = NULL;
    enum AVPixelFormat src_pix_fmt = AV_PIX_FMT_YUV420P;
    enum AVPixelFormat dst_pix_fmt = AV_PIX_FMT_YUV420P;

    while ((c = getopt_long(argc, argv, ":i:o:c:g:l:f:F:p:P:w:W:h:H:a", long_options, NULL)) != -1) {
        switch (c) {
        case 'i':
            strncpy(yuv_file, optarg, strlen(optarg));
            break;
        case 'o':
            strncpy(stream_file, optarg, strlen(optarg));
            break;
        case 'c':
            strncpy(codec_name, optarg, strlen(optarg));
            break;
        case 'g':
            gop = atoi(optarg);
            break;
        case 'f':
            fps = atoi(optarg);
            break;
        case 'F':
            enable_filter = atoi(optarg) ? true : false;
            break;
        case 'w':
            src_w = atoi(optarg);
            break;
        case 'W':
            dst_w = atoi(optarg);
            break;
        case 'h':
            src_h = atoi(optarg);
            break;
        case 'H':
            dst_h = atoi(optarg);
            break;
        case 'l':
            lowdelay = atoi(optarg);
            break;
        case 'p':
            src_pix_fmt = atoi(optarg);
            break;
        case 'P':
            dst_pix_fmt = atoi(optarg);
            break;
        default:
            help_message(argv[0]);
            return -1;
        }
    }

    if (strcmp(yuv_file, "\0") == 0 || strcmp(stream_file, "\0") == 0) {
        log_err("invalid yuv %s or stream file %s\n", yuv_file, stream_file);
        return -1;
    }

    fp_in = fopen(yuv_file, "rb");
    if (!fp_in) {
        log_err("fopen %s failed\n", yuv_file);
        return -1;
    }

    fp_out = fopen(stream_file, "wb");
    if (!fp_out) {
        log_err("fopen %s failed\n", stream_file);
        return -1;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        log_err("avcodec_find_encoder_by_name %s failed\n", codec_name);
        return -1;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        log_err("avcodec_alloc_context3 failed\n");
        return -1;
    }

    codec_ctx->gop_size         = gop;
    codec_ctx->max_b_frames     = 2;
    codec_ctx->time_base        = (AVRational){1, fps};
    codec_ctx->framerate        = (AVRational){fps, 1};
    codec_ctx->bit_rate         = 3000000;
    codec_ctx->rc_max_rate      = 3000000;
    codec_ctx->rc_min_rate      = 3000000;
    codec_ctx->rc_buffer_size   = 2000000;
    //codec_ctx->thread_count   = 4;
    //codec_ctx->thread_type    = FF_THREAD_FRAME;
    //codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (enable_filter) {
        codec_ctx->width            = dst_w;
        codec_ctx->height           = dst_h;
        codec_ctx->pix_fmt          = dst_pix_fmt;
    } else {
        codec_ctx->width            = src_w;
        codec_ctx->height           = src_h;
        codec_ctx->pix_fmt          = src_pix_fmt;
    }

    if (codec->id == AV_CODEC_ID_H264) {
        ret = av_opt_set(codec_ctx->priv_data, "preset", "medium", 0);
        if (ret < 0) {
            log_err("av_opt_set preset failed, error(%s)", av_err2str(ret));
            return ret;
        }

        ret = av_opt_set(codec_ctx->priv_data, "profile", "main", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        if (lowdelay)
            ret = av_opt_set(codec_ctx->priv_data, "tune","zerolatency",0);
        else
            ret = av_opt_set(codec_ctx->priv_data, "tune","film",0);
        if (ret < 0) {
            log_err("av_opt_set tune failed, error(%s)", av_err2str(ret));
            return -1;
        }
    }

    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return -1;
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    enc_frame = av_frame_alloc();
    if (!enc_frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    enc_frame->width  = codec_ctx->width;
    enc_frame->height = codec_ctx->height;
    enc_frame->format = codec_ctx->pix_fmt;
    ret = av_frame_get_buffer(enc_frame, 0);
    if (ret < 0) {
        log_err("av_frame_get_buffer, error(%s)\n", av_err2str(ret));
        return -1;
    }

    int src_framebuf_len = av_image_get_buffer_size(src_pix_fmt, src_w, src_h, 1);
    yuv_buf = (uint8_t *)malloc(src_framebuf_len);
    if (!yuv_buf) {
        log_err("malloc yuv_buf failed\n");
        return -1;
    }

    if (enable_filter) {
        sws_frame = av_frame_alloc();
        if (!sws_frame) {
            log_err("av_frame_alloc failed\n");
            return -1;
        }

        sws_frame->width  = src_w;
        sws_frame->height = src_h;
        sws_frame->format = src_pix_fmt;
        ret = av_frame_get_buffer(sws_frame, 0);
        if (ret < 0) {
            log_err("av_frame_get_buffer, error(%s)\n", av_err2str(ret));
            return -1;
        }

        sws_ctx = sws_getContext(src_w, src_h, src_pix_fmt, dst_w,
                                 dst_h, dst_pix_fmt, SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws_ctx) {
            log_err("sws_getContext failed\n");
            return -1;
        }
    }

    log_info("thread_count = %d, thread_type = %d, src_framebuf_len = %d",
              codec_ctx->thread_count, codec_ctx->thread_type, src_framebuf_len);

    log_info("encoder start");
    start_time = get_time_ms();

    for (;;) {
        memset(yuv_buf, 0, src_framebuf_len);
        size_t read_bytes = fread(yuv_buf, 1, src_framebuf_len, fp_in);
        if (read_bytes <= 0) {
            break;
        }

        if (av_frame_is_writable(enc_frame) == 0) {
            ret = av_frame_make_writable(enc_frame);
            if (ret < 0) {
                if (enc_frame->buf && enc_frame->buf[0])
                    log_err("av_frame_make_writable failed, ret = %d, ref_count = %d\n", ret, av_buffer_get_ref_count(enc_frame->buf[0]));
                else
                    log_err("av_frame_make_writable failed, ret = %d\n", ret);
                break;
            }
        }

        if (enable_filter) {
            need_size = av_image_fill_arrays(sws_frame->data, sws_frame->linesize, yuv_buf, src_pix_fmt, src_w, src_h, 1);
            if (need_size != src_framebuf_len) {
                log_err("av_image_fill_arrays failed, need_size = %d, src_framebuf_len = %d\n", need_size, src_framebuf_len);
                break;
            }

            sws_scale(sws_ctx, sws_frame->data, sws_frame->linesize, 0, src_h, enc_frame->data, enc_frame->linesize);
        } else {
            need_size = av_image_fill_arrays(enc_frame->data, enc_frame->linesize, yuv_buf, enc_frame->format, enc_frame->width, enc_frame->height, 1);
            if (need_size != src_framebuf_len) {
                log_err("av_image_fill_arrays failed, need_size = %d, src_framebuf_len = %d\n", need_size, src_framebuf_len);
                break;
            }
        }

        enc_frame->pts = pts;
        start_frame_time = get_time_ms();
        ret = encode_process(codec_ctx, enc_frame, pkt, fp_out);
        end_frame_time = get_time_ms();

        log_info("encode %d frame, time:%ldms", send_frame_count, end_frame_time - start_frame_time);
        if (ret < 0) {
            break;
        }

        pts += 40;
        send_frame_count++;
    }

    encode_process(codec_ctx, NULL, pkt, fp_out);

    end_time = get_time_ms();
    log_info("total encode time:%ldms", end_time - start_time);

    if (fp_in) {
        fclose(fp_in);
    }

    if (fp_out) {
        fclose(fp_out);
    }

    if (yuv_buf) {
        free(yuv_buf);
    }

    if (enable_filter) {
        av_frame_free(&sws_frame);
    }

    av_frame_free(&enc_frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);

    log_info("encoder ended, press enter to exit");
    getchar();

    return 0;
}
