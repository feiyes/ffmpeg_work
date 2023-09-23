#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>

#include "log.h"

typedef struct stream_info {
    bool enable_loop;
    bool enable_filter;
    int  width;
    int  height;
    int  video_index;
    enum AVCodecID codec;
    AVFormatContext* fmt_ctx;
} stream_info_t;

int read_mp4_info(char *mp4_file, stream_info_t* stream)
{
    int ret = 0;
    int index = 0;
    AVFormatContext *fmt = NULL;

    fmt = avformat_alloc_context();
    if (!fmt) {
        log_err("avformat_alloc_context failed\n");
        return -1;
    }

    ret = avformat_open_input(&fmt, mp4_file, NULL, NULL);
    if (ret) {
        log_err("avformat_open_input %s failed, error(%s)", mp4_file, av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(fmt, NULL);
    if (ret) {
        log_err("avformat_find_stream_info %s failed, error(%s)", mp4_file, av_err2str(ret));
        return -1;
    }

    index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (index < 0) {
        log_err("av_find_best_stream %s failed\n", mp4_file);
        return -1;
    }

    stream->fmt_ctx = fmt;
    stream->video_index = index;
    stream->codec  = fmt->streams[index]->codecpar->codec_id;
    stream->width  = fmt->streams[index]->codecpar->width;
    stream->height = fmt->streams[index]->codecpar->height;

    av_dump_format(fmt, 0, mp4_file, 0);

    return 0;
}

int extract_mp4_stream(char* stream_file, stream_info_t* stream)
{
    int ret;
    FILE *fp = NULL;
    AVPacket *packet;
    AVBSFContext *bsf_ctx = NULL;
    int video_index = stream->video_index;
    const AVBitStreamFilter *filter;
    AVFormatContext *fmt_ctx = stream->fmt_ctx;

    fp = fopen(stream_file, "wb");
    if (!fp) {
        log_err("fopen %s failed", stream_file);
        return -1;
    }

    if (stream->codec != AV_CODEC_ID_H264 && stream->codec != AV_CODEC_ID_H265) {
        log_err("unsupport video codec %d", stream->codec);
        return -1;
    }

    if (stream->enable_filter == true) {
        if (stream->codec == AV_CODEC_ID_H264) {
            av_bsf_list_parse_str("h264_mp4toannexb,filter_units=pass_types=1-8,extract_extradata,h264_metadata", &bsf_ctx);
        } else if (stream->codec == AV_CODEC_ID_H265) {
            av_bsf_list_parse_str("hevc_mp4toannexb,filter_units=remove_types=35|38,extract_extradata,hevc_metadata", &bsf_ctx);
        }
    } else {
        if (stream->codec == AV_CODEC_ID_H264) {
            filter = av_bsf_get_by_name("h264_mp4toannexb");
        } else if (stream->codec == AV_CODEC_ID_H265) {
            filter = av_bsf_get_by_name("hevc_mp4toannexb");
        }

        if (!filter) {
            log_err("av_bsf_get_by_name failed");
            return -1;
        }

        ret = av_bsf_alloc(filter, &bsf_ctx);
        if (ret) {
            log_err("av_bsf_alloc failed, error(%s)", av_err2str(ret));
            return -1;
        }
    }

    avcodec_parameters_copy(bsf_ctx->par_in, fmt_ctx->streams[video_index]->codecpar);

    ret = av_bsf_init(bsf_ctx);
    if (ret) {
        log_err("av_bsf_init failed, error(%s)", av_err2str(ret));
        return -1;
    }

    packet = av_packet_alloc();

    while (1)
    {
        ret = av_read_frame(fmt_ctx, packet);
        if (ret) {
            av_packet_unref(packet);
            if (stream->enable_loop == true) {
                log_info("replay...");
                av_seek_frame(fmt_ctx, video_index, 0, AVSEEK_FLAG_BACKWARD);
                ret = av_read_frame(fmt_ctx, packet);
            } else {
                break;
            }
        }

        if (packet->stream_index != video_index)
            continue;

        av_bsf_send_packet(bsf_ctx, packet);
        ret = av_bsf_receive_packet(bsf_ctx, packet);
        if (ret) {
            log_err("av_bsf_receive_packet failed, error(%s)", av_err2str(ret));
            continue;
        }

        if (fp) {
            fwrite(packet->data, packet->size, 1, fp);
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    if (fp) {
        fclose(fp);
    }

    return 0;
}

static struct option long_options[] = {
    {"input",         required_argument, NULL, 'i'},
    {"output",        required_argument, NULL, 'o'},
    {"filter",        required_argument, NULL, 'f'},
    {"loop",          required_argument, NULL, 'l'},
    {"help",          no_argument,       NULL, 'h'},
    {NULL,            0,                 NULL,   0}
};

int main(int argc, char* argv[])
{
    int c = 0;
    char mp4_file[256] = {'\0'};
    char stream_file[256] = {'\0'};
    stream_info_t stream = {0};

    while ((c = getopt_long(argc, argv, ":i:o:l:f:h:a", long_options, NULL)) != -1) {
    switch (c) {
    case 'i':
        strncpy(mp4_file, optarg, strlen(optarg));
        break;
    case 'o':
        strncpy(stream_file, optarg, strlen(optarg));
        break;
    case 'f':
        stream.enable_filter = atoi(optarg) ? true : false;
        break;
    case 'l':
        stream.enable_loop = atoi(optarg) ? true : false;
        break;
    case 'h':
        break;
    default:
        return -1;
    }
    }

    if (strcmp(mp4_file, "\0") == 0 || strcmp(stream_file, "\0") == 0) {
        log_err("invalid mp4 %s or out file %s\n", mp4_file, stream_file);
        return -1;
    }

    read_mp4_info(mp4_file, &stream);

    extract_mp4_stream(stream_file, &stream);

	return 0;
}
