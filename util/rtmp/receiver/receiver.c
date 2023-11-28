#include <stdio.h>
#include <libavutil/time.h>
#include <libavcodec/bsf.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>

#include "log.h"

#define USE_RTSP 1
// #define USE_H264BSF 1

// ffmpeg -i rtmp://localhost/livestream -c copy dump.flv
// ffplay rtsp://127.0.0.1:8554/stream
// ffmpeg -stimeout 30000000 -i rtsp://127.0.0.1:8554/stream -c copy output.mp4

int main(int argc, char* argv[])
{
    int ret;
    int frame_index = 0;
    int videoindex  = -1;
    int got_idr_frame = 0;
    AVPacket* packet = NULL;
    AVStream *in_stream  = NULL;
    AVStream *out_stream = NULL;
    const char *in_filename  = NULL;
    const char *out_filename = NULL;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;

#ifdef USE_RTSP
    in_filename = "rtsp://127.0.0.1:8554/stream";
#else
    in_filename  = "rtmp://localhost/livestream";
#endif
    out_filename = "receive.flv";

    avformat_network_init();

#ifdef USE_RTSP
    AVDictionary *avdic = NULL;
    av_dict_set(&avdic, "rtsp_transport", "tcp", 0);
    av_dict_set(&avdic, "max_delay", "5000000", 0);
    ret = avformat_open_input(&ifmt_ctx, in_filename, NULL, &avdic);
#else
    ret = avformat_open_input(&ifmt_ctx, in_filename, NULL, NULL);
#endif
    if (ret < 0) {
        log_err("avformat_open_input %s failed, error(%s)", in_filename, av_err2str(ret));
        goto end;
    }

    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info %s failed, error(%s)", in_filename, av_err2str(ret));
        goto end;
    }

    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoindex = i;
            break;
        }
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

    log_info("input format:");
    av_dump_format(ifmt_ctx, 0, in_filename, 0);

    ofmt_ctx = avformat_alloc_context();
    if (!ofmt_ctx) {
        log_err("avformat_alloc_context failed\n");
        return -1;
    }

    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename); //RTMP
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

    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        ofmt_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    log_info("output format:");
    av_dump_format(ofmt_ctx, 0, out_filename, 1);

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

#if USE_H264BSF
    AVBSFContext *bsf_ctx = NULL;
    av_bsf_list_parse_str("h264_mp4toannexb", &bsf_ctx);
    avcodec_parameters_copy(bsf_ctx->par_in, ifmt_ctx->streams[videoindex]->codecpar);

    ret = av_bsf_init(bsf_ctx);
    if (ret) {
        log_err("av_bsf_init failed, error(%s)", av_err2str(ret));
        goto end;
    }
#endif

    while (1) {
        ret = av_read_frame(ifmt_ctx, packet);
        if (ret < 0)
            break;

        if (packet->stream_index != videoindex) {
            continue;
        } else {
            if (packet->flags & AV_PKT_FLAG_KEY)
                got_idr_frame = 1;

            if (got_idr_frame == 0)
                continue;
        }

        in_stream  = ifmt_ctx->streams[packet->stream_index];
        out_stream = ofmt_ctx->streams[packet->stream_index];

        if (packet->pts < 0 || packet->dts < 0) {
            log_warn("invalid frame, pts = %ld, dts = %ld", packet->pts, packet->dts);
            continue;
        }

        // convert pts/dts
        packet->pos = -1;
        packet->pts = av_rescale_q_rnd(packet->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        packet->dts = av_rescale_q_rnd(packet->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        packet->duration = av_rescale_q(packet->duration, in_stream->time_base, out_stream->time_base);

        log_info("receive %8d video frames, pts = %ld, dts = %ld, duration = %ld\n", frame_index, packet->pts, packet->dts, packet->duration);

#if USE_H264BSF
        av_bsf_send_packet(bsf_ctx, packet);

        ret = av_bsf_receive_packet(bsf_ctx, packet);
        if (ret) {
            log_err("av_bsf_receive_packet failed, error(%s)", av_err2str(ret));
            continue;
        }
#endif

        ret = av_interleaved_write_frame(ofmt_ctx, packet);
        if (ret < 0) {
            log_err("av_interleaved_write_frame failed, error(%s)\n", av_err2str(ret));
            break;
        }

        frame_index++;
    }

#if USE_H264BSF
    av_bsf_free(&bsf_ctx);
#endif

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
