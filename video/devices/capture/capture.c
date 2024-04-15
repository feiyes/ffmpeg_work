#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include "log.h"

#define DO_SWS_SCALE  1
#define DO_ENCODE     1
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

void decode_process(AVCodecContext* dec_ctx, struct SwsContext *sws_ctx, AVFormatContext *enc_fmt_ctx,
                    AVCodecContext *enc_ctx, AVStream *enc_stream, AVPacket* dec_packet, AVFrame* src_frame,
                    AVFrame* sws_frame, AVPacket* enc_packet, FILE* src_yuv_fp, FILE* sws_yuv_fp)
{
    int ret;
    int src_y_size;

    ret = avcodec_send_packet(dec_ctx, dec_packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, src_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
            return;
        }

        src_y_size = src_frame->width * src_frame->height;
        log_info("receive video src_frame %d, %dx%d", dec_ctx->frame_number, src_frame->width, src_frame->height);

        switch (src_frame->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            fwrite(src_frame->data[0], 1, src_y_size,     src_yuv_fp); // Y
            fwrite(src_frame->data[1], 1, src_y_size / 4, src_yuv_fp); // U
            fwrite(src_frame->data[2], 1, src_y_size / 4, src_yuv_fp); // V
            break;
        case AV_PIX_FMT_YUYV422:
            fwrite(src_frame->data[0], 1, src_y_size * 2, src_yuv_fp);
            break;
        default :
            log_err("unsupport YUV format %d\n", src_frame->format);
            break;
        }

#ifdef DO_SWS_SCALE
        sws_scale(sws_ctx, (const uint8_t* const*)src_frame->data, src_frame->linesize,
                  0, dec_ctx->height, sws_frame->data,sws_frame->linesize);

        int sws_y_size = sws_frame->width * sws_frame->height;
        switch (sws_frame->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            fwrite(sws_frame->data[0], 1, sws_y_size,     sws_yuv_fp); // Y
            fwrite(sws_frame->data[1], 1, sws_y_size / 4, sws_yuv_fp); // U
            fwrite(sws_frame->data[2], 1, sws_y_size / 4, sws_yuv_fp); // V
            break;
        case AV_PIX_FMT_YUYV422:
            fwrite(sws_frame->data[0], 1, sws_y_size * 2, sws_yuv_fp);
            break;
        default :
            log_err("unsupport YUV format %d\n", sws_frame->format);
            break;
        }
#endif

#ifdef DO_ENCODE
        sws_frame->pts = src_frame->pts;

        ret = avcodec_send_frame(enc_ctx, sws_frame);
        if (ret >= 0) {
            ret = avcodec_receive_packet(enc_ctx, enc_packet);
            if (ret >= 0) {
                enc_packet->stream_index = enc_stream->index;

                av_packet_rescale_ts(enc_packet, enc_ctx->time_base, enc_stream->time_base);

                enc_packet->pos = -1;
                av_interleaved_write_frame(enc_fmt_ctx, enc_packet);

                av_packet_unref(enc_packet);
            }
        }
#endif
    }
}

