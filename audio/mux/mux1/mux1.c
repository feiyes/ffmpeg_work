#include <stdio.h>
#include <libavutil/log.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "log.h"

#define AacHeader

#define OUTPUT_CHANNELS 2
#define OUTPUT_BIT_RATE 96000
#define OUTPUT_SAMPLE_RATE 48000

const char* fileName = "../../../stream/video/mp4/war3end.mp4";
#ifdef AacHeader
const char* outFileName = "out.aac";
#else
const char* outFileName = "out.mp4";
#endif

const int sampling_frequencies[] = {
    96000,  // 0x0
    88200,  // 0x1
    64000,  // 0x2
    48000,  // 0x3
    44100,  // 0x4
    32000,  // 0x5
    24000,  // 0x6
    22050,  // 0x7
    16000,  // 0x8
    12000,  // 0x9
    11025,  // 0xa
    8000    // 0xb
            // 0xc d e f reserved
};

int add_adts_header(char* const adts_header, const int data_length,
                    const int profile, const int sample_rate, const int channels)
{
    int i = 0;
    int sample_freq_index = 3; // 48000hz
    int adtsLen = data_length + 7;

    int frequencies_size = sizeof(sampling_frequencies) / sizeof(sampling_frequencies[0]);
    for (i = 0; i < frequencies_size; i++) {
        if (sampling_frequencies[i] == sample_rate) {
            sample_freq_index = i;
            break;
        }
    }

    if (i >= frequencies_size) {
        log_err("unsupport sample rate:%d\n", sample_rate);
        return -1;
    }

    adts_header[0] = 0xff;                              // syncword:0xfff                          high 8bits
    adts_header[1] = 0xf0;                              // syncword:0xfff                          low  4bits
    adts_header[1] |= (0 << 3);                         // MPEG Version:0 for MPEG-4,1 for MPEG-2       1bit
    adts_header[1] |= (0 << 1);                         // Layer:0                                      2bits
    adts_header[1] |= 1;                                // protection absent:1                          1bit

    adts_header[2] = (profile)<<6;                      // profile:                                     2bits
    adts_header[2] |= (sample_freq_index & 0x0f)<<2;    // sampling frequency index                     4bits
    adts_header[2] |= (0 << 1);                         // private bit:0                                1bit
    adts_header[2] |= (channels & 0x04)>>2;             // channel configuration:                  high 1bit

    adts_header[3] = (channels & 0x03)<<6;              // channel configuration:                  low  2bits
    adts_header[3] |= (0 << 5);                         // original：0                                  1bit
    adts_header[3] |= (0 << 4);                         // home：0                                      1bit
    adts_header[3] |= (0 << 3);                         // copyright id bit：0                          1bit
    adts_header[3] |= (0 << 2);                         // copyright id start：0                        1bit
    adts_header[3] |= ((adtsLen & 0x1800) >> 11);       // frame length：value                     high 2bits

    adts_header[4] = (uint8_t)((adtsLen & 0x7f8) >> 3); // frame length:value                      mid  8bits
    adts_header[5] = (uint8_t)((adtsLen & 0x7) << 5);   // frame length:value                      low  3bits
    adts_header[5] |= 0x1f;                             // buffer fullness:0x7ff                   high 5bits
    adts_header[6] = 0xfc;      //‭11111100‬  // buffer fullness:0x7ff                   low  6bits

    return 0;
}

