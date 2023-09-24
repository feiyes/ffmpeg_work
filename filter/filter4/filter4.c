#include <libavformat/avformat.h>
#include "libavcodec/avcodec.h"
#include <libswscale/swscale.h>
#include "libavutil/imgutils.h"

#include "log.h"
#include "libavutil/opt.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#define INPUT_FILE_NAME "lh_online.yuv"
#define OUTPUT_FILE_NAME "lh_online_512_512.mp4"

#define SOURCE_AV_PIX_FMT_YUV AV_PIX_FMT_YUV420P 
#define SOURCE_VIDEO_WIDTH 512
#define SOURCE_VIDEO_HEIGHT 288

#define L_AVCODEID AV_CODEC_ID_MPEG4
#define ENC_AV_PIX_FMT_YUV AV_PIX_FMT_YUV420P 
#define ENC_VIDEO_WIDTH 512
#define ENC_VIDEO_HEIGHT 512
#define ENC_VIDEO_BITRATE 400000
#define ENC_TIME_BASE_DEN 15

AVFormatContext* o_fmt_ctx = NULL;
AVCodecContext* enc_c = NULL;;

AVFilterContext* buffersink_ctx;
AVFilterContext* buffersrc_ctx;
AVFilterGraph* filter_graph;

AVStream* out_stream;
AVFrame* frame, * tmp_frame;
AVPacket* enc_pkt;
int ret;

//const char* filters_descr = "scale=512x512";
const char* filters_descr = "movie=my_logo.jpg,scale=80:50[wm];[in][wm]overlay=0:0,scale=512:512[out]";
//const char* filters_descr = "drawbox=x=100:y=100:w=100:h=100:color=red";
//const char* filters_descr = "drawtext=fontfile=方正粗黑宋简体.ttf:fontsize=32:fontcolor=red:text='Hello Word'";

static void encode()
{
	ret = avcodec_send_frame(enc_c, tmp_frame);
	if (ret < 0) {
		log_err("Error sending a frame for encoding\n");
		exit(1);
	}

	while (1) {
		ret = avcodec_receive_packet(enc_c, enc_pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0)
			return;

		enc_pkt->stream_index = 0;
		av_packet_rescale_ts(enc_pkt, enc_c->time_base,	out_stream->time_base);

		printf("Write packet %ld (size=%d)\n", enc_pkt->pts, enc_pkt->size);
		ret = av_interleaved_write_frame(o_fmt_ctx, enc_pkt);
		av_packet_unref(enc_pkt);
	}
}

static int open_output_file()
{
	int ret;
	const AVCodec* codec;
	o_fmt_ctx = avformat_alloc_context();
	avformat_alloc_output_context2(&o_fmt_ctx, NULL, NULL, OUTPUT_FILE_NAME);
	if (!o_fmt_ctx) {
		av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
		return AVERROR_UNKNOWN;
	}

	codec = avcodec_find_encoder(L_AVCODEID);
	if (!codec) {
		av_log(NULL, AV_LOG_FATAL, "encoder Codec not found\n");
		return AVERROR_INVALIDDATA;
	}

	enc_c = avcodec_alloc_context3(codec);
	if (!enc_c) {
		av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
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

	if (o_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		enc_c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	out_stream = avformat_new_stream(o_fmt_ctx, NULL);
	if (!out_stream) {
		av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
		return AVERROR_UNKNOWN;
	}

	ret = avcodec_parameters_from_context(out_stream->codecpar, enc_c);
	if (0 != ret)
	{
		log_err("Failed to copy codec parameters\n");
		return -1;
	}

	out_stream->time_base = enc_c->time_base;

	ret = avcodec_open2(enc_c, codec, NULL);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream\n");
		return ret;
	}

	if (!(o_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&o_fmt_ctx->pb, OUTPUT_FILE_NAME, AVIO_FLAG_WRITE);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Could not open output file %s\n", OUTPUT_FILE_NAME);
			return ret;
		}
	}

	ret = avformat_write_header(o_fmt_ctx, NULL);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
		return ret;
	}

	return 0;
}

