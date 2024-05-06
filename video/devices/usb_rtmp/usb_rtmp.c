#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include "log.h"

#define DO_SWS_SCALE  1
#define DO_ENCODE     1
//#define DUMP_DEC_YUV  1
//#define DUMP_SWS_YUV  1
#define ENC_FPS       30
#define ENC_GOP       30
#define ENC_WIDTH     320
#define ENC_HEIGHT    240

typedef struct InputContext {
    FILE* fp;
    AVFrame*  frame;
    AVPacket* packet;
    AVCodecContext*  dec_ctx;
    AVFormatContext* fmt_ctx;
} InputContext;

typedef struct SwsContext {
    FILE* fp;
    AVFrame* frame;
    struct SwsContext *sws_ctx;
} SwsContext;

typedef struct OutputContext {
    AVStream* stream;
    AVPacket* packet;
    AVCodecContext*  enc_ctx;
    AVFormatContext* fmt_ctx;
} OutputContext;

void list_device_cap()
{
    const AVInputFormat* fmt = av_find_input_format("v4l2");
    if (!fmt) {
        log_err("av_find_input_format failed");
        return;
    }

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return;
    }

    AVDictionary* opt = NULL;
    av_dict_set(&opt, "list_formats", "3", 0);
    avformat_open_input(&fmt_ctx, "/dev/video0", fmt, &opt);

    av_dict_free(&opt);
    avformat_free_context(fmt_ctx);
}

void sws_process(AVCodecContext* dec_ctx, struct SwsContext *sws_ctx,
                 AVFrame* dec_frame, AVFrame* sws_frame, FILE* fp)
{
    sws_frame->pts       = dec_frame->pts;
    sws_frame->pkt_dts   = dec_frame->pkt_dts;
    sws_frame->time_base = dec_frame->time_base;
    sws_frame->duration  = dec_frame->duration;
    sws_scale(sws_ctx, (const uint8_t* const*)dec_frame->data, dec_frame->linesize,
              0, dec_ctx->height, sws_frame->data, sws_frame->linesize);

#ifdef DUMP_SWS_YUV
    int sws_y_size = sws_frame->width * sws_frame->height;
    switch (sws_frame->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        fwrite(sws_frame->data[0], 1, sws_y_size,     fp); // Y
        fwrite(sws_frame->data[1], 1, sws_y_size / 4, fp); // U
        fwrite(sws_frame->data[2], 1, sws_y_size / 4, fp); // V
        break;
    case AV_PIX_FMT_YUYV422:
        fwrite(sws_frame->data[0], 1, sws_y_size * 2, fp);
        break;
    default:
        log_err("unsupport YUV format %d\n", sws_frame->format);
        break;
    }
#endif
}