AVFormatContext * alloc_mp4_context()
{
    int ret = -1;
    AVStream *stream               = NULL;
    AVCodecContext *avctx          = NULL;
    AVIOContext *output_io_context = NULL;
    const AVCodec *output_codec    = NULL;
    AVFormatContext *out_context   = NULL;

    ret = avio_open(&output_io_context, outFileName, AVIO_FLAG_WRITE);
    if (ret < 0) {
        log_err("avio_open failed, error(%s)\n", av_err2str(ret));
        return NULL;
    }

    log_info("avformat_alloc_context\n");
    out_context = avformat_alloc_context();
    if (!out_context) {
        log_err("avformat_alloc_context failed\n");
        return NULL;
    }

    out_context->pb = output_io_context;
    out_context->oformat = av_guess_format(NULL, outFileName, NULL);

    log_info("avcodec_find_encoder\n");
    output_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!output_codec) {
        log_err("avcodec_find_encoder failed\n");
        return NULL;
    }

    log_info("avformat_new_stream\n");
    stream = avformat_new_stream(out_context, NULL);
    if (!stream) {
        log_err("avformat_new_stream failed\n");
        return NULL;
    }

    log_info("avcodec_alloc_context3\n");
    avctx = avcodec_alloc_context3(output_codec);
    if (!avctx) {
        log_err("avcodec_alloc_context3 failed\n");
        return NULL;
    }

    av_channel_layout_default(&avctx->ch_layout, OUTPUT_CHANNELS);
    avctx->sample_rate    = OUTPUT_SAMPLE_RATE;
    avctx->sample_fmt     = output_codec->sample_fmts[0];
    avctx->bit_rate       = OUTPUT_BIT_RATE;
    stream->time_base.den = OUTPUT_SAMPLE_RATE;
    stream->time_base.num = 1;

    if ((out_context)->oformat->flags & AVFMT_GLOBALHEADER)
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    log_info("avcodec_open2\n");
    ret = avcodec_open2(avctx, output_codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)\n", av_err2str(ret));
        return NULL;
    }

    log_info("avcodec_parameters_from_context\n");
    ret = avcodec_parameters_from_context(stream->codecpar, avctx);
    if (ret < 0) {
        log_err("avcodec_parameters_from_context failed, error(%s)\n", av_err2str(ret));
        return NULL;
    }

    av_dump_format(out_context, 0, outFileName, 1);

    return out_context;
}

int main()
{
    int ret = -1;
    FILE *fp = NULL;
    int audio_index = -1;
    AVPacket* packet = NULL;
    AVFormatContext *ctx = NULL;

    //av_log_set_level(AV_LOG_DEBUG);

    fp = fopen(outFileName, "wb");
    if (!fp) {
        log_err("fopen %s failed\n", outFileName);
        goto failed;
    }

    packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    ret = avformat_open_input(&ctx, fileName, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(ctx, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    audio_index = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (audio_index < 0) {
        log_err("av_find_best_stream %s stream failed\n", av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
        return -1;
    }

    if (ctx->streams[audio_index]->codecpar->codec_id != AV_CODEC_ID_AAC) {
        log_err("audio codec %d is not AAC\n", ctx->streams[audio_index]->codecpar->codec_id);
        goto failed;
    }

#ifndef AacHeader
    AVFormatContext* o_ctx = alloc_mp4_context();
    if (!o_ctx) {
        goto failed;
    }

    log_info("avformat_write_header\n");
    ret = avformat_write_header(o_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, error(%s)\n", av_err2str(ret));
        return -1;
    }
#endif

    while (av_read_frame(ctx, packet) >= 0) {
         if (packet->stream_index == audio_index) {
#ifdef AacHeader
            int len = -1;
            char adts_header_buf[7] = {0};
            add_adts_header(adts_header_buf, packet->size,
                            ctx->streams[audio_index]->codecpar->profile,
                            ctx->streams[audio_index]->codecpar->sample_rate,
                            ctx->streams[audio_index]->codecpar->ch_layout.nb_channels);
            fwrite(adts_header_buf, 1, 7, fp);

            len = fwrite(packet->data, 1, packet->size, fp);
            if (len != packet->size) {
                log_err("len of writed data isn't equal packet size(%d, %d)\n", len, packet->size);
            }
#else
            packet->stream_index = 0;
            ret = av_write_frame(o_ctx, packet);
#endif
        }

        av_packet_unref(packet);
    }

#ifndef AacHeader
    log_info("av_write_trailer\n");
    ret = av_write_trailer(o_ctx);
    if (ret < 0) {
        log_err("av_write_trailer failed, error(%s)\n", av_err2str(ret));
        return -1;
    }
#endif

failed:
    if (ctx) avformat_close_input(&ctx);
    if (fp) fclose(fp);

    return 0;
}