static int init_filter_graph()
{
	char args[512];
	int ret = 0;
	const AVFilter* buffersrc = avfilter_get_by_name("buffer");
	const AVFilter* buffersink = avfilter_get_by_name("buffersink");
	AVFilterInOut* outputs = avfilter_inout_alloc();
	AVFilterInOut* inputs = avfilter_inout_alloc();
	enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

	filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !filter_graph) {
		ret = AVERROR(ENOMEM);
		goto clearup;
	}

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d",
		SOURCE_VIDEO_WIDTH, SOURCE_VIDEO_HEIGHT, SOURCE_AV_PIX_FMT_YUV,
		1, ENC_TIME_BASE_DEN);

	ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
		args, NULL, filter_graph);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
		goto clearup;
	}

	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
		NULL, NULL, filter_graph);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
		goto clearup;
	}

	ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
		AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
		goto clearup;
	}


	outputs->name = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
		&inputs, &outputs, NULL)) < 0)
		goto clearup;

	if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
		goto clearup;

clearup:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}

static int filter_encode_write_frame()
{
	ret = av_buffersrc_add_frame(buffersrc_ctx, frame);
	if (ret < 0) {
		av_frame_unref(frame);
		log_err("Error submitting the frame to the filtergraph:");
		return ret;
	}

	while (1) {
		ret = av_buffersink_get_frame(buffersink_ctx, tmp_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0)
			return ret;
		encode();
		av_frame_unref(tmp_frame);
	}
	return ret;
}

int main(int argc, char* argv[])
{
	int i = 0;
	int video_size = 0;
	size_t  data_size;
	uint8_t* temp_buffer;

	int src_bpp = av_get_bits_per_pixel(av_pix_fmt_desc_get(SOURCE_AV_PIX_FMT_YUV));

	FILE* src_file = fopen(INPUT_FILE_NAME, "rb");

	tmp_frame = av_frame_alloc();
	if (!tmp_frame) {
		log_err("Could not allocate video tmp_frame\n");
		exit(1);
	}

	tmp_frame->format = ENC_AV_PIX_FMT_YUV;
	tmp_frame->width = ENC_VIDEO_WIDTH;
	tmp_frame->height = ENC_VIDEO_HEIGHT;

	frame = av_frame_alloc();
	if (!frame) {
		log_err("Could not allocate video frame\n");
		exit(1);
	}

	frame->format = SOURCE_AV_PIX_FMT_YUV;
	frame->width = SOURCE_VIDEO_WIDTH;
	frame->height = SOURCE_VIDEO_HEIGHT;

	temp_buffer = (uint8_t*)av_malloc(av_image_get_buffer_size(SOURCE_AV_PIX_FMT_YUV, SOURCE_VIDEO_WIDTH, SOURCE_VIDEO_HEIGHT, 1));

	if ((ret = av_image_alloc(frame->data, frame->linesize,
		SOURCE_VIDEO_WIDTH, SOURCE_VIDEO_HEIGHT, SOURCE_AV_PIX_FMT_YUV, 1)) < 0) {
		log_err("Could not allocate source image\n");
		goto end;
	}

	if ((ret = av_image_alloc(tmp_frame->data, tmp_frame->linesize,
		ENC_VIDEO_WIDTH, ENC_VIDEO_HEIGHT, ENC_AV_PIX_FMT_YUV, 1)) < 0) {
		log_err("Could not allocate source image\n");
		goto end;
	}

	enc_pkt = av_packet_alloc();
	if (!enc_pkt) {
		goto end;
	}

	ret = init_filter_graph();
	if (ret < 0) {
		log_err("Unable to init filter graph:");
		goto end;
	}

	if ((ret = open_output_file()) < 0) {
		av_log(NULL, AV_LOG_ERROR, "could not open output file %s.\n", OUTPUT_FILE_NAME);
		goto end;
	}

	video_size = SOURCE_VIDEO_WIDTH * SOURCE_VIDEO_HEIGHT;
	while (!feof(src_file)) {
		data_size = fread(temp_buffer, 1, video_size * src_bpp / 8, src_file);
		if (!data_size)
			break;

		memcpy(frame->data[0], temp_buffer, video_size);                          // Y
		memcpy(frame->data[1], temp_buffer + video_size, video_size / 4);         // U
		memcpy(frame->data[2], temp_buffer + video_size * 5 / 4, video_size / 4); // V

		frame->pts = i++;
		filter_encode_write_frame();
	}

	av_write_trailer(o_fmt_ctx);

end:
	avfilter_graph_free(&filter_graph);
	avcodec_free_context(&enc_c);
	av_frame_free(&tmp_frame);
	av_packet_free(&enc_pkt);
	avformat_free_context(o_fmt_ctx);

	return 0;
}
