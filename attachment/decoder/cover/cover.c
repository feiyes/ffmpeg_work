#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "log.h"

int main(int argc, char **argv)
{
    int ret = -1;
    AVFormatContext *fmt_ctx = NULL;

    if (argc < 2) {
        return -1;
    }

    FILE *fp = fopen("cover.jpg", "wb");
    if (fp == NULL) {
        log_err("fopen failed");
        return -1;
    }

    ret = avformat_open_input(&fmt_ctx, argv[1], NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input failed, ret = %d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info failed, ret = %d(%s)", ret, av_err2str(ret));
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    int cover_stream_idx = -1;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            cover_stream_idx = i;
            break;
        }
    }

    if (cover_stream_idx == -1) {
        log_err("no cover picture in stream");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVStream *cover_stream = fmt_ctx->streams[cover_stream_idx];
    AVCodecParameters *codec_par = cover_stream->codecpar;

    const AVCodec *codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        log_err("avcodec_find_decoder failed\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        log_err("avcodec_alloc_context3 failed\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    ret = avcodec_parameters_to_context(codec_ctx, codec_par);
    if (ret < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2, error(%s)", av_err2str(ret));
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVFrame *frame = av_frame_alloc();
    if (frame == NULL) {
        log_err("av_frame_alloc failed");
        avcodec_close(codec_ctx);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed");
        return -1;
    }

    while (1) {
        ret = av_read_frame(fmt_ctx, packet);
        if (ret < 0)
            break;

        if (packet->stream_index == cover_stream_idx) {
            fwrite(packet->data, 1, packet->size, fp);
            break;
        }

        av_packet_unref(packet);
    }

    fclose(fp);
    av_frame_free(&frame);
    avcodec_close(codec_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    return 0;
}
