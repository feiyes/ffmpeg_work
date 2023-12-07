#include "log.h"
#include "ff_open_file.h"

int ff_open_input_file(const char *filename, TranscodeContext *context)
{
    int ret;
    unsigned int i;

    AVFormatContext *ifmt_ctx = NULL;
    StreamContext *stream_ctx = NULL;

    ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input failed, error(%s)\n", av_err2str(ret));
        return ret;
    }

    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info failed, error(%s)\n", av_err2str(ret));
        return ret;
    }

    stream_ctx = av_calloc(ifmt_ctx->nb_streams, sizeof(*stream_ctx));
    if (!stream_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *stream = ifmt_ctx->streams[i];
        const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *codec_ctx;
        if (!dec) {
            log_err("avcodec_find_decoder stream #%u failed\n", i);
            return AVERROR_DECODER_NOT_FOUND;
        }

        codec_ctx = avcodec_alloc_context3(dec);
        if (!codec_ctx) {
            log_err("avcodec_alloc_context3 stream #%u failed\n", i);
            return AVERROR(ENOMEM);
        }

        ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (ret < 0) {
            log_err("avcodec_parameters_to_context stream #%u failed, error(%s)\n", i, av_err2str(ret));
            return ret;
        }

        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
            else if (codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
                codec_ctx->pkt_timebase = stream->time_base;

            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, NULL);
            if (ret < 0) {
                log_err("avcodec_open2 stream #%u failed, error(%s)\n", i, av_err2str(ret));
                return ret;
            }
        }

        stream_ctx[i].dec_ctx = codec_ctx;

        stream_ctx[i].dec_frame = av_frame_alloc();
        if (!stream_ctx[i].dec_frame)
            return AVERROR(ENOMEM);
    }

    context->ifmt_ctx   = ifmt_ctx;
    context->stream_ctx = stream_ctx;

    av_dump_format(ifmt_ctx, 0, filename, 0);

    return 0;
}

int ff_open_output_file(const char *filename, TranscodeContext *context)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    const AVCodec *encoder;
    int ret;
    unsigned int i;

    AVFormatContext *ofmt_ctx = NULL;
    AVFormatContext *ifmt_ctx = context->ifmt_ctx;
    StreamContext *stream_ctx = context->stream_ctx;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (!ofmt_ctx) {
        log_err("avformat_alloc_output_context2 failed");
        return AVERROR_UNKNOWN;
    }

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        in_stream = ifmt_ctx->streams[i];
        dec_ctx = stream_ctx[i].dec_ctx;

        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            log_err("avformat_new_stream failed\n");
            return AVERROR_UNKNOWN;
        }

        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* in this example, we choose transcoding to same codec */
            encoder = avcodec_find_encoder(dec_ctx->codec_id);
            if (!encoder) {
                log_err("avcodec_find_encoder failed, codec(%s)\n", avcodec_get_name(dec_ctx->codec_id));
                return AVERROR_INVALIDDATA;
            }

            enc_ctx = avcodec_alloc_context3(encoder);
            if (!enc_ctx) {
                log_err("avcodec_alloc_context3 encoder failed\n");
                return AVERROR(ENOMEM);
            }

            /* In this example, we transcode to same properties (picture size,
             * sample rate etc.). These properties can be changed for output
             * streams easily using filters */
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                enc_ctx->height = dec_ctx->height;
                enc_ctx->width = dec_ctx->width;
                enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                /* take first format from list of supported formats */
                if (encoder->pix_fmts)
                    enc_ctx->pix_fmt = encoder->pix_fmts[0];
                else
                    enc_ctx->pix_fmt = dec_ctx->pix_fmt;
                /* video time_base can be set to whatever is handy and supported by encoder */
                enc_ctx->time_base = av_inv_q(dec_ctx->framerate);

                if (context->copy_video) {
                    const AVBitStreamFilter *filter = NULL;

                    if (dec_ctx->codec_id == AV_CODEC_ID_H264)
                        filter = av_bsf_get_by_name("h264_mp4toannexb");
                    else if (dec_ctx->codec_id == AV_CODEC_ID_H265)
                        filter = av_bsf_get_by_name("hevc_mp4toannexb");
                    if (!filter) {
                        log_err("av_bsf_get_by_name failed");
                        return -1;
                    }

                    ret = av_bsf_alloc(filter, &context->bsf_ctx);
                    if (ret) {
                        log_err("av_bsf_alloc failed, error(%s)", av_err2str(ret));
                        return -1;
                    }

                    avcodec_parameters_copy(context->bsf_ctx->par_in, in_stream->codecpar);

                    ret = av_bsf_init(context->bsf_ctx);
                    if (ret) {
                        log_err("av_bsf_init failed, error(%s)", av_err2str(ret));
                        return -1;
                    }
                }
            } else {
                enc_ctx->sample_rate = dec_ctx->sample_rate;
                ret = av_channel_layout_copy(&enc_ctx->ch_layout, &dec_ctx->ch_layout);
                if (ret < 0)
                    return ret;
                /* take first format from list of supported formats */
                enc_ctx->sample_fmt = encoder->sample_fmts[0];
                enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
            }

            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            /* Third parameter can be used to pass settings to encoder */
            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0) {
                log_err("avcodec_open2 stream #%u failed, error(%s)\n", i, av_err2str(ret));
                return ret;
            }

            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
            if (ret < 0) {
                log_err("avcodec_parameters_from_context stream #%u failed, error(%s)\n", i, av_err2str(ret));
                return ret;
            }

            out_stream->time_base = enc_ctx->time_base;
            stream_ctx[i].enc_ctx = enc_ctx;
        } else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
            log_err("unknown type of stream #%d\n", i);
            return AVERROR_INVALIDDATA;
        } else {
            /* if this stream must be remuxed */
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                log_err("avcodec_parameters_copy subtitle of stream #%u failed, error(%s)", i, av_err2str(ret));
                return ret;
            }

            out_stream->time_base = in_stream->time_base;
        }
    }

    av_dump_format(ofmt_ctx, 0, filename, 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            log_err("avio_open failed, file(%s)", filename);
            return ret;
        }
    }

    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, error(%s)", av_err2str(ret));
        return ret;
    }

    context->ofmt_ctx = ofmt_ctx;

    return 0;
}
