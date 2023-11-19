#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "log.h"

#define INPUT_FILE_NAME  "../../../stream/video/h264/sintel.h264"
#define OUTPUT_FILE_NAME "out.mp4"

#define ENC_VIDEO_WIDTH    512
#define ENC_VIDEO_HEIGHT   288
#define ENC_VIDEO_BITRATE  400000
#define ENC_TIME_BASE_DEN  15
#define ENC_AV_CODEC_ID    AV_CODEC_ID_H265
#define ENC_AV_PIX_FMT_YUV AV_PIX_FMT_YUV420P 

int pts = 0;
int stream_index;
AVStream* in_stream  = NULL;
AVStream* out_stream = NULL;
AVCodecContext* dec_c = NULL;
AVCodecContext* enc_c = NULL;;
AVFormatContext* ifmt_ctx = NULL;
AVFormatContext* ofmt_ctx = NULL;

static void encode_process(AVFrame* frame, AVPacket* enc_pkt)
{
    int ret;

	ret = avcodec_send_frame(enc_c, frame);
	if (ret < 0) {
        log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return;
	}

	while (ret >= 0) {
		ret = avcodec_receive_packet(enc_c, enc_pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
		else if (ret < 0) {
            log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
            return;
		}

		enc_pkt->stream_index = 0;
		av_packet_rescale_ts(enc_pkt, enc_c->time_base, out_stream->time_base);

        // log_info("write packet size = %d, pts = %ld", enc_pkt->size, enc_pkt->pts);
		ret = av_interleaved_write_frame(ofmt_ctx, enc_pkt);
	    if (ret < 0) {
            log_err("av_interleaved_write_frame failed, error(%s)\n", av_err2str(ret));
        }

		av_packet_unref(enc_pkt);
	}
}

static void decode_process(AVPacket* dec_pkt, AVFrame* frame, AVPacket* enc_pkt)
{
    int ret;

	av_packet_rescale_ts(dec_pkt, in_stream->time_base,	dec_c->time_base);

	ret = avcodec_send_packet(dec_c, dec_pkt);
	if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
	}

	while (ret >= 0) {
		ret = avcodec_receive_frame(dec_c, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		else if (ret < 0) {
            log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
            return;
		}

		frame->pts = pts++;
		encode_process(frame, enc_pkt);

		av_frame_unref(frame);
	}
}

static int open_input_file()
{
	int ret;
	const AVCodec* codec;

    ret = avformat_open_input(&ifmt_ctx, INPUT_FILE_NAME, NULL, NULL);
	if (ret < 0) {
        log_err("avformat_open_input %s failed, error(%s)", INPUT_FILE_NAME, av_err2str(ret));
		return ret;
	}

    ret = avformat_find_stream_info(ifmt_ctx, NULL);
	if (ret < 0) {
        log_err("avformat_find_stream_info failed, error(%s)", av_err2str(ret));
		return ret;
	}

	stream_index = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (stream_index < 0) {
        log_err("av_find_best_stream %s failed\n", INPUT_FILE_NAME);
		return AVERROR(ENOMEM);
	}

	in_stream = ifmt_ctx->streams[stream_index];

	codec = avcodec_find_decoder(in_stream->codecpar->codec_id);
	if (!codec) {
        log_err("avcodec_find_decoder failed\n");
		return AVERROR_DECODER_NOT_FOUND;
	}

	dec_c = avcodec_alloc_context3(codec);
	if (!dec_c) {
        log_err("avcodec_alloc_context3 failed\n");
		return AVERROR(ENOMEM);
	}

    ret = avcodec_parameters_to_context(dec_c, in_stream->codecpar);
	if (ret < 0) {
        log_err("avcodec_parameters_to_context failed, error(%s)", av_err2str(ret));
		return ret;
	}

    ret = avcodec_open2(dec_c, codec, NULL);
	if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
		return ret;
	}

    return 0;
}

static int open_output_file()
{
	int ret;
	const AVCodec* codec;

	ofmt_ctx = avformat_alloc_context();
    if (!ofmt_ctx) {
        log_err("avformat_alloc_context failed\n");
        return -1;
    }

	avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, OUTPUT_FILE_NAME);
	if (!ofmt_ctx) {
        log_err("avformat_alloc_output_context2 failed");
		return AVERROR_UNKNOWN;
	}

	codec = avcodec_find_encoder(ENC_AV_CODEC_ID);
	if (!codec) {
        log_err("avcodec_find_encoder failed\n");
		return AVERROR_INVALIDDATA;
	}

	enc_c = avcodec_alloc_context3(codec);
	if (!enc_c) {
        log_err("avcodec_alloc_context3 failed\n");
		return AVERROR(ENOMEM);
	}

	enc_c->codec_id = codec->id;
	enc_c->pix_fmt = ENC_AV_PIX_FMT_YUV;  
	enc_c->bit_rate = ENC_VIDEO_BITRATE;
	enc_c->width = ENC_VIDEO_WIDTH; 
	enc_c->height = ENC_VIDEO_HEIGHT;
	enc_c->time_base.num = 1;
	enc_c->time_base.den = ENC_TIME_BASE_DEN; 
	enc_c->gop_size = 12;
	enc_c->max_b_frames = 4;

	//if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
	//	enc_c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	out_stream = avformat_new_stream(ofmt_ctx, NULL);
	if (!out_stream) {
        log_err("avformat_new_stream failed\n");
		return AVERROR_UNKNOWN;
	}

	ret = avcodec_parameters_from_context(out_stream->codecpar, enc_c);
	if (ret < 0) {
        log_err("avcodec_parameters_from_context failed, error(%s)\n", av_err2str(ret));
		return -1;
	}

	out_stream->time_base = enc_c->time_base;

	ret = avcodec_open2(enc_c, codec, NULL);
	if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)\n", av_err2str(ret));
		return ret;
	}

	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt_ctx->pb, OUTPUT_FILE_NAME, AVIO_FLAG_WRITE);
		if (ret < 0) {
            log_err("avio_open %s failed\n", OUTPUT_FILE_NAME);
			return ret;
		}
	}

	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
        log_err("avformat_write_header failed, error(%s)", av_err2str(ret));
		return ret;
	}

	return 0;
}

int main(int argc, char* argv[])
{
    int ret = 0;
    AVFrame* frame = NULL;
    AVPacket* enc_pkt = NULL;
    AVPacket* dec_pkt = NULL;

    ret = open_input_file();
	if (ret < 0) {
		goto end;
	}

    ret = open_output_file();
	if (ret < 0) {
		goto end;
	}

	dec_pkt = av_packet_alloc();
	if (!dec_pkt) {
		goto end;
	}

	enc_pkt = av_packet_alloc();
	if (!enc_pkt) {
		goto end;
	}

	frame = av_frame_alloc();
	if (!frame) {
		goto end;
	}

	while (av_read_frame(ifmt_ctx, dec_pkt) >= 0) {
		if (dec_pkt->stream_index == stream_index) {
			decode_process(dec_pkt, frame, enc_pkt);
		}

		av_packet_unref(dec_pkt);
	}

	av_write_trailer(ofmt_ctx);
end:
	avcodec_free_context(&dec_c);
	avcodec_free_context(&enc_c);
	av_frame_free(&frame);
	av_packet_free(&dec_pkt);
	av_packet_free(&enc_pkt);
	avformat_close_input(&ifmt_ctx);
	avformat_free_context(ofmt_ctx);

	return 0;
}
