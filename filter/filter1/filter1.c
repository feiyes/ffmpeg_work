#include <stdio.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

int main(int argc, char** argv)
{
    int ret = 0;
    char args[512];
    int in_width = 800;
    int in_height = 450;
    FILE* fp_in = NULL;
    FILE* fp_out = NULL;
    uint32_t frame_count = 0;
    const char* in_file = "800x450.yuv";
    const char* out_file = "crop_vfilter.yuv";

    fp_in = fopen(in_file, "rb+");
    if (!fp_in) {
        printf("fopen %s failed\n", in_file);
        return -1;
    }

    fp_out = fopen(out_file, "wb");
    if (!fp_out) {
        printf("fopen %s failed\n", out_file);
        return -1;
    }

    av_log_set_level(AV_LOG_TRACE);

    AVFilterGraph* filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        printf("avfilter_graph_alloc failed\n");
        return -1;
    }

    AVFilterContext* src_ctx;
    const AVFilter* src_buf = avfilter_get_by_name("buffer");
    sprintf(args, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                   in_width, in_height, AV_PIX_FMT_YUV420P, 1, 25, 1, 1);
    ret = avfilter_graph_create_filter(&src_ctx, src_buf, "in", args, NULL, filter_graph);
    if (ret < 0) {
        printf("avfilter_graph_create_filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    AVFilterContext* sink_ctx;
    const AVFilter* dst_buf = avfilter_get_by_name("buffersink");
    ret = avfilter_graph_create_filter(&sink_ctx, dst_buf, "out", NULL, "pix_fmts=0", filter_graph);
    if (ret < 0) {
        printf("avfilter_graph_create_filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    AVFilterContext *split_ctx;
    const AVFilter *split_filter = avfilter_get_by_name("split");
    ret = avfilter_graph_create_filter(&split_ctx, split_filter, "split", "outputs=2", NULL, filter_graph);
    if (ret < 0) {
        printf("avfilter_graph_create_filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    AVFilterContext *crop_ctx;
    const AVFilter *crop_filter = avfilter_get_by_name("crop");
    ret = avfilter_graph_create_filter(&crop_ctx, crop_filter, "crop", "out_w=iw:out_h=ih/2:x=0:y=0", NULL, filter_graph);
    if (ret < 0) {
        printf("avfilter_graph_create_filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    AVFilterContext *vflip_ctx;
    const AVFilter *vflip_filter = avfilter_get_by_name("vflip");
    ret = avfilter_graph_create_filter(&vflip_ctx, vflip_filter, "vflip", NULL, NULL, filter_graph);
    if (ret < 0) {
        printf("avfilter_graph_create_filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    AVFilterContext *overlay_ctx;
    const AVFilter *overlay_filter = avfilter_get_by_name("overlay");
    ret = avfilter_graph_create_filter(&overlay_ctx, overlay_filter, "overlay", "0:H/2", NULL, filter_graph);
    if (ret < 0) {
        printf("avfilter_graph_create_filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    // src filter to split filter
    ret = avfilter_link(src_ctx, 0, split_ctx, 0);
    if (ret != 0) {
        printf("avfilter_link src filter to split filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    // split filter's first pad to overlay filter's main pad
    ret = avfilter_link(split_ctx, 0, overlay_ctx, 0);
    if (ret != 0) {
        printf("avfilter_link split filter to overlay filter main pad failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    // split filter's second pad to crop filter
    ret = avfilter_link(split_ctx, 1, crop_ctx, 0);
    if (ret != 0) {
        printf("avfilter_link split filter's second pad to crop filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    // crop filter to vflip filter
    ret = avfilter_link(crop_ctx, 0, vflip_ctx, 0);
    if (ret != 0) {
        printf("avfilter_link crop filter to vflip filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    // vflip filter to overlay filter's second pad
    ret = avfilter_link(vflip_ctx, 0, overlay_ctx, 1);
    if (ret != 0) {
        printf("avfilter_link vflip filter to overlay filter's second pad failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    // overlay filter to sink filter
    ret = avfilter_link(overlay_ctx, 0, sink_ctx, 0);
    if (ret != 0) {
        printf("avfilter_link overlay filter to sink filter failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    // check filter graph
    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        printf("avfilter_graph_config failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    FILE* graphFile = fopen("graph_file.txt", "w");
    char *graph_str = avfilter_graph_dump(filter_graph, NULL);
    fprintf(graphFile, "%s", graph_str);
    av_free(graph_str);

    AVFrame *frame_in = av_frame_alloc();
    unsigned char *frame_buffer_in = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width, in_height, 1));
    av_image_fill_arrays(frame_in->data, frame_in->linesize, frame_buffer_in,
                         AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    AVFrame *frame_out = av_frame_alloc();
    unsigned char *frame_buffer_out = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width, in_height, 1));
    av_image_fill_arrays(frame_out->data, frame_out->linesize, frame_buffer_out,
                         AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    frame_in->width = in_width;
    frame_in->height = in_height;
    frame_in->format = AV_PIX_FMT_YUV420P;

    while (1) {
        if (fread(frame_buffer_in, 1, in_width*in_height * 3 / 2, fp_in) != in_width*in_height * 3 / 2) {
            break;
        }

        frame_in->data[0] = frame_buffer_in;
        frame_in->data[1] = frame_buffer_in + in_width*in_height;
        frame_in->data[2] = frame_buffer_in + in_width*in_height * 5 / 4;

        ret = av_buffersrc_add_frame(src_ctx, frame_in);
        if (ret < 0) {
            printf("av_buffersrc_add_frame failed, error(%s)\n", av_err2str(ret));
            break;
        }

        ret = av_buffersink_get_frame(sink_ctx, frame_out);
        if (ret < 0)
            break;

        if (frame_out->format == AV_PIX_FMT_YUV420P) {
            for (int i = 0; i < frame_out->height; i++) {
                fwrite(frame_out->data[0] + frame_out->linesize[0] * i, 1, frame_out->width, fp_out);
            }

            for (int i = 0; i < frame_out->height / 2; i++) {
                fwrite(frame_out->data[1] + frame_out->linesize[1] * i, 1, frame_out->width / 2, fp_out);
            }

            for (int i = 0; i < frame_out->height / 2; i++) {
                fwrite(frame_out->data[2] + frame_out->linesize[2] * i, 1, frame_out->width / 2, fp_out);
            }
        }

        if (frame_count % 25 == 0)
            printf("process %d frame!\n",frame_count);

        ++frame_count;
        av_frame_unref(frame_out);
    }

    fclose(fp_in);
    fclose(fp_out);

    av_frame_free(&frame_in);
    av_frame_free(&frame_out);
    avfilter_graph_free(&filter_graph);

    return 0;
}
