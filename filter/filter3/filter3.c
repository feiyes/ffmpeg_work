#include <getopt.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include "log.h"

typedef struct filter_info {
    int  in_width;
    int  in_height;
    int  out_width;
    int  out_height;
    int  time_base_den;
    enum AVPixelFormat in_fmt;
    enum AVPixelFormat out_fmt;
    AVFilterContext* src_ctx;
    AVFilterContext* sink_ctx;
    AVFilterGraph* filter_graph;
} filter_info_t;

//const char* filters_descr = "[in]scale=1280:360[out]";
//const char* filters_descr = "drawbox=x=100:y=100:w=800:h=200:color=red";
const char* filters_descr = "drawtext=fontfile=方正粗黑宋简体.ttf:fontsize=32:fontcolor=red:text='Hello Word'";

static int init_filter_graph(filter_info_t* filter_info)
{
    int ret = 0;
    char args[512];
    AVFilterInOut* input  = avfilter_inout_alloc();
    AVFilterInOut* output = avfilter_inout_alloc();
    const AVFilter* buf_src  = avfilter_get_by_name("buffer");
    const AVFilter* buf_sink = avfilter_get_by_name("buffersink");
    enum AVPixelFormat pix_fmts[] = {filter_info->out_fmt, AV_PIX_FMT_NONE};

    filter_info->filter_graph = avfilter_graph_alloc();
    if (!output || !input || !filter_info->filter_graph) {
        ret = AVERROR(ENOMEM);
        log_err("avfilter_graph_alloc failed\n");
        goto clearup;
    }

    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d",
             filter_info->in_width, filter_info->in_height, filter_info->in_fmt, 1, filter_info->time_base_den);

    ret = avfilter_graph_create_filter(&filter_info->src_ctx, buf_src, "in", args, NULL, filter_info->filter_graph);
    if (ret < 0) {
        log_err("avfilter_graph_create_filter in_filter failed, error(%s)\n", av_err2str(ret));
        goto clearup;
    }

    ret = avfilter_graph_create_filter(&filter_info->sink_ctx, buf_sink, "out", NULL, NULL, filter_info->filter_graph);
    if (ret < 0) {
        log_err("avfilter_graph_create_filter out_filter failed, error(%s)\n", av_err2str(ret));
        goto clearup;
    }

    ret = av_opt_set_int_list(filter_info->sink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        log_err("av_opt_set_int_list pix_fmts of buf_sink failed, error(%s)\n", av_err2str(ret));
        goto clearup;
    }

    input->name = av_strdup("out");
    input->filter_ctx = filter_info->sink_ctx;
    input->pad_idx = 0;
    input->next = NULL;

    output->name = av_strdup("in");
    output->filter_ctx = filter_info->src_ctx;
    output->pad_idx = 0;
    output->next = NULL;
    
    ret = avfilter_graph_parse_ptr(filter_info->filter_graph, filters_descr, &input, &output, NULL);
    if (ret < 0) {
        log_err("avfilter_graph_parse_ptr failed, error(%s)", av_err2str(ret));
        goto clearup;
    }
    
    ret = avfilter_graph_config(filter_info->filter_graph, NULL);
    if (ret < 0) {
        log_err("avfilter_graph_parse_ptr, error(%s)", av_err2str(ret));
        goto clearup;
    }
    
clearup:
    avfilter_inout_free(&input);
    avfilter_inout_free(&output);
    
    return ret;
}

static struct option long_options[] = {
    {"den",           required_argument, NULL, 'd'},
    {"input",         required_argument, NULL, 'i'},
    {"output",        required_argument, NULL, 'o'},
    {"in_width",      required_argument, NULL, 'w'},
    {"in_height",     required_argument, NULL, 'h'},
    {"out_width",     required_argument, NULL, 'W'},
    {"out_height",    required_argument, NULL, 'H'},
    {NULL,            0,                 NULL,   0}
};

static void help_message()
{
    fprintf(stderr, "./filter3 -i 1280_720_yuv420p.yuv -o 1280_720_yuv420p_out.yuv -w 1280 -h 720 -f 0 -W 1280 -H 360 -F 0\n");
}

