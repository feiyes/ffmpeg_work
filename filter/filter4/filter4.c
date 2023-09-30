#include <libavformat/avformat.h>
#include "libavcodec/avcodec.h"
#include <libswscale/swscale.h>
#include "libavutil/imgutils.h"

#include "log.h"
#include "libavutil/opt.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#define INPUT_FILE_NAME "640_480.yuv"
#define OUTPUT_FILE_NAME "ring.mp4"

#define SOURCE_AV_PIX_FMT_YUV AV_PIX_FMT_YUV420P 
#define SOURCE_VIDEO_WIDTH 640
#define SOURCE_VIDEO_HEIGHT 480

#define L_AVCODEID AV_CODEC_ID_MPEG4
#define ENC_AV_PIX_FMT_YUV AV_PIX_FMT_YUV420P 
#define ENC_VIDEO_WIDTH 640
#define ENC_VIDEO_HEIGHT 240
#define ENC_VIDEO_BITRATE 400000
#define ENC_TIME_BASE_DEN 15

#define DUMP_FILTER_FRAME 1

AVFormatContext* o_fmt_ctx = NULL;
AVCodecContext* enc_c = NULL;;

AVFilterContext* sink_ctx;
AVFilterContext* src_ctx;
AVFilterGraph* filter_graph;

AVStream* out_stream;
AVFrame* frame, * tmp_frame;
AVPacket* enc_pkt;

//const char* filters_descr = "scale=640x240";
//const char* filters_descr = "[in]scale=640:240[out]";
//const char* filters_descr = "movie=ring_logo.jpg,scale=80:50[wm];[in][wm]overlay=0:0,scale=640:240[out]";
//const char* filters_descr = "drawbox=x=100:y=100:w=100:h=100:color=red";
//const char* filters_descr = "drawtext=fontfile=方正粗黑宋简体.ttf:fontsize=32:fontcolor=red:text='Hello Word'";
const char* filters_descr = "drawtext=fontcolor=red:text='Hello Word'";

static void encode_process()
{
    int ret;

	ret = avcodec_send_frame(enc_c, tmp_frame);
	if (ret) {
		log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return;
	}

	while (1) {
		ret = avcodec_receive_packet(enc_c, enc_pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
        } else if (ret < 0) {
            log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
			return;
        }

		enc_pkt->stream_index = 0;
		av_packet_rescale_ts(enc_pkt, enc_c->time_base,	out_stream->time_base);

		log_info("write packet size = %d, pts = %ld", enc_pkt->size, enc_pkt->pts);

        ret = av_interleaved_write_frame(o_fmt_ctx, enc_pkt);
        if (ret) {
            log_err("av_interleaved_write_frame failed, error(%s)\n", av_err2str(ret));
        }

		av_packet_unref(enc_pkt);
	}
}

