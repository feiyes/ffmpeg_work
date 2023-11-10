#include "log.h"
#include "ff_codec.h"
#include "ff_filter.h"
#include "ff_open_file.h"
#include "ff_transcode.h"

TranscodeContext transcode_context;

int main(int argc, char **argv)
{
    int ret;
    AVPacket *packet = NULL;
    unsigned int stream_index;
    unsigned int i;

    if (argc != 3) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s <input file> <output file>\n", argv[0]);
        return 1;
    }

    //av_log_set_level(AV_LOG_INFO);

    log_info("ff_open_input_file\n");
    if ((ret = ff_open_input_file(argv[1], &transcode_context)) < 0)
        goto end;

    log_info("ff_open_output_file\n");
    if ((ret = ff_open_output_file(argv[2], &transcode_context)) < 0)
        goto end;

    log_info("ff_init_filters\n");
    if ((ret = ff_init_filters(&transcode_context)) < 0)
        goto end;

    log_info("av_packet_alloc\n");
    if (!(packet = av_packet_alloc()))
        goto end;

    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(transcode_context.ifmt_ctx, packet)) < 0)
            break;
        stream_index = packet->stream_index;
        av_log(NULL, AV_LOG_INFO, "Demuxer gave frame of stream_index %u\n",
                stream_index);

        if (transcode_context.filter_ctx[stream_index].filter_graph) {
            StreamContext *stream = &transcode_context.stream_ctx[stream_index];

            av_log(NULL, AV_LOG_INFO, "Going to reencode&filter the frame\n");

            av_packet_rescale_ts(packet,
                                 transcode_context.ifmt_ctx->streams[stream_index]->time_base,
                                 stream->dec_ctx->time_base);
            ret = avcodec_send_packet(stream->dec_ctx, packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(stream->dec_ctx, stream->dec_frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                    break;
                else if (ret < 0)
                    goto end;

                stream->dec_frame->pts = stream->dec_frame->best_effort_timestamp;
                ret = ff_filter_encode_write_frame(stream->dec_frame, stream_index, &transcode_context);
                if (ret < 0)
                    goto end;
            }
        } else {
            /* remux this frame without reencoding */
            av_packet_rescale_ts(packet,
                                 transcode_context.ifmt_ctx->streams[stream_index]->time_base,
                                 transcode_context.ofmt_ctx->streams[stream_index]->time_base);

            ret = av_interleaved_write_frame(transcode_context.ofmt_ctx, packet);
            if (ret < 0)
                goto end;
        }
        av_packet_unref(packet);
    }

    /* flush filters and encoders */
    for (i = 0; i < transcode_context.ifmt_ctx->nb_streams; i++) {
        /* flush filter */
        if (!transcode_context.filter_ctx[i].filter_graph)
            continue;
        ret = ff_filter_encode_write_frame(NULL, i, &transcode_context);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing filter failed\n");
            goto end;
        }

        /* flush encoder */
        ret = ff_flush_encoder(i, &transcode_context);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
            goto end;
        }
    }

    av_write_trailer(transcode_context.ofmt_ctx);
end:
    av_packet_free(&packet);
    for (i = 0; i < transcode_context.ifmt_ctx->nb_streams; i++) {
        avcodec_free_context(&transcode_context.stream_ctx[i].dec_ctx);
        if (transcode_context.ofmt_ctx && transcode_context.ofmt_ctx->nb_streams > i && transcode_context.ofmt_ctx->streams[i] && transcode_context.stream_ctx[i].enc_ctx)
            avcodec_free_context(&transcode_context.stream_ctx[i].enc_ctx);
        if (transcode_context.filter_ctx && transcode_context.filter_ctx[i].filter_graph) {
            avfilter_graph_free(&transcode_context.filter_ctx[i].filter_graph);
            av_packet_free(&transcode_context.filter_ctx[i].enc_pkt);
            av_frame_free(&transcode_context.filter_ctx[i].filtered_frame);
        }

        av_frame_free(&transcode_context.stream_ctx[i].dec_frame);
    }
    av_free(transcode_context.filter_ctx);
    av_free(transcode_context.stream_ctx);
    avformat_close_input(&transcode_context.ifmt_ctx);
    if (transcode_context.ofmt_ctx && !(transcode_context.ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&transcode_context.ofmt_ctx->pb);
    avformat_free_context(transcode_context.ofmt_ctx);

    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "Error occurred: %s\n", av_err2str(ret));

    return ret ? 1 : 0;
}
