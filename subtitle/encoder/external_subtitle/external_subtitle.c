#include <stdio.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>

int open_input_video(const char *input_video_path, AVFormatContext **input_format_ctx)
{
    int ret;
    ret = avformat_open_input(input_format_ctx, input_video_path, NULL, NULL);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        fprintf(stderr, "无法打开视频文件 %s: %s\n", input_video_path, err_buf);
        return ret;
    }

    ret = avformat_find_stream_info(*input_format_ctx, NULL);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        fprintf(stderr, "无法获取视频文件流信息 %s: %s\n", input_video_path, err_buf);
        return ret;
    }

    return 0;
}

int open_input_subtitle(const char *input_subtitle_path, AVFormatContext **input_format_ctx)
{
    int ret;
    ret = avformat_open_input(input_format_ctx, input_subtitle_path, NULL, NULL);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        fprintf(stderr, "无法打开字幕文件 %s: %s\n", input_subtitle_path, err_buf);
        return ret;
    }

    ret = avformat_find_stream_info(*input_format_ctx, NULL);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        fprintf(stderr, "无法获取字幕文件流信息 %s: %s\n", input_subtitle_path, err_buf);
        return ret;
    }

    return 0;
}

int init_output_video(const char *output_video_path, AVFormatContext **output_format_ctx)
{
    int ret;

    ret = avformat_alloc_output_context2(output_format_ctx, NULL, NULL, output_video_path);
    if (!(*output_format_ctx)) {
        fprintf(stderr, "无法分配输出视频格式上下文\n");
        return -1;
    }

    ret = avio_open(&((*output_format_ctx)->pb), output_video_path, AVIO_FLAG_WRITE);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        fprintf(stderr, "无法打开输出视频文件 %s: %s\n", output_video_path, err_buf);
        return ret;
    }

    ret = avformat_write_header(*output_format_ctx, NULL);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        fprintf(stderr, "无法写入输出视频文件头 %s: %s\n", output_video_path, err_buf);
        return ret;
    }

    return 0;
}

// 拷贝流数据（视频、音频、字幕等）从输入到输出
int copy_streams(AVFormatContext *input_format_ctx, AVFormatContext *output_format_ctx) {
    int ret;
    int i;
    // 遍历输入视频文件的所有流
    for (i = 0; i < input_format_ctx->nb_streams; i++) {
        AVStream *in_stream = input_format_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(output_format_ctx, NULL);
        if (!out_stream) {
            fprintf(stderr, "无法创建输出视频流\n");
            return -1;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            char err_buf[1024];
            av_strerror(ret, err_buf, sizeof(err_buf));
            fprintf(stderr, "无法拷贝流参数: %s\n", err_buf);
            return ret;
        }

        out_stream->codecpar->codec_tag = 0;
    }

    return 0;
}

// 处理并添加字幕到输出视频
int process_subtitles(AVFormatContext *input_video_ctx, AVFormatContext *input_subtitle_ctx,
                      AVFormatContext *output_video_ctx) {
    int ret;
    // 这里假设字幕流在输入字幕文件的第0个流中（实际可能需要查找正确的字幕流索引）
    AVStream *subtitle_stream = input_subtitle_ctx->streams[0];
    AVStream *out_subtitle_stream = output_video_ctx->streams[subtitle_stream->index];

    // 遍历字幕流中的每个数据包（这里只是简单示例，实际可能需要更精细处理时间等）
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;
    while (av_read_frame(input_subtitle_ctx, &packet) >= 0) {
        if (packet.stream_index == subtitle_stream->index) {
            // 设置数据包的时间戳等信息基于输出视频流的时间基准
            packet.pts = av_rescale_q_rnd(packet.pts, subtitle_stream->time_base,
                                         out_subtitle_stream->time_base, AV_ROUND_NEAR_INF);
            packet.dts = av_rescale_q_rnd(packet.dts, subtitle_stream->time_base,
                                         out_subtitle_stream->time_base, AV_ROUND_NEAR_INF);
            packet.duration = av_rescale_q(packet.duration, subtitle_stream->time_base,
                                           out_subtitle_stream->time_base);
            packet.stream_index = out_subtitle_stream->index;

            ret = av_interleaved_write_frame(output_video_ctx, &packet);
            if (ret < 0) {
                char err_buf[1024];
                av_strerror(ret, err_buf, sizeof(err_buf));
                fprintf(stderr, "无法写入字幕数据包到输出视频: %s\n", err_buf);
                return ret;
            }
        }

        av_packet_unref(&packet);
    }

    return 0;
}

// 关闭所有上下文并释放资源
void close_all(AVFormatContext *input_video_ctx, AVFormatContext *input_subtitle_ctx,
               AVFormatContext *output_video_ctx) {
    av_write_trailer(output_video_ctx);
    avio_close(output_video_ctx->pb);
    avformat_free_context(output_video_ctx);

    avformat_close_input(&input_video_ctx);
    avformat_close_input(&input_subtitle_ctx);
}

int main(int argc, char **argv)
{
    if (argc!= 4) {
        fprintf(stderr, "用法: %s <输入视频文件路径> <输入字幕文件路径> <输出视频文件路径>\n", argv[0]);
        return -1;
    }

    const char *input_video_path = argv[1];
    const char *input_subtitle_path = argv[2];
    const char *output_video_path = argv[3];

    AVFormatContext *input_video_ctx = NULL;
    AVFormatContext *input_subtitle_ctx = NULL;
    AVFormatContext *output_video_ctx = NULL;

    int ret = open_input_video(input_video_path, &input_video_ctx);
    if (ret < 0) {
        goto end;
    }

    ret = open_input_subtitle(input_subtitle_path, &input_subtitle_ctx);
    if (ret < 0) {
        goto end;
    }

    ret = init_output_video(output_video_path, &output_video_ctx);
    if (ret < 0) {
        goto end;
    }

    ret = copy_streams(input_video_ctx, output_video_ctx);
    if (ret < 0) {
        goto end;
    }

    ret = process_subtitles(input_video_ctx, input_subtitle_ctx, output_video_ctx);
    if (ret < 0) {
        goto end;
    }

    close_all(input_video_ctx, input_subtitle_ctx, output_video_ctx);

    return 0;

end:
    if (input_video_ctx) {
        avformat_close_input(&input_video_ctx);
    }
    if (input_subtitle_ctx) {
        avformat_close_input(&input_subtitle_ctx);
    }
    if (output_video_ctx) {
        avformat_free_context(output_video_ctx);
    }
    return ret;
}