int main(int argc, char* argv[])
{
    int c;
    int ret;
    int in_bpp = 0;
    int in_luma_size  = 0;
    int out_luma_size = 0;
    size_t buf_size;
    uint8_t* buf = NULL;
    FILE* fp_in  = NULL;
    FILE* fp_out = NULL;
    AVFrame* in_frame  = NULL;
    AVFrame* out_frame = NULL;
    char in_file[256] = {'\0'};
    char out_file[256] = {'\0'};
    filter_info_t filter_info = {0};

    filter_info.in_width       = 1280;
    filter_info.in_height      = 720;
    filter_info.out_width      = 1280;
    filter_info.out_height     = 720;
    filter_info.time_base_den  = 25;
    filter_info.in_fmt         = AV_PIX_FMT_YUV420P;
    filter_info.out_fmt        = AV_PIX_FMT_YUV420P;

    while ((c = getopt_long(argc, argv, ":i:o:f:F:w:W:h:H:t:a", long_options, NULL)) != -1) {
    switch (c) {
    case 'i':
        strncpy(in_file, optarg, strlen(optarg));
        break;
    case 'o':
        strncpy(out_file, optarg, strlen(optarg));
        break;
    case 'f':
        filter_info.in_fmt = atoi(optarg);
        break;
    case 'w':
        filter_info.in_width = atoi(optarg);
        break;
    case 'h':
        filter_info.in_height = atoi(optarg);
        break;
    case 'F':
        filter_info.out_fmt = atoi(optarg);
        break;
    case 'W':
        filter_info.out_width = atoi(optarg);
        break;
    case 'H':
        filter_info.out_height = atoi(optarg);
        break;
    case 'd':
        filter_info.time_base_den = atoi(optarg);
        break;
    default:
        help_message();
        return -1;
    }
    }

    if (strcmp(in_file, "\0") == 0 || strcmp(out_file, "\0") == 0) {
        log_err("invalid mp4 %s or out file %s\n", in_file, out_file);
        help_message();
        return -1;
    }

    fp_in = fopen(in_file, "rb");
    if (!fp_in) {
        log_err("fopen %s failed\n", in_file);
        return -1;
    }

    fp_out = fopen(out_file, "wb+");
    if (!fp_out) {
        log_err("fopen %s failed\n", out_file);
        return -1;
    }

    in_frame = av_frame_alloc();
    if (!in_frame) {
        log_err("av_frame_alloc in frame failed\n");
        return -1;
    }

    out_frame = av_frame_alloc();
    if (!out_frame) {
        log_err("av_frame_alloc out frame failed\n");
        return -1;
    }

    in_frame->format  = filter_info.in_fmt;
    in_frame->width   = filter_info.in_width;
    in_frame->height  = filter_info.in_height;
    out_frame->format = filter_info.out_fmt;
    out_frame->width  = filter_info.out_width;
    out_frame->height = filter_info.out_height;

    log_info("in_fmt %d, in_width %d, in_height %d, out_fmt %d, out_width %d, out_height %d\n",
              in_frame->format, in_frame->width, in_frame->height, out_frame->format, out_frame->width, out_frame->height);

    buf = (uint8_t*)av_malloc(av_image_get_buffer_size(filter_info.in_fmt, filter_info.in_width, filter_info.in_height, 1));
    if (!buf) {
        log_err("av_frame_alloc out frame failed\n");
        return -1;
    }

    ret = av_image_alloc(in_frame->data, in_frame->linesize, filter_info.in_width, filter_info.in_height, filter_info.in_fmt, 1);
    if (ret < 0) {
        log_err("av_image_alloc input frame failed");
        goto end;
    }

    ret = av_image_alloc(out_frame->data, out_frame->linesize, filter_info.out_width, filter_info.out_height, filter_info.out_fmt, 1);
    if (ret < 0) {
        log_err("av_image_alloc output frame failed");
        goto end;
    }

    ret = init_filter_graph(&filter_info);
    if (ret < 0) {
        goto end;
    }

    in_luma_size  = filter_info.in_width * filter_info.in_height;
    out_luma_size = out_frame->width * out_frame->height;
    in_bpp = av_get_bits_per_pixel(av_pix_fmt_desc_get(filter_info.in_fmt));

    while (!feof(fp_in)) {
        buf_size = fread(buf, 1, in_luma_size * in_bpp / 8, fp_in);
        if (!buf_size)
            break;

        memcpy(in_frame->data[0], buf,                        in_luma_size);      // Y
        memcpy(in_frame->data[1], buf + in_luma_size,         in_luma_size / 4);  // U
        memcpy(in_frame->data[2], buf + in_luma_size * 5 / 4, in_luma_size / 4);  // V

        ret = av_buffersrc_add_frame(filter_info.src_ctx, in_frame);
        if (ret < 0) {
            av_frame_unref(in_frame);
            log_err("av_buffersrc_add_frame failed, error(%s)", av_err2str(ret));
            return ret;
        }

        while ((ret = av_buffersink_get_frame(filter_info.sink_ctx, out_frame)) >= 0) {
            fwrite(out_frame->data[0], 1, out_luma_size,     fp_out);  // Y
            fwrite(out_frame->data[1], 1, out_luma_size / 4, fp_out);  // U
            fwrite(out_frame->data[2], 1, out_luma_size / 4, fp_out);  // V

            av_frame_unref(out_frame);
        }

        if (ret == AVERROR(EAGAIN)) {
            continue;
        } else if (ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            log_err("av_buffersink_get_frame failed, error(%s)", av_err2str(ret));
            goto end;
        }
    }

end:
    avfilter_graph_free(&filter_info.filter_graph);
    av_frame_free(&out_frame);
    av_frame_free(&in_frame);
    fclose(fp_in);
    fclose(fp_out);

    return 0;
}