int main(int argc, char* argv[])
{
    int ret = -1;
    int video_index = -1;
    char dec_yuv_file[16];
    char sws_yuv_file[16];
    char enc_h264_file[16];
    FILE* sws_yuv_fp = NULL;
    AVFrame* sws_frame = NULL;
    AVStream *enc_stream = NULL;
    AVPacket *enc_packet = NULL;
    AVCodecContext *enc_ctx = NULL;
    struct SwsContext *sws_ctx = NULL;
    AVFormatContext *enc_fmt_ctx = NULL;
    const AVOutputFormat *enc_out_fmt = NULL;

    avdevice_register_all();

    av_log_set_level(AV_LOG_INFO);

    list_device_cap();

    const AVInputFormat* in_fmt = av_find_input_format("v4l2");
    if (!in_fmt) {
        log_err("av_find_input_format failed");
        return -1;
    }

    AVFormatContext* in_fmt_ctx = avformat_alloc_context();
    if (!in_fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return -1;
    }

    AVDictionary* opt = NULL;
    av_dict_set(&opt, "video_size", "640x480", 0);
    ret = avformat_open_input(&in_fmt_ctx, "/dev/video0", in_fmt, &opt);
    if (ret) {
        log_err("avformat_open_input failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
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

    AVPacket *dec_packet = av_packet_alloc();
    if (!dec_packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    AVFrame* src_frame = av_frame_alloc();
    if (!src_frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    sprintf(dec_yuv_file, "%dx%d_dec.yuv", dec_ctx->width, dec_ctx->height);
    FILE* src_yuv_fp = fopen(dec_yuv_file, "wb+");
    if (!src_yuv_fp) {
        log_err("fopen %s failed", dec_yuv_file);
        return -1;
    }

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

    sprintf(sws_yuv_file, "%dx%d_sws.yuv", sws_frame->width, sws_frame->height);
    sws_yuv_fp = fopen(sws_yuv_file, "wb+");
    if (!src_yuv_fp) {
        log_err("fopen %s failed", dec_yuv_file);
        return -1;
    }
#endif

#ifdef DO_ENCODE
    sprintf(enc_h264_file, "%dx%d.h264", ENC_WIDTH, ENC_HEIGHT);

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

    ret = avformat_alloc_output_context2(&enc_fmt_ctx, NULL, NULL, enc_h264_file);
    if (ret < 0) {
        log_err("avformat_alloc_output_context2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avio_open(&enc_fmt_ctx->pb, enc_h264_file, AVIO_FLAG_READ_WRITE);
    if (ret < 0) {
        log_err("avio_open failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    enc_stream = avformat_new_stream(enc_fmt_ctx, NULL);
    if (!enc_stream) {
        log_err("avformat_new_stream failed");
        return -1;
    }

    enc_stream->time_base.den = 60;
    enc_stream->time_base.num = 1;

    enc_out_fmt = enc_fmt_ctx->oformat;
    const AVCodec *enc_codec = avcodec_find_encoder(enc_out_fmt->video_codec);
    if (!enc_codec) {
        log_err("avcodec_find_encoder failed");
        return -1;
    }

    enc_ctx = avcodec_alloc_context3(enc_codec);
    if (!enc_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return -1;
    }

    enc_ctx->time_base.num = 1;
    enc_ctx->time_base.den = 60;
    enc_ctx->gop_size   = 10;
    enc_ctx->width      = ENC_WIDTH;
    enc_ctx->height     = ENC_HEIGHT;
    enc_ctx->bit_rate   = 110000;
    enc_ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
    enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    enc_ctx->codec_id   = enc_out_fmt->video_codec;

    if (enc_ctx->codec_id == AV_CODEC_ID_H264) {
        enc_ctx->qmin         = 10;
        enc_ctx->qmax         = 51;
        enc_ctx->qcompress    = 0.6;
    } else if (enc_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        enc_ctx->max_b_frames = 2;
    } else if (enc_ctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        enc_ctx->mb_decision  = 2;
    }

    ret = avcodec_parameters_from_context(enc_fmt_ctx->streams[enc_stream->index]->codecpar, enc_ctx);
    if (ret < 0) {
        log_err("avcodec_parameters_from_context failed, error(%s)", av_err2str(ret));
        return -1;
    }

    ret = avcodec_open2(enc_ctx, enc_codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avformat_write_header(enc_fmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }
#endif

    while (av_read_frame(in_fmt_ctx, dec_packet) >= 0) {
        if (dec_packet->stream_index == video_index) {
            decode_process(dec_ctx, sws_ctx, enc_fmt_ctx, enc_ctx, enc_stream, dec_packet,
                           src_frame, sws_frame, enc_packet, src_yuv_fp, sws_yuv_fp);
        }

        av_packet_unref(dec_packet);
    }

    decode_process(dec_ctx, sws_ctx, enc_fmt_ctx, enc_ctx, enc_stream, NULL,
                   src_frame, sws_frame, enc_packet, src_yuv_fp, sws_yuv_fp);

    if (src_yuv_fp) fclose(src_yuv_fp);

    if (sws_yuv_fp) fclose(sws_yuv_fp);

    if (dec_ctx) avcodec_free_context(&dec_ctx);

    av_dict_free(&opt);
    av_frame_free(&src_frame);
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
