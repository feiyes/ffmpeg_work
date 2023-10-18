#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavcodec/avcodec.h>

#include "log.h"

#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096
static int s_print_format = 0;

static void print_sample_format(const AVFrame *frame)
{
    printf("ar-samplerate: %uHz\n", frame->sample_rate);
    printf("ac-channel: %u\n", frame->ch_layout.nb_channels);
    printf("f-format: %u\n", frame->format);
}

static void decode(AVCodecContext *dec_ctx, AVPacket *packet, AVFrame *frame, FILE *fp_out)
{
    int i, ch;
    int ret, data_size;

    ret = avcodec_send_packet(dec_ctx, packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, packet_size = %d, error(%s)\n", packet->size, av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
            return;
        }

        data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
        if (data_size < 0) {
            log_err("av_get_bytes_per_sample failed\n");
            return;
        }

        if (s_print_format == 0) {
            s_print_format = 1;
            print_sample_format(frame);
        }

        /**
            P表示Planar（平面），其数据格式排列方式为 :
            LLLLLLRRRRRRLLLLLLRRRRRRLLLLLLRRRRRRL...（每个LLLLLLRRRRRR为一个音频帧）
            而不带P的数据格式（即交错排列）排列方式为：
            LRLRLRLRLRLRLRLRLRLRLRLRLRLRLRLRLRLRL...（每个LR为一个音频样本）
         播放范例：   ffplay -ar 48000 -ac 2 -f f32le believe.pcm
          */
        for (i = 0; i < frame->nb_samples; i++)
        {
            for (ch = 0; ch < dec_ctx->ch_layout.nb_channels; ch++)  // 交错的方式写入, 大部分float的格式输出
                fwrite(frame->data[ch] + data_size*i, 1, data_size, fp_out);
        }
    }
}

// ffplay -ar 48000 -ac 2 -f f32le believe.pcm
int main(int argc, char **argv)
{
    int len = 0;
    int ret = 0;
    FILE *fp_in = NULL;
    FILE *fp_out = NULL;
    uint8_t *data = NULL;
    size_t data_size = 0;
    AVPacket *packet = NULL;
    const char *filename;
    const AVCodec *codec;
    const char *outfilename;
    AVFrame *decoded_frame = NULL;
    AVCodecContext *codec_ctx= NULL;
    AVCodecParserContext *parser = NULL;
    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    enum AVCodecID audio_codec_id = AV_CODEC_ID_AAC;

    if (argc <= 2) {
        log_err("Usage: %s <input file> <output file>\n", argv[0]);
        return -1;
    }

    filename    = argv[1];
    outfilename = argv[2];

    fp_in = fopen(filename, "rb");
    if (!fp_in) {
        log_err("fopen %s failed\n", filename);
        return -1;
    }

    fp_out = fopen(outfilename, "wb");
    if (!fp_out) {
        log_err("fopen %s failed\n", outfilename);
        return -1;
    }

    packet = av_packet_alloc();

    if (strstr(filename, "aac") != NULL) {
        audio_codec_id = AV_CODEC_ID_AAC;
    } else if (strstr(filename, "mp3") != NULL) {
        audio_codec_id = AV_CODEC_ID_MP3;
    } else {
        printf("default codec id:%d\n", audio_codec_id);
    }

    // 获取裸流的解析器 AVCodecParserContext(数据)  +  AVCodecParser(方法)
    parser = av_parser_init(audio_codec_id);
    if (!parser) {
        log_err("av_parser_init failed\n");
        return -1;
    }

    codec = avcodec_find_decoder(audio_codec_id);  // AV_CODEC_ID_AAC
    if (!codec) {
        log_err("avcodec_find_decoder failed\n");
        return -1;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        log_err("Could not allocate audio codec context\n");
        return -1;
    }

    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
        return -1;
    }

    data      = inbuf;
    data_size = fread(inbuf, 1, AUDIO_INBUF_SIZE, fp_in);

    while (data_size > 0)
    {
        if (!decoded_frame)
        {
            if (!(decoded_frame = av_frame_alloc())) {
                log_err("Could not allocate audio frame\n");
                return -1;
            }
        }

        ret = av_parser_parse2(parser, codec_ctx, &packet->data, &packet->size,
                               data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            log_err("av_parser_parse2 failed, error(%s)\n", av_err2str(ret));
            return -1;
        }

        data      += ret;   // 跳过已经解析的数据
        data_size -= ret;   // 对应的缓存大小也做相应减小

        if (packet->size)
            decode(codec_ctx, packet, decoded_frame, fp_out);

        if (data_size < AUDIO_REFILL_THRESH)    // 如果数据少了则再次读取
        {
            memmove(inbuf, data, data_size);    // 把之前剩的数据拷贝到buffer的起始位置
            data = inbuf;
            // 读取数据 长度: AUDIO_INBUF_SIZE - data_size
            len = fread(data + data_size, 1, AUDIO_INBUF_SIZE - data_size, fp_in);
            if (len > 0)
                data_size += len;
        }
    }

    packet->data = NULL;   // 让其进入drain mode
    packet->size = 0;
    decode(codec_ctx, packet, decoded_frame, fp_out);

    fclose(fp_out);
    fclose(fp_in);

    avcodec_free_context(&codec_ctx);
    av_parser_close(parser);
    av_frame_free(&decoded_frame);
    av_packet_free(&packet);

    log_info("main finish, please enter Enter and exit");

    return 0;
}
