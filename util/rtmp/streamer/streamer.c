#include <stdio.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>

#include "log.h"

#define USE_RTSP 1

// ./mediamtx
// ffmpeg -re -i /mnt/hgfs/Work/stream/464cam.h264 -c copy -f flv rtmp://localhost/livestream
// ffmpeg -re -i input.mp4 -c copy -rtsp_transport tcp -f rtsp rtsp://127.0.0.1:8554/stream
// ffmpeg -re -stream_loop -1 -i input.mp4 -c copy -f rtsp rtsp://127.0.0.1:8554/stream

int main(int argc, char* argv[])
{
    int ret;
    int frame_index = 0;
    int videoindex  = -1;
    int64_t pts_time = -1;
    int64_t now_time = -1;
    int64_t duration = -1;
    int64_t start_time = 0;
    AVPacket* packet = NULL;
    AVStream *in_stream  = NULL;
    AVStream *out_stream = NULL;
    const char *in_filename  = NULL;
    const char *out_filename = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVRational time_base   = {1, AV_TIME_BASE};
    AVRational time_base_q = {1, AV_TIME_BASE};

    in_filename  = "../../../stream/video/mp4/cuc_ieschool.flv";
#ifdef USE_RTSP
    out_filename = "rtsp://127.0.0.1:8554/stream";
#else
    out_filename = "rtmp://localhost/livestream";
#endif

    avformat_network_init();

    // input stream
    ret = avformat_open_input(&ifmt_ctx, in_filename, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input %s failed, error(%s)", in_filename, av_err2str(ret));
        goto end;
    }

    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info %s failed, error(%s)", in_filename, av_err2str(ret));
        goto end;
    }

    log_info("input format: ");
    av_dump_format(ifmt_ctx, 0, in_filename, 0);

    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoindex = i;
            break;
        }
    }

    if (videoindex == -1) {
        log_err("not found video stream");
        goto end;
    }

    codec_ctx = avcodec_alloc_context3(NULL);
    if (!codec_ctx) {
        log_err("avcodec_alloc_context3 failed");
        goto end;
    }

    ret = avcodec_parameters_to_context(codec_ctx, ifmt_ctx->streams[videoindex]->codecpar);
    if (ret < 0) {
        log_err("avcodec_parameters_to_context failed, error(%s)", av_err2str(ret));
        goto end;
    }

    //ret = av_opt_set(codec_ctx->priv_data, "tune","zerolatency",0);

    // output stream
    ofmt_ctx = avformat_alloc_context();
    if (!ofmt_ctx) {
        log_err("avformat_alloc_context failed\n");
        goto end;
    }

#ifdef USE_RTSP
    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, "rtsp", out_filename); // RTSP
#else
    // ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, "mpegts", out_filename); // UDP
    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", out_filename); // RTMP
#endif
    if (ret < 0) {
        log_err("avformat_alloc_output_context2 failed, error(%s)", av_err2str(ret));
        goto end;
    }

    out_stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!out_stream) {
        log_err("avformat_new_stream failed\n");
        goto end;
    }

    ret = avcodec_parameters_from_context(out_stream->codecpar, codec_ctx);
    if (ret < 0) {
        log_err("avcodec_parameters_from_context failed, error(%s)", av_err2str(ret));
        goto end;
    }

    log_info("output format: ");
    av_dump_format(ofmt_ctx, 0, out_filename, 1);

    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        ofmt_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            log_err("avio_open %s failed\n", in_filename);
            goto end;
        }
    }

    packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed\n");
        goto end;
    }

    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, error(%s)", av_err2str(ret));
        goto end;
    }

    start_time = av_gettime();
    while (1) {
        ret = av_read_frame(ifmt_ctx, packet);
        if (ret < 0)
            break;

        if (packet->stream_index != videoindex) {
            continue;
        }

        in_stream  = ifmt_ctx->streams[packet->stream_index];
        out_stream = ofmt_ctx->streams[packet->stream_index];

        time_base = in_stream->time_base;

        if (packet->pts == AV_NOPTS_VALUE) {
            duration    = (double)AV_TIME_BASE / av_q2d(in_stream->r_frame_rate);
            packet->pts = (double)(frame_index * duration) / (double)(av_q2d(time_base) * AV_TIME_BASE);
            packet->dts = packet->pts;
            packet->duration = (double)duration / (double)(av_q2d(time_base) * AV_TIME_BASE);
        }

        now_time = av_gettime() - start_time;
        pts_time = av_rescale_q(packet->dts, time_base, time_base_q);
        if (pts_time > now_time)
            av_usleep(pts_time - now_time);

        // convert pts/dts
        packet->pos = -1;
        packet->pts = av_rescale_q_rnd(packet->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        packet->dts = av_rescale_q_rnd(packet->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        packet->duration = av_rescale_q(packet->duration, in_stream->time_base, out_stream->time_base);

        log_info("send %8d video frames, pts = %ld, dts = %ld, duration = %ld\n", frame_index, packet->pts, packet->dts, packet->duration);

        ret = av_interleaved_write_frame(ofmt_ctx, packet);
        if (ret < 0) {
            log_err("av_interleaved_write_frame failed, error(%s)\n", av_err2str(ret));
            break;
        }

        frame_index++;
    }

    av_write_trailer(ofmt_ctx);
end:
    av_packet_free(&packet);

    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_close(ofmt_ctx->pb);

    avformat_free_context(ofmt_ctx);

    avcodec_free_context(&codec_ctx);
    avformat_close_input(&ifmt_ctx);

    return 0;
}
