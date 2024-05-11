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
    int a_idx;
    int v_idx;
    AVFrame*  frame;
    AVPacket* packet;
    AVCodecContext*  a_ctx;
    AVCodecContext*  v_ctx;
    AVFormatContext* fmt_ctx;
} InputContext;

typedef struct SwsContext {
    FILE* fp;
    uint8_t* buffer;
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

void dump_yuv(AVFrame* frame, FILE* fp)
{
    int src_y_size = frame->width * frame->height;

    switch (frame->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        fwrite(frame->data[0], 1, src_y_size,     fp); // Y
        fwrite(frame->data[1], 1, src_y_size / 4, fp); // U
        fwrite(frame->data[2], 1, src_y_size / 4, fp); // V
        break;
    case AV_PIX_FMT_YUYV422:
        fwrite(frame->data[0], 1, src_y_size * 2, fp);
        break;
    default:
        log_err("unsupport YUV format %d\n", frame->format);
        break;
    }
}

void sws_process(AVCodecContext* v_ctx, struct SwsContext *sws_ctx,
                 AVFrame* dec_frame, AVFrame* sws_frame, FILE* fp)
{
    sws_frame->pts          = dec_frame->pts;
    sws_frame->pkt_dts      = dec_frame->pkt_dts;
    sws_frame->time_base    = dec_frame->time_base;
    sws_frame->pkt_duration = dec_frame->pkt_duration;
    sws_scale(sws_ctx, (const uint8_t* const*)dec_frame->data, dec_frame->linesize,
              0, v_ctx->height, sws_frame->data, sws_frame->linesize);

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

    ret = avcodec_send_packet(i->v_ctx, i->packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(i->v_ctx, i->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
            return;
        }

#ifdef DUMP_DEC_YUV
        if (i->packet->stream_index == i->v_idx) {
            dump_yuv(i->frame, i->fp);
        }
#endif

#ifdef DO_SWS_SCALE
        sws_process(i->v_ctx, s->sws_ctx, i->frame, s->frame, s->fp);
#endif

#ifdef DO_ENCODE
        encode_process(o->fmt_ctx, o->enc_ctx, o->stream, s->frame, o->packet);
#endif
    }
}

int open_input(char* cmd, char* in_url, InputContext* ic)
{
    int ret = -1;
    AVDictionary* opt = NULL;

    ic->fmt_ctx = avformat_alloc_context();
    if (!ic->fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return -1;
    }

    if (strcmp(cmd, "vod") == 0 || strcmp(cmd, "enc") == 0) {
        ret = avformat_open_input(&ic->fmt_ctx, in_url, NULL, NULL);
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
        ret = avformat_open_input(&ic->fmt_ctx, in_url, in_fmt, &opt);
        if (ret) {
            log_err("avformat_open_input failed, error=%d(%s)", ret, av_err2str(ret));
            return -1;
        }
    }

    ret = avformat_find_stream_info(ic->fmt_ctx, NULL);
    if (ret) {
        log_err("avformat_find_stream_info failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    av_dump_format(ic->fmt_ctx, 0, in_url, 0);

    for (int i = 0; i < ic->fmt_ctx->nb_streams; i++) {
        AVStream* stream = ic->fmt_ctx->streams[i];
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
            ic->a_idx = i;
            ic->a_ctx = codec_ctx;
            log_info("audio codec %s", avcodec_get_name(codecpar->codec_id));
        } else if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ic->v_idx = i;
            ic->v_ctx = codec_ctx;
            log_info("video codec %s, delay %d", avcodec_get_name(codecpar->codec_id), codecpar->video_delay);
        }
    }

    ic->packet = av_packet_alloc();
    if (!ic->packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    ic->frame = av_frame_alloc();
    if (!ic->frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

#ifdef DUMP_DEC_YUV
    char dec_file[64] = {0};
    sprintf(dec_file, "%dx%d_dec.yuv", ic->v_ctx->width, ic->v_ctx->height);
    ic->fp = fopen(dec_file, "wb+");
    if (!ic->fp) {
        log_err("fopen %s failed", dec_file);
        return -1;
    }
#endif

    av_dict_free(&opt);

    return 0;
}

#ifdef DO_SWS_SCALE
int open_filter(InputContext* ic, SwsContext* sc)
{
    int ret = -1;

    sc->frame = av_frame_alloc();
    if (!sc->frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    sc->frame->width  = ic->v_ctx->width;
    sc->frame->height = ic->v_ctx->height;
    sc->frame->format = AV_PIX_FMT_YUV420P;
    sc->sws_ctx = sws_getContext(ic->v_ctx->width, ic->v_ctx->height, ic->v_ctx->pix_fmt,
                                 sc->frame->width, sc->frame->height, sc->frame->format,
                                 SWS_BICUBIC, NULL, NULL, NULL);
    if (!sc->sws_ctx) {
        log_err("sws_getContext failed");
        return -1;
    }

    int frame_bytes = av_image_get_buffer_size(sc->frame->format, sc->frame->width, sc->frame->height, 1);

    sc->buffer = (unsigned char*)av_malloc(frame_bytes * sizeof(unsigned char));
    if (!sc->buffer) {
        log_err("av_malloc failed");
        return -1;
    }

    ret = av_image_fill_arrays(sc->frame->data,   sc->frame->linesize, sc->buffer,
                               sc->frame->format, sc->frame->width, sc->frame->height, 1);
    if (ret < 0) {
        log_err("av_image_fill_arrays failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

#ifdef DUMP_SWS_YUV
    char sws_file[64] = {0};
    sprintf(sws_file, "%dx%d_sws.yuv", sc->frame->width, sc->frame->height);
    sc->fp = fopen(sws_file, "wb+");
    if (!sc->fp) {
        log_err("fopen %s failed", sws_file);
        return -1;
    }
#endif

    return 0;
}
#endif

#ifdef DO_ENCODE
int open_output(char* venc_name, const char *out_format, char* out_url, OutputContext* oc)
{
    int ret = -1;

    oc->packet = av_packet_alloc();
    if (!oc->packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    oc->fmt_ctx = avformat_alloc_context();
    if (!oc->fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return -1;
    }

    ret = avformat_alloc_output_context2(&oc->fmt_ctx, NULL, out_format, out_url);
    if (ret < 0) {
        log_err("avformat_alloc_output_context2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    oc->stream = avformat_new_stream(oc->fmt_ctx, NULL);
    if (!oc->stream) {
        log_err("avformat_new_stream failed");
        return -1;
    }

    const AVCodec *enc_codec = avcodec_find_encoder_by_name(venc_name);
    if (!enc_codec) {
        log_err("avcodec_find_encoder failed");
        return -1;
    }

    oc->enc_ctx = avcodec_alloc_context3(enc_codec);
    if (!oc->enc_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return -1;
    }

    oc->enc_ctx->bit_rate   = 110000;
    oc->enc_ctx->gop_size   = ENC_GOP;
    oc->enc_ctx->width      = ENC_WIDTH;
    oc->enc_ctx->height     = ENC_HEIGHT;
    oc->enc_ctx->codec_id   = enc_codec->id;
    oc->enc_ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
    oc->enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    oc->enc_ctx->time_base  = (AVRational){ 1, ENC_FPS };

    if (oc->enc_ctx->codec_id == AV_CODEC_ID_H264 ||
        oc->enc_ctx->codec_id == AV_CODEC_ID_H265) {
        oc->enc_ctx->qmin         = 10;
        oc->enc_ctx->qmax         = 51;
        oc->enc_ctx->qcompress    = 0.6;
        oc->enc_ctx->max_b_frames = 2;
    } else if (oc->enc_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        oc->enc_ctx->max_b_frames = 2;
    } else if (oc->enc_ctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        oc->enc_ctx->mb_decision  = 2;
    }

    if (oc->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        oc->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(oc->enc_ctx, enc_codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avcodec_parameters_from_context(oc->stream->codecpar, oc->enc_ctx);
    if (ret < 0) {
        log_err("avcodec_parameters_from_context failed, error(%s)", av_err2str(ret));
        return -1;
    }

    oc->stream->time_base    = oc->enc_ctx->time_base;
    oc->stream->r_frame_rate = av_inv_q(oc->enc_ctx->time_base);

    av_dump_format(oc->fmt_ctx, 0, out_url, 1);

    if (!(oc->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&oc->fmt_ctx->pb, out_url, AVIO_FLAG_READ_WRITE);
        if (ret < 0) {
            log_err("avio_open failed, error=%d(%s)", ret, av_err2str(ret));
            return -1;
        }
    }

    ret = avformat_write_header(oc->fmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    return 0;
}
#endif

int main(int argc, char* argv[])
{
    int ret = -1;
    char* cmd = NULL;
    char* in_url = NULL;
    char* out_url = NULL;
    SwsContext    sc = {0};
    InputContext  ic = {0};
    OutputContext oc = {0};
    char* venc_name = NULL;
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

    ret = open_input(cmd, in_url, &ic);
    if (ret < 0) return -1;

#ifdef DO_SWS_SCALE
    ret = open_filter(&ic, &sc);
    if (ret < 0) return -1;
#endif

#ifdef DO_ENCODE
    ret = open_output(venc_name, out_format, out_url, &oc);
    if (ret < 0) return -1;
#endif

    while (av_read_frame(ic.fmt_ctx, ic.packet) >= 0) {
        if (ic.packet->stream_index == ic.v_idx) {
            ic.packet->pts = ic.v_ctx->frame_number;
            decode_process(&ic, &sc, &oc);
        } else if (ic.packet->stream_index == ic.a_idx) {
            ic.packet->pts = ic.a_ctx->frame_number;
            decode_process(&ic, &sc, &oc);
        }

        av_packet_unref(ic.packet);
    }

    if (ic.v_ctx) {
        ic.packet = NULL;
        decode_process(&ic, &sc, &oc);
    } else if (ic.a_ctx) {
        ic.packet = NULL;
        decode_process(&ic, &sc, &oc);
    }

    if (ic.fp) fclose(ic.fp);

    if (sc.fp) fclose(sc.fp);

    if (ic.v_ctx) avcodec_free_context(&ic.v_ctx);

    av_frame_free(&ic.frame);
#ifdef DO_SWS_SCALE
    av_free(sc.buffer);
    av_frame_free(&sc.frame);
#endif

#ifdef DO_ENCODE
    av_write_trailer(oc.fmt_ctx);
    av_packet_free(&oc.packet);
    avcodec_free_context(&oc.enc_ctx);
    avcodec_close(oc.enc_ctx);
    avformat_close_input(&oc.fmt_ctx);
#endif
    av_packet_free(&ic.packet);
    avformat_free_context(ic.fmt_ctx);

    return 0;
}
