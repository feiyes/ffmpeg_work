#include <getopt.h>
#include <libavcodec/avcodec.h>

#include "log.h"

#define IN_BUF_SIZE 1920*1080

static void decode_process(AVCodecContext* dec_ctx, AVFrame* frame, AVPacket* packet, FILE* fp)
{
    int ret;
    int y_size;

    ret = avcodec_send_packet(dec_ctx, packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
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

        y_size = frame->width * frame->height;
        log_info("receive video frame %d", dec_ctx->frame_number);

        fwrite(frame->data[0], 1, y_size,     fp); // Y
        fwrite(frame->data[1], 1, y_size / 4, fp); // U
        fwrite(frame->data[2], 1, y_size / 4, fp); // V
    }
}

static struct option long_options[] = {
    {"input",         required_argument, NULL, 'i'},
    {"output",        required_argument, NULL, 'o'},
    {"codec",         required_argument, NULL, 'c'},
    {NULL,            0,                 NULL,   0}
};

// For decoding stream with a known codec type.
int main(int argc, char* argv[])
{
    int c = 0;
    int ret = -1;
    FILE* fp_in  = NULL;
    FILE* fp_out = NULL;
    char yuv_file[256] = {'\0'};
    char stream_file[256] = {'\0'};
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    while ((c = getopt_long(argc, argv, ":i:o:c:a", long_options, NULL)) != -1) {
        switch (c) {
        case 'i':
            strncpy(stream_file, optarg, strlen(optarg));
            break;
        case 'o':
            strncpy(yuv_file, optarg, strlen(optarg));
            break;
        case 'c':
            codec_id = atoi(optarg);
            break;
        default:
            return -1;
        }
    }

    if (strcmp(stream_file, "\0") == 0 || strcmp(yuv_file, "\0") == 0) {
        log_err("invalid stream %s or yuv file %s\n", stream_file, yuv_file);
        return -1;
    }

    log_info("stream_file(%s), yuv_file(%s), codec(%s)", stream_file, yuv_file, avcodec_get_name(codec_id));

    fp_in = fopen(stream_file, "rb");
    if (!fp_in) {
        log_err("fopen %s failed\n", stream_file);
        return -1;
    }

    fp_out = fopen(yuv_file, "wb+");
    if (!fp_out) {
        log_err("fopen %s failed\n", yuv_file);
        return -1;
    }

    const AVCodec* codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        log_err("avcodec_find_decoder failed\n");
        return -1;
    }

    AVCodecParserContext* parser = av_parser_init(codec->id);
    if (!parser) {
        log_err("av_parser_init failed\n");
        return -1;
    }

    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) {
        log_err("avcodec_alloc_context3 failed\n");
        return -1;
    }

    ret = avcodec_open2(context, codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2, error(%s)", av_err2str(ret));
        return -1;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    int eof;
    int len = -1;
    uint8_t* data = NULL;
    size_t data_size = -1;
    uint8_t in_buf[IN_BUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE] = {0};

    do {
        data_size = fread(in_buf, 1, IN_BUF_SIZE, fp_in);
        if (ferror(fp_in))
            break;
        eof = !data_size;

        data = in_buf;
        while (data_size > 0 || eof) {
            len = av_parser_parse2(parser, context, &packet->data, &packet->size,
                                   data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (len < 0) {
                log_err("av_parser_parse2 failed");
                return -1;
            }

            data += len;
            data_size -= len;

            if (packet->size) {
                if (codec_id == AV_CODEC_ID_JPEGLS) {
                    int i = 0;
                    for (i = 0; i < packet->size -1; i++) {
                        if (packet->data[i] == 0xFF && packet->data[i + 1] == 0xD9)
                            break;
                    }

                    packet->size = i + 2;
                }

                log_info("send video %c frame, codec = %s, width = %d(%d), height = %d(%d) %d",
                          av_get_picture_type_char(parser->pict_type), avcodec_get_name(parser->parser->codec_ids[0]),
                          parser->coded_width, parser->width,
                          parser->coded_height, parser->height, packet->size);

                decode_process(context, frame, packet, fp_out);
            }
            else if (eof)
                break;
        }
    } while (!eof);

    decode_process(context, frame, NULL, fp_out);

    if (fp_in) fclose(fp_in);
    if (fp_out) fclose(fp_out);

    av_parser_close(parser);
    avcodec_free_context(&context);
    av_frame_free(&frame);
    av_packet_free(&packet);

    return 0;
}