void encode_process(AVFormatContext *fmt_ctx, AVCodecContext *enc_ctx,
                    AVStream *stream, AVFrame* sws_frame, AVPacket* packet)
{
    int ret = -1;

    ret = avcodec_send_frame(enc_ctx, sws_frame);
    if (ret < 0) {
        //log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return;
    }

    ret = avcodec_receive_packet(enc_ctx, packet);
    if (ret < 0) {
        //log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    av_packet_rescale_ts(packet, enc_ctx->time_base, stream->time_base);

    log_info("receive video packet, pts(%ss %s) dts(%ss %s)",
              av_ts2timestr(packet->pts, &stream->time_base), av_ts2str(packet->pts),
              av_ts2timestr(packet->dts, &stream->time_base), av_ts2str(packet->dts));

    packet->pos = -1;
    packet->stream_index = stream->index;
    av_interleaved_write_frame(fmt_ctx, packet);

    av_packet_unref(packet);
}

void decode_process(InputContext* i, SwsContext* s, OutputContext* o)
{
    int ret = -1;

    ret = avcodec_send_packet(i->dec_ctx, i->packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(i->dec_ctx, i->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
            return;
        }

#ifdef DUMP_DEC_YUV
        int src_y_size = i->frame->width * i->frame->height;
        switch (i->frame->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            fwrite(i->frame->data[0], 1, src_y_size,     i->fp); // Y
            fwrite(i->frame->data[1], 1, src_y_size / 4, i->fp); // U
            fwrite(i->frame->data[2], 1, src_y_size / 4, i->fp); // V
            break;
        case AV_PIX_FMT_YUYV422:
            fwrite(i->frame->data[0], 1, src_y_size * 2, i->fp);
            break;
        default:
            log_err("unsupport YUV format %d\n", i->frame->format);
            break;
        }
#endif

#ifdef DO_SWS_SCALE
        sws_process(i->dec_ctx, s->sws_ctx, i->frame, s->frame, s->fp);
#endif

#ifdef DO_ENCODE
        encode_process(o->fmt_ctx, o->enc_ctx, o->stream, s->frame, o->packet);
#endif
    }
}

int main(int argc, char* argv[])
{
    int ret = -1;
    char* cmd = NULL;
    char* in_url = NULL;
    char* out_url = NULL;
    int video_index = -1;
    SwsContext    s = {0};
    InputContext  i = {0};
    OutputContext o = {0};
    char* venc_name = NULL;
    AVDictionary* opt = NULL;
    const char *out_format = NULL;

    if (argc < 2) {
        return -1;
    }

    cmd        = argv[1];
    out_format = argv[2];
    venc_name  = argv[3];
    in_url     = argv[4];
    out_url    = argv[5];

    avdevice_register_all();

    av_log_set_level(AV_LOG_INFO);

    i.fmt_ctx = avformat_alloc_context();
    if (!i.fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return -1;
    }

    if (strcmp(cmd, "vod") == 0 || strcmp(cmd, "enc") == 0) {
        ret = avformat_open_input(&i.fmt_ctx, in_url, NULL, NULL);
        if (ret) {
            log_err("avformat_open_input failed, error=%d(%s)", ret, av_err2str(ret));
            return -1;
        }
    } else if (strcmp(cmd, "uvc") == 0) {
        list_device_cap();

        const AVInputFormat* in_fmt = av_find_input_format("v4l2");
        if (!in_fmt) {
            log_err("av_find_input_format failed");
            return -1;
        }

        av_dict_set(&opt, "video_size", "640x480", 0);
        ret = avformat_open_input(&i.fmt_ctx, in_url, in_fmt, &opt);
        if (ret) {
            log_err("avformat_open_input failed, error=%d(%s)", ret, av_err2str(ret));
            return -1;
        }
    }

    ret = avformat_find_stream_info(i.fmt_ctx, NULL);
    if (ret) {
        log_err("avformat_find_stream_info failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    video_index = av_find_best_stream(i.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_index) {
        log_err("av_find_best_stream failed");
        return -1;
    }

    i.dec_ctx = avcodec_alloc_context3(NULL);
    if (!i.dec_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return -1;
    }

    avcodec_parameters_to_context(i.dec_ctx, i.fmt_ctx->streams[video_index]->codecpar);

    const AVCodec* decoder = avcodec_find_decoder(i.dec_ctx->codec_id);
    if (!decoder) {
        log_err("avcodec_find_decoder failed");
        return -1;
    }

    ret = avcodec_open2(i.dec_ctx, decoder, NULL);
    if (ret) {
        log_err("avcodec_open2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    i.packet = av_packet_alloc();
    if (!i.packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    i.frame = av_frame_alloc();
    if (!i.frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    av_dump_format(i.fmt_ctx, 0, in_url, 0);

#ifdef DUMP_DEC_YUV
    char dec_file[64] = {0};
    sprintf(dec_file, "%dx%d_dec.yuv", i.dec_ctx->width, i.dec_ctx->height);
    i.fp = fopen(dec_file, "wb+");
    if (!i.fp) {
        log_err("fopen %s failed", dec_file);
        return -1;
    }
#endif

#ifdef DO_SWS_SCALE
    s.frame = av_frame_alloc();
    if (!s.frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    s.frame->width  = i.dec_ctx->width;
    s.frame->height = i.dec_ctx->height;
    s.frame->format = AV_PIX_FMT_YUV420P;
    s.sws_ctx = sws_getContext(i.dec_ctx->width, i.dec_ctx->height, i.dec_ctx->pix_fmt,
                               s.frame->width, s.frame->height, s.frame->format, SWS_BICUBIC, NULL, NULL, NULL);
    if (!s.sws_ctx) {
        log_err("sws_getContext failed");
        return -1;
    }

    int frame_bytes = av_image_get_buffer_size(s.frame->format, s.frame->width, s.frame->height, 1);

    uint8_t* sws_buffer = (unsigned char*)av_malloc(frame_bytes * sizeof(unsigned char));
    if (!sws_buffer) {
        log_err("av_malloc failed");
        return -1;
    }

    ret = av_image_fill_arrays(s.frame->data,   s.frame->linesize, sws_buffer,
                               s.frame->format, s.frame->width, s.frame->height, 1);
    if (ret < 0) {
        log_err("av_image_fill_arrays failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

#ifdef DUMP_SWS_YUV
    char sws_file[64] = {0};
    sprintf(sws_file, "%dx%d_sws.yuv", s.frame->width, s.frame->height);
    s.fp = fopen(sws_file, "wb+");
    if (!s.fp) {
        log_err("fopen %s failed", sws_file);
        return -1;
    }
#endif
#endif

#ifdef DO_ENCODE
    o.packet = av_packet_alloc();
    if (!o.packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    o.fmt_ctx = avformat_alloc_context();
    if (!o.fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return -1;
    }

    ret = avformat_alloc_output_context2(&o.fmt_ctx, NULL, out_format, out_url);
    if (ret < 0) {
        log_err("avformat_alloc_output_context2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    o.stream = avformat_new_stream(o.fmt_ctx, NULL);
    if (!o.stream) {
        log_err("avformat_new_stream failed");
        return -1;
    }

    const AVCodec *enc_codec = avcodec_find_encoder_by_name(venc_name);
    if (!enc_codec) {
        log_err("avcodec_find_encoder failed");
        return -1;
    }

    o.enc_ctx = avcodec_alloc_context3(enc_codec);
    if (!o.enc_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return -1;
    }

    o.enc_ctx->bit_rate   = 110000;
    o.enc_ctx->gop_size   = ENC_GOP;
    o.enc_ctx->width      = ENC_WIDTH;
    o.enc_ctx->height     = ENC_HEIGHT;
    o.enc_ctx->codec_id   = enc_codec->id;
    o.enc_ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
    o.enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    o.enc_ctx->time_base  = (AVRational){ 1, ENC_FPS };

    if (o.enc_ctx->codec_id == AV_CODEC_ID_H264 ||
        o.enc_ctx->codec_id == AV_CODEC_ID_H265) {
        o.enc_ctx->qmin         = 10;
        o.enc_ctx->qmax         = 51;
        o.enc_ctx->qcompress    = 0.6;
        o.enc_ctx->max_b_frames = 2;
    } else if (o.enc_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        o.enc_ctx->max_b_frames = 2;
    } else if (o.enc_ctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        o.enc_ctx->mb_decision  = 2;
    }

    if (o.fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        o.enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(o.enc_ctx, enc_codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avcodec_parameters_from_context(o.stream->codecpar, o.enc_ctx);
    if (ret < 0) {
        log_err("avcodec_parameters_from_context failed, error(%s)", av_err2str(ret));
        return -1;
    }

    o.stream->time_base    = o.enc_ctx->time_base;
    o.stream->r_frame_rate = av_inv_q(o.enc_ctx->time_base);

    av_dump_format(o.fmt_ctx, 0, out_url, 1);

    if (!(o.fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&o.fmt_ctx->pb, out_url, AVIO_FLAG_READ_WRITE);
        if (ret < 0) {
            log_err("avio_open failed, error=%d(%s)", ret, av_err2str(ret));
            return -1;
        }
    }

    ret = avformat_write_header(o.fmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }
#endif

    while (av_read_frame(i.fmt_ctx, i.packet) >= 0) {
        if (i.packet->stream_index == video_index) {
            i.packet->pts = i.dec_ctx->frame_number;
            decode_process(&i, &s, &o);
        }

        av_packet_unref(i.packet);
    }

    i.packet = NULL;
    decode_process(&i, &s, &o);

    if (i.fp) fclose(i.fp);

    if (s.fp) fclose(s.fp);

    if (i.dec_ctx) avcodec_free_context(&i.dec_ctx);

    av_dict_free(&opt);
    av_frame_free(&i.frame);
#ifdef DO_SWS_SCALE
    av_free(sws_buffer);
    av_frame_free(&s.frame);
#endif

#ifdef DO_ENCODE
    av_write_trailer(o.fmt_ctx);
    av_packet_free(&o.packet);
    avcodec_free_context(&o.enc_ctx);
    avcodec_close(o.enc_ctx);
    avformat_close_input(&o.fmt_ctx);
#endif
    av_packet_free(&i.packet);
    avformat_free_context(i.fmt_ctx);

    return 0;
}
