#include <stdio.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

AVFilterContext *src_ctx = NULL;
AVFilterContext *sink_ctx = NULL;
AVFilterGraph *filter_graph = NULL;

int init_filters(const int width, const int height, const int format)
{
    int ret = 0;
    char filter_args[1024] = {0};
    AVFilterInOut *inputs = NULL;
    AVFilterInOut *outputs = NULL;

    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        printf("avfilter_graph_alloc failed\n");
        return -1;
    }

#if 0
    snprintf(filter_args, sizeof(filter_args),
             "buffer=video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d[v0];" // Parsed_buffer_0
             "[v0]split[main][tmp];"                // Parsed_split_1
             "[tmp]crop=iw:ih/2:0:0,vflip[flip];"   // Parsed_crop_2 Parsed_vflip_3
             "[main][flip]overlay=0:H/2[result];"   // Parsed_overlay_4
             "[result]buffersink",                  // Parsed_buffersink_5
             width, height, format, 1, 25, 1, 1);
#else
    snprintf(filter_args, sizeof(filter_args),
             "buffer=video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d[v0];" // Parsed_buffer_0
             "[v0]split[main][tmp];"                // Parsed_split_1
             "[tmp]crop=iw:ih/2:0:0,vflip[flip];"   // Parsed_crop_2 Parsed_vflip_3
             "[main]buffersink;"                    // Parsed_buffersink_4
             "[flip]buffersink",                    // Parsed_buffersink_5
             width, height, format, 1, 25, 1, 1);
#endif

    ret = avfilter_graph_parse2(filter_graph, filter_args, &inputs, &outputs);
    if (ret < 0) {
        printf("avfilter_graph_parse2 failed, error(%s)\n", av_err2str(ret));
        return ret;
    }

    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        printf("avfilter_graph_config, error(%s)\n", av_err2str(ret));
        return ret;
    }

    // Get AVFilterContext from AVFilterGraph parsing from string
    src_ctx = avfilter_graph_get_filter(filter_graph, "Parsed_buffer_0");
    if (!src_ctx) {
        printf("avfilter_graph_get_filter Parsed_buffer_0 failed\n");
        return -1;
    }

    sink_ctx = avfilter_graph_get_filter(filter_graph, "Parsed_buffersink_5");
    if (!sink_ctx) {
        printf("avfilter_graph_get_filter Parsed_buffersink_5 failed\n");
        return -1;
    }

    printf("sink_width:%d, sink_height:%d\n",
            av_buffersink_get_w(sink_ctx), av_buffersink_get_h(sink_ctx));

    return 0;
}

// ffmpeg -i 9.5.flv -vf "split[main][tmp];[tmp]crop=iw:ih/2:0:0,vflip [flip];[main][flip]overlay=0:H/2" -b:v 500k -vcodec libx264 9.5_out.flv
int main(int argc, char** argv)
{
    int ret = 0;
    int in_width = 800;
    int in_height = 450;
    FILE* fp_in = NULL;
    FILE* fp_out = NULL;
    uint32_t frame_count = 0;
    const char* in_file = "800x450.yuv";
    const char* out_file = "crop_vfilter.yuv";

    ret = init_filters(in_width, in_height, AV_PIX_FMT_YUV420P);
    if (ret < 0) {
        printf("init_filters failed\n");
        return -1;
    }

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

    FILE* graph_file = fopen("graph_file.txt", "w");
    char *graph_str = avfilter_graph_dump(filter_graph, NULL);
    fprintf(graph_file, "%s", graph_str);
    av_free(graph_str);

    AVFrame *frame_in = av_frame_alloc();
    unsigned char *frame_buffer_in = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width, in_height, 1));
    av_image_fill_arrays(frame_in->data, frame_in->linesize, frame_buffer_in, AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    AVFrame *frame_out = av_frame_alloc();
    unsigned char *frame_buffer_out = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width, in_height, 1));
    av_image_fill_arrays(frame_out->data, frame_out->linesize, frame_buffer_out, AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    frame_in->width  = in_width;
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
            printf("process %d frame\n", frame_count);

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
