#include <stdlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "log.h"

int main(int argc, char **argv)
{
    int ret = -1;

    if (argc!= 3) {
        printf("Usage: %s <input video file> <cover picture file>\n", argv[0]);
        return -1;
    }

    AVFormatContext *in_video_fmt = NULL;
    ret = avformat_open_input(&in_video_fmt, argv[1], NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input failed, ret = %d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(in_video_fmt, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info failed, ret = %d(%s)", ret, av_err2str(ret));
        avformat_close_input(&in_video_fmt);
        return -1;
    }

    AVFormatContext *in_cover_fmt = NULL;
    ret = avformat_open_input(&in_cover_fmt, argv[2], NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input failed, ret = %d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(in_cover_fmt, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info failed, ret = %d(%s)", ret, av_err2str(ret));
        avformat_close_input(&in_video_fmt);
        avformat_close_input(&in_cover_fmt);
        return -1;
    }

    AVFormatContext *out_fmt = NULL;
    avformat_alloc_output_context2(&out_fmt, NULL, NULL, "output_video.mp4");
    if (out_fmt == NULL) {
        log_err("avformat_alloc_output_context2 failed");
        avformat_close_input(&in_video_fmt);
        avformat_close_input(&in_cover_fmt);
        return -1;
    }

    for (int i = 0; i < in_video_fmt->nb_streams; i++) {
        AVStream *in_stream = in_video_fmt->streams[i];
        AVStream *out_stream = avformat_new_stream(out_fmt, NULL);
        if (out_stream == NULL) {
            log_err("avformat_new_stream failed");
            avformat_close_input(&in_video_fmt);
            avformat_close_input(&in_cover_fmt);
            avformat_free_context(out_fmt);
            return -1;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            log_err("avcodec_parameters_copy failed, ret = %d(%s)", ret, av_err2str(ret));
            avformat_close_input(&in_video_fmt);
            avformat_close_input(&in_cover_fmt);
            avformat_free_context(out_fmt);
            return -1;
        }
    }

    AVStream *pic_stream = avformat_new_stream(out_fmt, NULL);
    if (pic_stream == NULL) {
        log_err("avformat_new_stream failed");
        avformat_close_input(&in_video_fmt);
        avformat_close_input(&in_cover_fmt);
        avformat_free_context(out_fmt);
        return -1;
    }

    ret = avcodec_parameters_copy(pic_stream->codecpar, in_cover_fmt->streams[0]->codecpar);
    if (ret < 0) {
        log_err("avcodec_parameters_copy failed, ret = %d(%s)", ret, av_err2str(ret));
        avformat_close_input(&in_video_fmt);
        avformat_close_input(&in_cover_fmt);
        avformat_free_context(out_fmt);
        return -1;
    }

    pic_stream->disposition |= AV_DISPOSITION_ATTACHED_PIC;

    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_fmt->pb, "output_video.mp4", AVIO_FLAG_WRITE);
        if (ret < 0) {
            log_err("avio_open failed, ret = %d(%s)", ret, av_err2str(ret));
            avformat_close_input(&in_video_fmt);
            avformat_close_input(&in_cover_fmt);
            avformat_free_context(out_fmt);
            return -1;
        }
    }

    ret = avformat_write_header(out_fmt, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, ret = %d(%s)", ret, av_err2str(ret));
        avio_close(out_fmt->pb);
        avformat_close_input(&in_video_fmt);
        avformat_close_input(&in_cover_fmt);
        avformat_free_context(out_fmt);
        return -1;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed");
        return -1;
    }

    while (1) {
        ret = av_read_frame(in_video_fmt, packet);
        if (ret < 0)
            break;

        packet->stream_index = out_fmt->streams[packet->stream_index]->index;
        ret = av_write_frame(out_fmt, packet);
        if (ret < 0) {
            log_err("av_write_frame failed, ret = %d(%s)", ret, av_err2str(ret));
            break;
        }

        av_packet_unref(packet);
    }

    for (int i = 0; i < in_cover_fmt->nb_streams; i++) {
        while (1) {
            ret = av_read_frame(in_cover_fmt, packet);
            if (ret < 0)
                break;

            packet->stream_index = pic_stream->index;
            ret = av_write_frame(out_fmt, packet);
            if (ret < 0) {
                log_err("av_write_frame failed, ret = %d(%s)", ret, av_err2str(ret));
                break;
            }

            av_packet_unref(packet);
        }
    }

    ret = av_write_trailer(out_fmt);
    if (ret < 0) {
        log_err("av_write_trailer failed, ret = %d(%s)", ret, av_err2str(ret));
        return -1;
    }

    av_packet_free(&packet);
    avio_close(out_fmt->pb);
    avformat_close_input(&in_video_fmt);
    avformat_close_input(&in_cover_fmt);
    avformat_free_context(out_fmt);

    return 0;
}
