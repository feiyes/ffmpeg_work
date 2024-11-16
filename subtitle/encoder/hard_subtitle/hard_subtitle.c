#include <stdbool.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

#include "log.h"

char srt_file[128];
char m_fontsConf[128];
AVFormatContext* s_fmt = NULL;
AVFormatContext* o_fmt = NULL;
int in_video_index, in_audio_index;
int out_video_index, out_audio_index;
AVCodecContext* de_video_ctx = NULL;
AVCodecContext* en_video_ctx = NULL;
AVFrame* de_frame = NULL;
AVFilterContext* src_filter_ctx = NULL;
AVFilterContext* sink_filter_ctx = NULL;

void encode_process(AVFormatContext* o_fmt, AVCodecContext* en_video_ctx, AVFrame* frame)
{
    int ret = avcodec_send_frame(en_video_ctx, frame);
    if (ret < 0) {
        log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (true) {
        AVPacket* pkt = av_packet_alloc();
        ret = avcodec_receive_packet(en_video_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
            av_packet_unref(pkt);
            break;
        }

        //log_info("video pts %d(%s)", pkt->pts, av_ts2timestr(pkt->pts, &o_fmt->streams[out_video_index]->time_base));

        pkt->stream_index = out_video_index;
        av_packet_rescale_ts(pkt, en_video_ctx->time_base, o_fmt->streams[out_video_index]->time_base);
        av_write_frame(o_fmt, pkt);

        av_packet_unref(pkt);
    }
}

void decode_process(AVPacket* pkt)
{
    if (!de_frame) {
        de_frame = av_frame_alloc();
    }

    int ret = avcodec_send_packet(de_video_ctx, pkt);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (true) {
        ret = avcodec_receive_frame(de_video_ctx, de_frame);
        if (ret == AVERROR_EOF) {
            ret = av_buffersrc_add_frame_flags(src_filter_ctx, NULL, AV_BUFFERSRC_FLAG_KEEP_REF);
            if (ret < 0) {
                log_err("av_buffersrc_add_frame_flags failed");
                break;
            }
            break;
        } else if (ret < 0) {
            break;
        }

        ret = av_buffersrc_add_frame_flags(src_filter_ctx, de_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) {
            log_err("av_buffersrc_add_frame_flags failed");
            break;
        }

        while (true) {
            AVFrame* enframe = av_frame_alloc();
            ret = av_buffersink_get_frame(sink_filter_ctx, enframe);
            if (ret == AVERROR_EOF) {
                encode_process(o_fmt, en_video_ctx, NULL);
                av_frame_unref(enframe);
            } else if (ret < 0) {
                av_frame_unref(enframe);
                break;
            }

            encode_process(o_fmt, en_video_ctx, enframe);

            av_frame_unref(enframe);
        }
    }
}

int init_filter_graph(AVStream* stream, char* srt_file)
{
    int ret = 0;
    const AVFilter* buf_src = avfilter_get_by_name("buffer");
    const AVFilter* buf_sink = avfilter_get_by_name("buffersink");
    AVFilterGraph* filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        log_err("avfilter_graph_alloc failed\n");
        return -1;
    }

    char args[512];
    sprintf(args, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                  stream->codecpar->width, stream->codecpar->height,
                  stream->codecpar->format, stream->time_base.num, stream->time_base.den,
                  stream->codecpar->sample_aspect_ratio.num, stream->codecpar->sample_aspect_ratio.den);
    ret = avfilter_graph_create_filter(&src_filter_ctx, buf_src, "in", args, NULL, filter_graph);
    if (ret < 0) {
        log_err("avfilter_graph_create_filter in_filter failed, ret = %d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avfilter_graph_create_filter(&sink_filter_ctx, buf_sink, "out", NULL, NULL, filter_graph);
    if (ret < 0) {
        log_err("avfilter_graph_create_filter out_filter failed, ret = %d(%s)", ret, av_err2str(ret));
        return -1;
    }

    // TODO
    //srt_file.replace(":", "\\:");

    AVFilterInOut* input = avfilter_inout_alloc();
    if (!input) {
        log_err("avfilter_inout_alloc failed");
        return -1;
    }

    input->next = NULL;
    input->pad_idx = 0;
    input->name = av_strdup("out");
    input->filter_ctx = sink_filter_ctx;

    AVFilterInOut* output = avfilter_inout_alloc();
    if (!output) {
        log_err("avfilter_inout_alloc failed");
        return -1;
    }

    output->next = NULL;
    output->pad_idx = 0;
    output->name = av_strdup("in");
    output->filter_ctx = src_filter_ctx;

    char filter_des[512];
    //sprintf(filter_des, "subtitles=filename='sintel.srt'");        //只能使用当前路径的srt文件
    //sprintf(filter_des, "drawtext=fontfile=arial.ttf:fontcolor=white:fontsize=30:text='Aispeech'");
    sprintf(filter_des, "drawbox=x=100:y=100:w=100:h=100:color=pink@0.5");    //加一个方框 可以运行
    ret = avfilter_graph_parse_ptr(filter_graph, filter_des, &input, &output, NULL);
    if (ret < 0) {
        log_err("avfilter_graph_parse_ptr failed, ret = %d(%s)", ret, av_err2str(ret));
        return -1;
    }

    av_buffersink_set_frame_size(sink_filter_ctx, en_video_ctx->frame_size);

    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        log_err("avfilter_graph_config failed, error(%s)", av_err2str(ret));
        return -1;
    }

    avfilter_inout_free(&input);
    avfilter_inout_free(&output);

    return 0;
}

void hard_subtitle(const char* src_file, const char* out_file)
{
    int ret = 0;
    AVFormatContext* v_fmt = NULL;

    ret = avformat_open_input(&v_fmt, src_file, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input failed, ret = %d(%s)", ret, av_err2str(ret));
        return;
    }

    ret = avformat_find_stream_info(v_fmt, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info failed, ret = %d(%s)", ret, av_err2str(ret));
        return;
    }

    ret = avformat_alloc_output_context2(&o_fmt, NULL, NULL, out_file);
    if (ret < 0) {
        log_err("avformat_alloc_output_context2 failed, ret = %d(%s)", ret, av_err2str(ret));
        return;
    }

    for (int i = 0; i < v_fmt->nb_streams; i++) {
        AVStream* stream = v_fmt->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVStream* video_stream = avformat_new_stream(o_fmt, NULL);
            if (!video_stream) {
                log_err("avformat_new_stream failed");
                return;
            }

            const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!codec) {
                log_err("avcodec_find_decoder failed");
                return;
            }

            de_video_ctx = avcodec_alloc_context3(codec);
            if (!de_video_ctx) {
                log_err("avcodec_alloc_context3 failed");
                return;
            }

            ret = avcodec_parameters_to_context(de_video_ctx, stream->codecpar);
            if (ret < 0) {
                log_err("avcodec_parameters_to_context failed, ret = %d(%s)", ret, av_err2str(ret));
                return;
            }

            ret = avcodec_open2(de_video_ctx, codec, NULL);
            if (ret < 0) {
                log_err("avcodec_open2 failed, ret = %d(%s)", ret, av_err2str(ret));
                return;
            }

            const AVCodec* encodec = avcodec_find_encoder(stream->codecpar->codec_id);
            if (!encodec) {
                log_err("avcodec_find_encoder failed");
                return;
            }

            en_video_ctx = avcodec_alloc_context3(encodec);
            if (!en_video_ctx) {
                log_err("avcodec_alloc_context3 failed");
                return;
            }

            ret = avcodec_parameters_to_context(en_video_ctx, stream->codecpar);
            if (ret < 0) {
                log_err("avcodec_parameters_to_context failed, ret = %d(%s)", ret, av_err2str(ret));
                return;
            }

            en_video_ctx->gop_size = 12;
            en_video_ctx->framerate.den = 1;
            en_video_ctx->framerate.num = stream->r_frame_rate.num;
            en_video_ctx->time_base = stream->time_base;
            if (o_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
                en_video_ctx->flags = AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            ret = avcodec_open2(en_video_ctx, encodec, NULL);
            if (ret < 0) {
                log_err("avcodec_open2 failed, error(%s)\n", av_err2str(ret));
                return;
            }

            ret = avcodec_parameters_from_context(video_stream->codecpar, en_video_ctx);
            if (ret < 0) {
                log_err("avcodec_parameters_copy failed, ret = %d(%s)", ret, av_err2str(ret));
                return;
            }

            in_video_index = i;
            out_video_index = video_stream->index;
            video_stream->codecpar->codec_tag = 0;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            AVStream* audio_stream = avformat_new_stream(o_fmt, NULL);
            if (!audio_stream) {
                log_err("avformat_new_stream failed");
                return;
            }

            ret = avcodec_parameters_copy(audio_stream->codecpar, stream->codecpar);
            if (ret < 0) {
                log_err("avcodec_parameters_copy failed, ret = %d(%s)", ret, av_err2str(ret));
                return;
            }

            in_audio_index = i;
            out_audio_index = audio_stream->index;
            audio_stream->codecpar->codec_tag = 0;
        }
    }

    if (in_video_index == -1) {
        log_err("no valid video stream");
        return;
    }

    if (!(o_fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&o_fmt->pb, out_file, AVIO_FLAG_WRITE);
        if (ret < 0) {
            log_err("avio_open %s failed, ret = %d(%s)\n", out_file, ret, av_err2str(ret));
            return;
        }
    }

    av_dump_format(o_fmt, -1, out_file, 1);

    ret = avformat_write_header(o_fmt, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, ret = %d(%s)", ret, av_err2str(ret));
        return;
    }

    ret = init_filter_graph(v_fmt->streams[in_video_index], srt_file);
    if (ret < 0) {
        return;
    }

    AVPacket* inpkt = av_packet_alloc();

    while (av_read_frame(v_fmt, inpkt) >= 0) {
        if (inpkt->stream_index == in_video_index) {
            decode_process(inpkt);
        } else if (inpkt->stream_index == in_audio_index) {
            av_packet_rescale_ts(inpkt, v_fmt->streams[in_audio_index]->time_base, o_fmt->streams[out_audio_index]->time_base);
            inpkt->stream_index = out_audio_index;
            //log_err("audio pts %d(%s)", inpkt->pts, av_ts2timestr(inpkt->pts, &o_fmt->streams[out_audio_index]->time_base));
            av_write_frame(o_fmt, inpkt);
        }
    }

    av_packet_unref(inpkt);

    log_info("finish");
    decode_process(NULL);

    av_write_trailer(o_fmt);

    if (v_fmt) {
        avformat_close_input(&v_fmt);
        avformat_free_context(v_fmt);
    }

    if (s_fmt) {
        avformat_close_input(&s_fmt);
        avformat_free_context(s_fmt);
    }

    if (o_fmt) {
        avformat_close_input(&o_fmt);
        avformat_free_context(o_fmt);
    }
}

int main(int argc, char* argv[])
{
    char src_file[128] = "../../../stream/video/mp4/sintel.ts";
    char out_file[128] = "./sintel.mp4";

    hard_subtitle(src_file, out_file);

    return 0;
}
