#include "log.h"
#include "ff_codec.h"
#include "ff_filter.h"
#include "ff_open_file.h"
#include "ff_transcode.h"
#include <stdbool.h>
#include <getopt.h>

#define MAX_GETOPT_OPTIONS 100

struct OptionExt {
    const char *name;
    int has_arg;
    int *flag;
    int val;
    const char *help;
};

typedef struct ConfigParam {
    char in_file[128];
    char out_file[128];
    char acodec_name[16];
    char vcodec_name[16];
} ConfigParam;

static void Help(struct OptionExt *opt, const char *programName)
{
     fprintf(stderr, "------------------------------------------------------------------------------\n");
     fprintf(stderr, "-h                          help\n");
     fprintf(stderr, "-i                          input  stream path\n");
     fprintf(stderr, "-o                          output stream path\n");

     for (int i = 0; i < MAX_GETOPT_OPTIONS; i++) {
         if (opt[i].name == NULL)
             break;

         fprintf(stderr, "%s", opt[i].help);
     }
}

static bool parse_arguments(int argc, char **argv, ConfigParam *cfg)
{
    int opt = 0;
    int index = 0;
    char* optString = "i:o:ha:";

    struct option options[MAX_GETOPT_OPTIONS];
    struct OptionExt options_help[MAX_GETOPT_OPTIONS] = {
        {"an",                    1, NULL, 0, "--an                        disable audio\n"},
        {"vn",                    1, NULL, 0, "--vn                        disable video\n"},
        {"c:a",                   1, NULL, 0, "--c:a                       force audio codec\n"},
        {"c:v",                   1, NULL, 0, "--c:v                       force video codec\n"},
        {"acodec",                1, NULL, 0, "--acodec                    force audio codec('copy' to copy stream)\n"},
        {"vcodec",                1, NULL, 0, "--vcodec                    force video codec('copy' to copy stream)\n"},
        {NULL,                    0, NULL, 0},
    };

    for (int i = 0; i < MAX_GETOPT_OPTIONS;i++) {
        if (options_help[i].name == NULL)
            break;

        memcpy(&options[i], &options_help[i], sizeof(struct option));
    }

    while ((opt = getopt_long(argc, argv, optString, options, &index)) != -1) {
        switch (opt) {
        case 'i':
            memcpy(cfg->in_file, optarg, strlen(optarg));
            break;
        case 'o':
            memcpy(cfg->out_file, optarg, strlen(optarg));
            break;
        case 0:
            if (!strcmp(options[index].name, "c:a")) {
                memcpy(cfg->acodec_name, optarg, strlen(optarg));
            } else if (!strcmp(options[index].name, "c:v")) {
                memcpy(cfg->vcodec_name, optarg, strlen(optarg));
            } else if (!strcmp(options[index].name, "acodec")) {
                memcpy(cfg->acodec_name, optarg, strlen(optarg));
            } else if (!strcmp(options[index].name, "vcodec")) {
                memcpy(cfg->vcodec_name, optarg, strlen(optarg));
            } else if (!strcmp(options[index].name, "an")) {
                memset(cfg->acodec_name, '\0', strlen(cfg->acodec_name));
            } else if (!strcmp(options[index].name, "vn")) {
                memset(cfg->vcodec_name, '\0', strlen(cfg->vcodec_name));
            } else {
                Help(options_help, argv[0]);
                return false;
            }
            break;
        case 'h':
        default:
            Help(options_help, argv[0]);
            return false;
        }
    }

    return true;
}

