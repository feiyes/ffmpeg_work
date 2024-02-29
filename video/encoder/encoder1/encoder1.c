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
#include <libavformat/avformat.h>

#include "log.h"

#define SEI_SIZE      1024
#define USERDATA_SIZE 32

static int send_frame_count = 0;
static int get_frame_count  = 0;

int64_t get_time_ms()
{
    return av_gettime_relative() / 1000;
}

typedef struct EncoderContext {
    FILE *fp;
    AVStream* stream;
    const AVCodec * codec;
    AVCodecContext *codec_context;
    AVFormatContext* format_context;
} EncoderContext;

static int encode_process(EncoderContext* ctx, AVFrame *frame, AVPacket *packet)
{
    int ret;

    if (frame)
        log_info("send %d frame, pts %3"PRId64, send_frame_count, frame->pts);

    ret = avcodec_send_frame(ctx->codec_context, frame);
    if (ret < 0) {
        log_err("avcodec_send_frame, error(%s)\n", av_err2str(ret));
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx->codec_context, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        } else if (ret < 0) {
            log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
            return -1;
        }

        if (packet->flags & AV_PKT_FLAG_KEY)
            log_info("receive %d frame(key), pts:%3"PRId64" dts:%3"PRId64" size:%d",
                      get_frame_count, packet->pts, packet->dts, packet->size);

        if (!packet->flags)
            log_info("receive %d frame(non-key), pts:%3"PRId64" dts:%3"PRId64" size:%d",
                      get_frame_count, packet->pts, packet->dts, packet->size);

        if (ctx->codec->id == AV_CODEC_ID_MPEG4) {
            packet->stream_index = 0;
            av_packet_rescale_ts(packet, ctx->codec_context->time_base, ctx->stream->time_base);
            ret = av_interleaved_write_frame(ctx->format_context, packet);
            if (ret < 0) {
                log_err("av_interleaved_write_frame failed, error(%s)\n", av_err2str(ret));
                return -1;
            }
        } else {
            fwrite(packet->data, 1, packet->size, ctx->fp);
        }

        get_frame_count++;
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
    char *user_data = NULL;
    AVFrame *enc_frame = NULL;
    AVFrame *sws_frame = NULL;
    AVBufferRef *sei_buf = NULL;
    AVFrameSideData *side_data = NULL;
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

    EncoderContext ctx = {0};

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

    ctx.codec = avcodec_find_encoder_by_name(codec_name);
    if (!ctx.codec) {
        log_err("avcodec_find_encoder_by_name %s failed\n", codec_name);
        return -1;
    }

    ctx.codec_context = avcodec_alloc_context3(ctx.codec);
    if (!ctx.codec_context) {
        log_err("avcodec_alloc_context3 failed\n");
        return -1;
    }

    ctx.codec_context->gop_size         = gop;
    ctx.codec_context->max_b_frames     = 2;
    ctx.codec_context->time_base        = (AVRational){1, fps};
    ctx.codec_context->framerate        = (AVRational){fps, 1};
    ctx.codec_context->bit_rate         = 3000000;
    ctx.codec_context->rc_max_rate      = 3000000;
    ctx.codec_context->rc_min_rate      = 3000000;
    ctx.codec_context->rc_buffer_size   = 2000000;
    //ctx.codec_context->thread_count   = 4;
    //ctx.codec_context->thread_type    = FF_THREAD_FRAME;
    //ctx.codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (enable_filter) {
        ctx.codec_context->width            = dst_w;
        ctx.codec_context->height           = dst_h;
        ctx.codec_context->pix_fmt          = dst_pix_fmt;
    } else {
        ctx.codec_context->width            = src_w;
        ctx.codec_context->height           = src_h;
        ctx.codec_context->pix_fmt          = src_pix_fmt;
    }

    if (ctx.codec->id == AV_CODEC_ID_H264) {
        ret = av_opt_set(ctx.codec_context->priv_data, "preset", "medium", 0);
        if (ret < 0) {
            log_err("av_opt_set preset failed, error(%s)", av_err2str(ret));
            return ret;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "profile", "high", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "level", "4", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "qp", "10", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "intra-refresh", "0", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "aud", "0", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "udu_sei", "0", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "fastfirstpass", "1", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "weightp", "1", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "a53cc", "0", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ret = av_opt_set(ctx.codec_context->priv_data, "forced-idr", "0", 0);
        if (ret < 0) {
            log_err("av_opt_set profile failed, error(%s)", av_err2str(ret));
            return -1;
        }

        if (lowdelay)
            ret = av_opt_set(ctx.codec_context->priv_data, "tune","zerolatency",0);
        else
            ret = av_opt_set(ctx.codec_context->priv_data, "tune","film",0);
        if (ret < 0) {
            log_err("av_opt_set tune failed, error(%s)", av_err2str(ret));
            return -1;
        }
    }

    ret = avcodec_open2(ctx.codec_context, ctx.codec, NULL);
    if (ret) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return -1;
    }

    if (ctx.codec->id == AV_CODEC_ID_MPEG4) {
        ctx.format_context = avformat_alloc_context();
        if (!ctx.format_context) {
            log_err("avformat_alloc_context failed\n");
            return -1;
        }

        ret = avformat_alloc_output_context2(&ctx.format_context, NULL, NULL, stream_file);
        if (ret < 0) {
            log_err("avformat_alloc_output_context2 failed, error(%s)", av_err2str(ret));
            return -1;

        }

        ctx.stream = avformat_new_stream(ctx.format_context, NULL);
        if (!ctx.stream) {
            log_err("avformat_new_stream failed\n");
            return -1;
        }

        ret = avcodec_parameters_from_context(ctx.stream->codecpar, ctx.codec_context);
        if (ret < 0) {
            log_err("avcodec_parameters_from_context failed, error(%s)", av_err2str(ret));
            return -1;
        }

        ctx.stream->time_base = ctx.codec_context->time_base;
    }

    if (ctx.codec->id == AV_CODEC_ID_MPEG4) {
        if (!(ctx.format_context->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&ctx.format_context->pb, stream_file, AVIO_FLAG_WRITE);
            if (ret < 0) {
                log_err("avio_open %s failed\n", stream_file);
                return ret;
            }
        }

        ret = avformat_write_header(ctx.format_context, NULL);
        if (ret < 0) {
            log_err("avformat_write_header failed, error(%s)", av_err2str(ret));
            return -1;
        }
    } else {
        ctx.fp = fopen(stream_file, "wb");
        if (!ctx.fp) {
            log_err("fopen %s failed\n", stream_file);
            return -1;
        }
    }

    user_data = av_malloc(USERDATA_SIZE);
    if (!user_data) {
        log_err("av_malloc failed\n");
        return -1;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    enc_frame = av_frame_alloc();
    if (!enc_frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    sei_buf = av_buffer_alloc(SEI_SIZE);
    if (!sei_buf) {
        log_err("av_buffer_alloc failed\n");
        return -1;
    }

    memset(user_data, 0x0, USERDATA_SIZE);
    sei_buf->data = (uint8_t*)user_data;
    sei_buf->size = USERDATA_SIZE;
    side_data = av_frame_new_side_data_from_buf(enc_frame, AV_FRAME_DATA_SEI_UNREGISTERED, sei_buf);
    if (!side_data) {
        log_err("av_frame_new_side_data_from_buf failed\n");
        return -1;
    }

    enc_frame->width  = ctx.codec_context->width;
    enc_frame->height = ctx.codec_context->height;
    enc_frame->format = ctx.codec_context->pix_fmt;
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
              ctx.codec_context->thread_count, ctx.codec_context->thread_type, src_framebuf_len);

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
                    log_err("av_frame_make_writable failed, ref_count = %d, error(%s)\n", av_buffer_get_ref_count(enc_frame->buf[0]), av_err2str(ret));
                else
                    log_err("av_frame_make_writable failed, error(%s)\n", av_err2str(ret));
                break;
            }
        }

        if (enable_filter) {
            need_size = av_image_fill_arrays(sws_frame->data, sws_frame->linesize, yuv_buf, src_pix_fmt, src_w, src_h, 1);
            if (need_size != src_framebuf_len) {
                log_err("av_image_fill_arrays failed, need_size = %d, src_framebuf_len = %d\n", need_size, src_framebuf_len);
                break;
            }

            sws_scale(sws_ctx, (const uint8_t **)sws_frame->data, sws_frame->linesize, 0, src_h, enc_frame->data, enc_frame->linesize);
        } else {
            need_size = av_image_fill_arrays(enc_frame->data, enc_frame->linesize, yuv_buf, enc_frame->format, enc_frame->width, enc_frame->height, 1);
            if (need_size != src_framebuf_len) {
                log_err("av_image_fill_arrays failed, need_size = %d, src_framebuf_len = %d\n", need_size, src_framebuf_len);
                break;
            }
        }

        snprintf(user_data, USERDATA_SIZE, "Custom User Data %d", send_frame_count);

        enc_frame->pts = pts;
        start_frame_time = get_time_ms();
        ret = encode_process(&ctx, enc_frame, packet);
        end_frame_time = get_time_ms();

        log_info("encode %d frame, time:%ldms", send_frame_count, end_frame_time - start_frame_time);
        if (ret < 0) {
            break;
        }

        pts += 40;
        send_frame_count++;
    }

    encode_process(&ctx, NULL, packet);

    if (ctx.codec->id == AV_CODEC_ID_MPEG4) {
        av_write_trailer(ctx.format_context);
    }

    end_time = get_time_ms();
    log_info("total encode time:%ldms", end_time - start_time);

    if (fp_in) fclose(fp_in);
    if (yuv_buf) free(yuv_buf);
    if (ctx.fp) fclose(ctx.fp);
    if (packet) av_packet_free(&packet);
    if (enable_filter) av_frame_free(&sws_frame);
    if (ctx.codec_context) avcodec_free_context(&ctx.codec_context);

    if (ctx.codec->id == AV_CODEC_ID_MPEG4) {
        avio_close(ctx.format_context->pb);
        avformat_free_context(ctx.format_context);
    }

    if (user_data) av_free(user_data);
    if (enc_frame) {
        av_frame_remove_side_data(enc_frame, AV_FRAME_DATA_SEI_UNREGISTERED);
        av_frame_free(&enc_frame);
    }

    log_info("encoder ended, press enter to exit");
    getchar();

    return 0;
}
