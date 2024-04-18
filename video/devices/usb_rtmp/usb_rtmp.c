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
#define ENC_WIDTH     320
#define ENC_HEIGHT    240

void list_device_cap()
{
    const AVInputFormat* in_fmt = av_find_input_format("v4l2");
    if (!in_fmt) {
        log_err("av_find_input_format failed");
        return;
    }

    AVFormatContext* in_fmt_ctx = avformat_alloc_context();
    if (!in_fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return;
    }

    AVDictionary* opt = NULL;
    av_dict_set(&opt, "list_formats", "3", 0);
    avformat_open_input(&in_fmt_ctx, "/dev/video0", in_fmt, &opt);

    av_dict_free(&opt);
    avformat_free_context(in_fmt_ctx);
}

void sws_process(AVCodecContext* dec_ctx, struct SwsContext *sws_ctx,
                 AVFrame* dec_frame, AVFrame* sws_frame, FILE* sws_fp)
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
        fwrite(sws_frame->data[0], 1, sws_y_size,     sws_fp); // Y
        fwrite(sws_frame->data[1], 1, sws_y_size / 4, sws_fp); // U
        fwrite(sws_frame->data[2], 1, sws_y_size / 4, sws_fp); // V
        break;
    case AV_PIX_FMT_YUYV422:
        fwrite(sws_frame->data[0], 1, sws_y_size * 2, sws_fp);
        break;
    default :
        log_err("unsupport YUV format %d\n", sws_frame->format);
        break;
    }
#endif
}

void encode_process(AVFormatContext *enc_fmt_ctx, AVCodecContext *enc_ctx, AVStream *enc_stream,
                    AVStream *dec_stream, AVFrame* sws_frame, AVPacket* enc_packet)
{
    int ret = -1;

    ret = avcodec_send_frame(enc_ctx, sws_frame);
    if (ret < 0) {
        //log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return;
    }

    ret = avcodec_receive_packet(enc_ctx, enc_packet);
    if (ret < 0) {
        //log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    av_packet_rescale_ts(enc_packet, dec_stream->time_base, enc_stream->time_base);

    //log_info("receive video enc_packet, pts(%ss %ld)\n",
    //          av_ts2timestr(enc_packet->pts, &enc_stream->time_base), enc_packet->pts);

    enc_packet->pos = -1;
    enc_packet->stream_index = enc_stream->index;
    av_interleaved_write_frame(enc_fmt_ctx, enc_packet);

    av_packet_unref(enc_packet);
}

void decode_process(AVCodecContext* dec_ctx, AVStream *dec_stream, struct SwsContext *sws_ctx, AVFormatContext *enc_fmt_ctx,
                    AVCodecContext* enc_ctx, AVStream *enc_stream, AVPacket* dec_packet, AVFrame* dec_frame,
                    AVFrame* sws_frame, AVPacket* enc_packet, FILE* dec_fp, FILE* sws_fp)
{
    int ret = -1;

    ret = avcodec_send_packet(dec_ctx, dec_packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, dec_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
            return;
        }

        //log_info("receive video dec_frame %d, %dx%d, pts = %ss",
        //          dec_ctx->frame_number, dec_frame->width, dec_frame->height, av_ts2timestr(dec_frame->pts, &dec_stream->time_base));

#ifdef DUMP_DEC_YUV
        int src_y_size = dec_frame->width * dec_frame->height;
        switch (dec_frame->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            fwrite(dec_frame->data[0], 1, src_y_size,     dec_fp); // Y
            fwrite(dec_frame->data[1], 1, src_y_size / 4, dec_fp); // U
            fwrite(dec_frame->data[2], 1, src_y_size / 4, dec_fp); // V
            break;
        case AV_PIX_FMT_YUYV422:
            fwrite(dec_frame->data[0], 1, src_y_size * 2, dec_fp);
            break;
        default :
            log_err("unsupport YUV format %d\n", dec_frame->format);
            break;
        }
#endif

#ifdef DO_SWS_SCALE
        sws_process(dec_ctx, sws_ctx, dec_frame, sws_frame, sws_fp);

        //log_info("receive video sws_frame, %dx%d, pts = %ss",
        //          sws_frame->width, sws_frame->height, av_ts2timestr(sws_frame->pts, &dec_stream->time_base));
#endif

#ifdef DO_ENCODE
        encode_process(enc_fmt_ctx, enc_ctx, enc_stream, dec_stream, sws_frame, enc_packet);
#endif
    }
}

