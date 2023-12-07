#include "log.h"
#include "ff_codec.h"

int ff_encode_write_frame(unsigned int stream_index, int flush, StreamContext *stream_ctx, FilteringContext *filter_ctx, AVFormatContext *ofmt_ctx)
{
    StreamContext *stream = &stream_ctx[stream_index];
    FilteringContext *filter = &filter_ctx[stream_index];
    AVFrame *filt_frame = flush ? NULL : filter->filtered_frame;
    AVPacket *enc_pkt = filter->enc_pkt;
    int ret;

    log_info("Encoding frame\n");
    /* encode filtered frame */
    av_packet_unref(enc_pkt);

    ret = avcodec_send_frame(stream->enc_ctx, filt_frame);
    if (ret < 0) {
        log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return ret;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(stream->enc_ctx, enc_pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;

        /* prepare packet for muxing */
        enc_pkt->stream_index = stream_index;
        av_packet_rescale_ts(enc_pkt,
                             stream->enc_ctx->time_base,
                             ofmt_ctx->streams[stream_index]->time_base);

        av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
        /* mux encoded frame */
        ret = av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        if (ret < 0) {
            log_err("av_interleaved_write_frame failed, error(%s)\n", av_err2str(ret));
        }
    }

    return ret;
}

int ff_flush_encoder(unsigned int stream_index, TranscodeContext *context)
{
    StreamContext *stream_ctx = context->stream_ctx;

    if (!(stream_ctx[stream_index].enc_ctx->codec->capabilities & AV_CODEC_CAP_DELAY))
        return 0;

    log_info("Flushing stream #%u encoder", stream_index);
    return ff_encode_write_frame(stream_index, 1, stream_ctx, context->filter_ctx, context->ofmt_ctx);
}