int main(int argc, char **argv)
{
    int ret;
    unsigned int i;
    AVPacket *packet = NULL;
    unsigned int stream_index;
    ConfigParam cfg = {0};
    TranscodeContext context = {0};

    ret = parse_arguments(argc, argv, &cfg);
    if (ret == false) return -1;

    //av_log_set_level(AV_LOG_INFO);

    if (strcmp(cfg.acodec_name, ""))
        memcpy(context.acodec_name, cfg.acodec_name, strlen(cfg.acodec_name));

    if (strcmp(cfg.vcodec_name, ""))
        memcpy(context.vcodec_name, cfg.vcodec_name, strlen(cfg.vcodec_name));

    log_info("open_input_file, stream(%s->%s), codec(%s %s)\n",
              cfg.in_file, cfg.out_file, cfg.acodec_name, cfg.vcodec_name);
    ret = ff_open_input_file(cfg.in_file, &context);
    if (ret < 0) {
        log_err("open_input_file failed");
        goto end;
    }

    log_info("open_output_file\n");
    ret = ff_open_output_file(cfg.out_file, &context);
    if (ret < 0) {
        log_err("open_output_file failed");
        goto end;
    }

    log_info("init_filters\n");
    ret = ff_init_filters(&context);
    if (ret < 0) {
        log_err("init_filters failed");
        goto end;
    }

    log_info("av_packet_alloc\n");
    packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed\n");
        goto end;
    }

    AVFormatContext *ifmt_ctx    = context.ifmt_ctx;
    AVFormatContext *ofmt_ctx    = context.ofmt_ctx;
    StreamContext *stream_ctx    = context.stream_ctx;
    FilteringContext *filter_ctx = context.filter_ctx;

    while (1) {
        ret = av_read_frame(ifmt_ctx, packet);
        if (ret < 0) {
            log_err("av_read_frame failed, error(%s)", av_err2str(ret));
            break;
        }

        stream_index = packet->stream_index;
        av_log(NULL, AV_LOG_INFO, "Demuxer gave frame of stream_index %u\n",
                stream_index);

        if (filter_ctx[stream_index].filter_graph) {
            StreamContext *stream = &stream_ctx[stream_index];

            av_log(NULL, AV_LOG_INFO, "Going to reencode&filter the frame\n");

            av_packet_rescale_ts(packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 stream->dec_ctx->time_base);
            ret = avcodec_send_packet(stream->dec_ctx, packet);
            if (ret < 0) {
                log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(stream->dec_ctx, stream->dec_frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                    break;
                else if (ret < 0) {
                    log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
                    goto end;
                }

                stream->dec_frame->pts = stream->dec_frame->best_effort_timestamp;
                ret = ff_filter_encode_write_frame(stream->dec_frame, stream_index, &context);
                if (ret < 0)
                    goto end;
            }
        } else {
            /* remux this frame without reencoding */
            av_packet_rescale_ts(packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 ofmt_ctx->streams[stream_index]->time_base);

            ret = av_interleaved_write_frame(ofmt_ctx, packet);
            if (ret < 0) {
                log_err("av_interleaved_write_frame failed, error(%s)\n", av_err2str(ret));
                goto end;
            }
        }
        av_packet_unref(packet);
    }

    /* flush filters and encoders */
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        if (!filter_ctx[i].filter_graph)
            continue;

        ret = ff_filter_encode_write_frame(NULL, i, &context);
        if (ret < 0) {
            log_err("flushing filter failed\n");
            goto end;
        }

        ret = ff_flush_encoder(i, &context);
        if (ret < 0) {
            log_err("flushing encoder failed\n");
            goto end;
        }
    }

    av_write_trailer(ofmt_ctx);
end:
    av_packet_free(&packet);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        avcodec_free_context(&stream_ctx[i].dec_ctx);

        if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
            avcodec_free_context(&stream_ctx[i].enc_ctx);

        if (filter_ctx && filter_ctx[i].filter_graph) {
            avfilter_graph_free(&filter_ctx[i].filter_graph);
            av_packet_free(&filter_ctx[i].enc_pkt);
            av_frame_free(&filter_ctx[i].filtered_frame);
        }

        av_frame_free(&stream_ctx[i].dec_frame);
    }

    av_free(filter_ctx);
    av_free(stream_ctx);
    avformat_close_input(&ifmt_ctx);

    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);

    avformat_free_context(ofmt_ctx);

    if (ret < 0)
        log_err("transcode failed\n");

    return ret ? 1 : 0;
}
