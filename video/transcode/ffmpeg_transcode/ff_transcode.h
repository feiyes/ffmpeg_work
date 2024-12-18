#ifndef __AV_TRANSCODE_H__
#define __AV_TRANSCODE_H__

#include <stdbool.h>
#include <libavcodec/bsf.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;

    AVPacket *enc_pkt;
    AVFrame *filtered_frame;
} FilteringContext;

typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;

    AVFrame *dec_frame;
} StreamContext;

typedef struct TransCodeContext {
    bool copy_audio;
    bool copy_video;
    char acodec_name[16];
    char vcodec_name[16];
    AVBSFContext *bsf_ctx;
    AVFormatContext *ifmt_ctx;
    AVFormatContext *ofmt_ctx;
    StreamContext *stream_ctx;
    FilteringContext *filter_ctx;
} TranscodeContext;

#endif