static int open_output_file()
{
	int ret;
	const AVCodec* codec;

	o_fmt_ctx = avformat_alloc_context();
	ret = avformat_alloc_output_context2(&o_fmt_ctx, NULL, NULL, OUTPUT_FILE_NAME);
	if (ret) {
        log_err("avformat_alloc_output_context2 failed, error(%s)", av_err2str(ret));
		return AVERROR_UNKNOWN;
	}

	codec = avcodec_find_encoder(L_AVCODEID);
	if (!codec) {
        log_err("avcodec_find_encoder L_AVCODEID failed\n");
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

	//if (o_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
	//	enc_c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	out_stream = avformat_new_stream(o_fmt_ctx, NULL);
	if (!out_stream) {
        log_err("avformat_new_stream failed\n");
		return AVERROR_UNKNOWN;
	}

	ret = avcodec_parameters_from_context(out_stream->codecpar, enc_c);
	if (ret) {
		log_err("avcodec_parameters_from_context failed, error(%s)\n", av_err2str(ret));
		return ret;
	}

	out_stream->time_base = enc_c->time_base;

	ret = avcodec_open2(enc_c, codec, NULL);
	if (ret < 0) {
        log_err("avcodec_open2 failed, error(%s)\n", av_err2str(ret));
		return ret;
	}

	if (!(o_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&o_fmt_ctx->pb, OUTPUT_FILE_NAME, AVIO_FLAG_WRITE);
		if (ret < 0) {
            log_err("avio_open %s failed\n", OUTPUT_FILE_NAME);
			return ret;
		}
	}

	ret = avformat_write_header(o_fmt_ctx, NULL);
	if (ret < 0) {
        log_err("avformat_write_header failed, error(%s)", av_err2str(ret));
		return ret;
	}

	return 0;
}

static int init_filter_graph()
{
	int ret = 0;
	char args[512];
	AVFilterInOut* inputs = avfilter_inout_alloc();
	AVFilterInOut* outputs = avfilter_inout_alloc();
	const AVFilter* buffersrc = avfilter_get_by_name("buffer");
	const AVFilter* buffersink = avfilter_get_by_name("buffersink");
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

	ret = avfilter_graph_create_filter(&src_ctx, buffersrc, "in", args, NULL, filter_graph);
	if (ret < 0) {
        log_err("avfilter_graph_create_filter in_filter failed, error(%s)\n", av_err2str(ret));
		goto clearup;
	}

	ret = avfilter_graph_create_filter(&sink_ctx, buffersink, "out", NULL, NULL, filter_graph);
	if (ret < 0) {
        log_err("avfilter_graph_create_filter out_filter failed, error(%s)\n", av_err2str(ret));
		goto clearup;
	}

	ret = av_opt_set_int_list(sink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
        log_err("av_opt_set_int_list pix_fmts failed\n");
		goto clearup;
	}


	outputs->name = av_strdup("in");
	outputs->filter_ctx = src_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	inputs->name = av_strdup("out");
	inputs->filter_ctx = sink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;

    ret = avfilter_graph_parse_ptr(filter_graph, filters_descr, &inputs, &outputs, NULL);
	if (ret < 0)
		goto clearup;

    ret = avfilter_graph_config(filter_graph, NULL);
	if (ret < 0)
		goto clearup;

clearup:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}

static int filter_encode_write_frame()
{
    int ret;

	ret = av_buffersrc_add_frame(src_ctx, frame);
	if (ret < 0) {
		av_frame_unref(frame);
		log_err("av_buffersrc_add_frame failed, error(%s)", av_err2str(ret));
		return ret;
	}

	while (1) {
		ret = av_buffersink_get_frame(sink_ctx, tmp_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0)
			return ret;

#ifdef DUMP_FILTER_FRAME
        FILE* fp_out = NULL;
        int out_luma_size = 0;
        char* out_file = "filter.yuv";

        fp_out = fopen(out_file, "wb+");
        if (!fp_out) {
            log_err("fopen %s failed\n", out_file);
            return -1;
        }

        out_luma_size = tmp_frame->width * tmp_frame->height;
        
        fwrite(tmp_frame->data[0], 1, out_luma_size,     fp_out);  // Y
        fwrite(tmp_frame->data[1], 1, out_luma_size / 4, fp_out);  // U
        fwrite(tmp_frame->data[2], 1, out_luma_size / 4, fp_out);  // V
#endif

		encode_process();

		av_frame_unref(tmp_frame);
	}

	return ret;
}

int main(int argc, char* argv[])
{
	int i = 0;
    int ret = 0;
	int video_size = 0;
	size_t  data_size;
	uint8_t* temp_buffer;

	int src_bpp = av_get_bits_per_pixel(av_pix_fmt_desc_get(SOURCE_AV_PIX_FMT_YUV));

	FILE* src_file = fopen(INPUT_FILE_NAME, "rb");
    if (src_file == NULL) {
        log_err("fopen %s failed\n", INPUT_FILE_NAME);
        return -1;
    }

	tmp_frame = av_frame_alloc();
	if (!tmp_frame) {
		log_err("Could not allocate video tmp_frame\n");
        return -1;
	}

	tmp_frame->format = ENC_AV_PIX_FMT_YUV;
	tmp_frame->width = ENC_VIDEO_WIDTH;
	tmp_frame->height = ENC_VIDEO_HEIGHT;

	frame = av_frame_alloc();
	if (!frame) {
		log_err("av_frame_alloc failed\n");
        return -1;
	}

	frame->format = SOURCE_AV_PIX_FMT_YUV;
	frame->width  = SOURCE_VIDEO_WIDTH;
	frame->height = SOURCE_VIDEO_HEIGHT;

	temp_buffer = (uint8_t*)av_malloc(av_image_get_buffer_size(SOURCE_AV_PIX_FMT_YUV, SOURCE_VIDEO_WIDTH, SOURCE_VIDEO_HEIGHT, 1));
    if (temp_buffer == NULL) {
        log_err("av_malloc failed\n");
        return -1;
    }

    ret = av_image_alloc(frame->data, frame->linesize,
	        	SOURCE_VIDEO_WIDTH, SOURCE_VIDEO_HEIGHT, SOURCE_AV_PIX_FMT_YUV, 1);
	if (ret < 0) {
		log_err("av_image_alloc failed, error(%s)\n", av_err2str(ret));
		goto end;
	}

    ret = av_image_alloc(tmp_frame->data, tmp_frame->linesize,
		ENC_VIDEO_WIDTH, ENC_VIDEO_HEIGHT, ENC_AV_PIX_FMT_YUV, 1);
	if (ret < 0) {
		log_err("av_image_alloc failed, error(%s)\n", av_err2str(ret));
		goto end;
	}

	enc_pkt = av_packet_alloc();
	if (!enc_pkt) {
		goto end;
	}

	ret = init_filter_graph();
	if (ret < 0) {
		goto end;
	}

    ret = open_output_file();
	if (ret < 0) {
		goto end;
	}

	video_size = SOURCE_VIDEO_WIDTH * SOURCE_VIDEO_HEIGHT;
	while (!feof(src_file)) {
		data_size = fread(temp_buffer, 1, video_size * src_bpp / 8, src_file);
		if (!data_size) {
			break;
        }

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