int main(int argc, char* argv[])
{
    int ret = -1;
    char* cmd = NULL;
    FILE* dec_fp = NULL;
    FILE* sws_fp = NULL;
    char* src_url = NULL;
    char* dst_url = NULL;
    int video_index = -1;
    AVDictionary* opt = NULL;
    const char *format = NULL;
    AVFrame* dec_frame = NULL;
    AVFrame* sws_frame = NULL;
    AVStream *enc_stream = NULL;
    AVPacket *dec_packet = NULL;
    AVPacket *enc_packet = NULL;
    AVCodecContext *enc_ctx = NULL;
    struct SwsContext *sws_ctx = NULL;
    AVFormatContext *enc_fmt_ctx = NULL;

    if (argc < 2) {
        return -1;
    }

    cmd     = argv[1];
    format  = argv[2];
    src_url = argv[3];
    dst_url = argv[4];
    avdevice_register_all();

    av_log_set_level(AV_LOG_INFO);

    AVFormatContext* in_fmt_ctx = avformat_alloc_context();
    if (!in_fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return -1;
    }

    if (strcmp(cmd, "vod") == 0 || strcmp(cmd, "enc") == 0) {
        ret = avformat_open_input(&in_fmt_ctx, src_url, NULL, NULL);
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
        ret = avformat_open_input(&in_fmt_ctx, src_url, in_fmt, &opt);
        if (ret) {
            log_err("avformat_open_input failed, error=%d(%s)", ret, av_err2str(ret));
            return -1;
        }
    }

    ret = avformat_find_stream_info(in_fmt_ctx, NULL);
    if (ret) {
        log_err("avformat_find_stream_info failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    video_index = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_index) {
        log_err("av_find_best_stream failed");
        return -1;
    }

    AVCodecContext* dec_ctx = avcodec_alloc_context3(NULL);
    if (!dec_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return -1;
    }

    avcodec_parameters_to_context(dec_ctx, in_fmt_ctx->streams[video_index]->codecpar);

    const AVCodec* decoder = avcodec_find_decoder(dec_ctx->codec_id);
    if (!decoder) {
        log_err("avcodec_find_decoder failed");
        return -1;
    }

    ret = avcodec_open2(dec_ctx, decoder, NULL);
    if (ret) {
        log_err("avcodec_open2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    dec_packet = av_packet_alloc();
    if (!dec_packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    dec_frame = av_frame_alloc();
    if (!dec_frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

#ifdef DUMP_DEC_YUV
    char dec_file[64] = {0};
    sprintf(dec_file, "%dx%d_dec.yuv", dec_ctx->width, dec_ctx->height);
    dec_fp = fopen(dec_file, "wb+");
    if (!dec_fp) {
        log_err("fopen %s failed", dec_file);
        return -1;
    }
#endif

#ifdef DO_SWS_SCALE
    sws_frame = av_frame_alloc();
    if (!sws_frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, dec_ctx->width,
                             dec_ctx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
    if (!sws_ctx) {
        log_err("sws_getContext failed");
        return -1;
    }

    int frame_bytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, dec_ctx->width, dec_ctx->height, 1);

    uint8_t* sws_buffer = (unsigned char*)av_malloc(frame_bytes * sizeof(unsigned char));
    if (!sws_buffer) {
        log_err("av_malloc failed");
        return -1;
    }

    sws_frame->width  = dec_ctx->width;
    sws_frame->height = dec_ctx->height;
    sws_frame->format = AV_PIX_FMT_YUV420P;
    ret = av_image_fill_arrays(sws_frame->data, sws_frame->linesize, sws_buffer,
                               AV_PIX_FMT_YUV420P, dec_ctx->width, dec_ctx->height, 1);
    if (ret < 0) {
        log_err("av_image_fill_arrays failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

#ifdef DUMP_SWS_YUV
    char sws_file[64] = {0};
    sprintf(sws_file, "%dx%d_sws.yuv", sws_frame->width, sws_frame->height);
    sws_fp = fopen(sws_file, "wb+");
    if (!dec_fp) {
        log_err("fopen %s failed", dec_file);
        return -1;
    }
#endif
#endif

#ifdef DO_ENCODE
    enc_packet = av_packet_alloc();
    if (!dec_packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    enc_fmt_ctx = avformat_alloc_context();
    if (!enc_fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return -1;
    }

    ret = avformat_alloc_output_context2(&enc_fmt_ctx, NULL, format, dst_url);
    if (ret < 0) {
        log_err("avformat_alloc_output_context2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    enc_stream = avformat_new_stream(enc_fmt_ctx, NULL);
    if (!enc_stream) {
        log_err("avformat_new_stream failed");
        return -1;
    }

    const AVCodec *enc_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!enc_codec) {
        log_err("avcodec_find_encoder failed");
        return -1;
    }

    enc_ctx = avcodec_alloc_context3(enc_codec);
    if (!enc_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return -1;
    }

    enc_ctx->gop_size   = 10;
    enc_ctx->width      = ENC_WIDTH;
    enc_ctx->height     = ENC_HEIGHT;
    enc_ctx->bit_rate   = 110000;
    enc_ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
    enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    enc_ctx->codec_id   = AV_CODEC_ID_H264;
    enc_ctx->time_base  = (AVRational){ 1, 15 };

    if (enc_ctx->codec_id == AV_CODEC_ID_H264) {
        enc_ctx->qmin         = 10;
        enc_ctx->qmax         = 51;
        enc_ctx->max_b_frames = 0;
        enc_ctx->qcompress    = 0.6;
    } else if (enc_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        enc_ctx->max_b_frames = 2;
    } else if (enc_ctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        enc_ctx->mb_decision  = 2;
    }

    if (enc_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(enc_ctx, enc_codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avcodec_parameters_from_context(enc_stream->codecpar, enc_ctx);
    if (ret < 0) {
        log_err("avcodec_parameters_from_context failed, error(%s)", av_err2str(ret));
        return -1;
    }

    enc_stream->time_base    = enc_ctx->time_base;
    enc_stream->r_frame_rate = av_inv_q(enc_ctx->time_base);

    av_dump_format(enc_fmt_ctx, 0, dst_url, 1);

    if (!(enc_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&enc_fmt_ctx->pb, dst_url, AVIO_FLAG_READ_WRITE);
        if (ret < 0) {
            log_err("avio_open failed, error=%d(%s)", ret, av_err2str(ret));
            return -1;
        }
    }

    ret = avformat_write_header(enc_fmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }
#endif

    while (av_read_frame(in_fmt_ctx, dec_packet) >= 0) {
        if (dec_packet->stream_index == video_index) {
            AVRational time_base = in_fmt_ctx->streams[video_index]->time_base;
            int64_t duration = (double)AV_TIME_BASE / av_q2d(in_fmt_ctx->streams[video_index]->r_frame_rate);

            if (dec_packet->pts == AV_NOPTS_VALUE) {
                dec_packet->pts = (double)(dec_ctx->frame_number * duration) / (double)(av_q2d(time_base)*AV_TIME_BASE);
                dec_packet->dts = dec_packet->pts;
                dec_packet->duration = (double)duration / (double)(av_q2d(time_base)*AV_TIME_BASE);
            }

            //log_info("send video dec_packet %d, pts(%s %ss %ldus)",
            //          dec_ctx->frame_number, av_ts2str(dec_packet->pts),
            //          av_ts2timestr(dec_packet->pts, &time_base),
            //          av_rescale_q(dec_packet->pts, time_base, AV_TIME_BASE_Q));

            decode_process(dec_ctx, in_fmt_ctx->streams[video_index], sws_ctx, enc_fmt_ctx,
                           enc_ctx, enc_stream, dec_packet, dec_frame, sws_frame, enc_packet, dec_fp, sws_fp);
        }

        av_packet_unref(dec_packet);
    }

    decode_process(dec_ctx, in_fmt_ctx->streams[video_index], sws_ctx, enc_fmt_ctx,
                   enc_ctx, enc_stream, NULL, dec_frame, sws_frame, enc_packet, dec_fp, sws_fp);

    if (dec_fp) fclose(dec_fp);

    if (sws_fp) fclose(sws_fp);

    if (dec_ctx) avcodec_free_context(&dec_ctx);

    av_dict_free(&opt);
    av_frame_free(&dec_frame);
#ifdef DO_SWS_SCALE
    av_free(sws_buffer);
    av_frame_free(&sws_frame);
#endif
#ifdef DO_ENCODE
    av_write_trailer(enc_fmt_ctx);
    av_packet_free(&enc_packet);
    avcodec_free_context(&enc_ctx);
    avcodec_close(enc_ctx);
    avformat_close_input(&enc_fmt_ctx);
#endif
    av_packet_free(&dec_packet);
    avformat_free_context(in_fmt_ctx);

    return 0;
}
