#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "log.h"

// Notice: mp4 only support mov_text subtitle format.

void soft_subtitle(const char* video_file, const char* subtitle_file, const char* out_file)
{
    int ret = 0;
    AVFormatContext* v_fmt = NULL;
    AVFormatContext* s_fmt = NULL;
    AVFormatContext* o_fmt = NULL;
    int in_video_index    = -1, in_audio_index     = -1, in_subtitle_index  = -1;
    int out_video_index   = -1, out_audio_index    = -1, out_subtitle_index = -1;

    log_info("add audio & video stream");
    // audio & video stream
    ret = avformat_open_input(&v_fmt, video_file, NULL, NULL);
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

            ret = avcodec_parameters_copy(video_stream->codecpar, stream->codecpar);
            if (ret < 0) {
                log_err("avcodec_parameters_copy failed, ret = %d(%s)", ret, av_err2str(ret));
                return;
            }

            in_video_index = i;
            out_video_index = video_stream->index;
            video_stream->codecpar->codec_tag = 0;

            log_info("video codec(%s)", avcodec_get_name(video_stream->codecpar->codec_id));
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

            log_info("audio codec(%s)", avcodec_get_name(audio_stream->codecpar->codec_id));
        }
    }

    if (!(o_fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&o_fmt->pb, out_file, AVIO_FLAG_WRITE);
        if (ret < 0) {
            log_err("avio_open failed, ret = %d(%s)", ret, av_err2str(ret));
            return;
        }
    }

    // subtitle stream
    log_info("add subtitle stream");
    ret = avformat_open_input(&s_fmt, subtitle_file, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input failed, ret = %d(%s)", ret, av_err2str(ret));
        return;
    }

    ret = avformat_find_stream_info(s_fmt, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info failed, ret = %d(%s)", ret, av_err2str(ret));
        return;
    }

    in_subtitle_index = av_find_best_stream(s_fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    if (in_subtitle_index < 0) {
        log_err("not find subtitle stream 0");
        return;
    }

    AVStream* subtitle_stream = avformat_new_stream(o_fmt, NULL);
    if (!subtitle_stream) {
        log_err("avformat_new_stream failed");
        return;
    }

    ret = avcodec_parameters_copy(subtitle_stream->codecpar, s_fmt->streams[in_subtitle_index]->codecpar);
    if (ret < 0) {
        log_err("avcodec_parameters_copy failed, ret = %d(%s)", ret, av_err2str(ret));
        return;
    }

    subtitle_stream->codecpar->codec_tag = 0;
    out_subtitle_index = subtitle_stream->index;

    log_info("subtitle codec(%s)", avcodec_get_name(subtitle_stream->codecpar->codec_id));

    ret = avformat_write_header(o_fmt, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, ret = %d(%s)", ret, av_err2str(ret));
        return;
    }

    av_dump_format(o_fmt, 0, out_file, 1);

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed");
        return;
    }

    // Notice: MKV need to write subtitle before audio and video stream.
    while (av_read_frame(s_fmt, packet) >= 0) {
        AVStream* in_stream  = s_fmt->streams[in_subtitle_index];
        AVStream* out_stream = o_fmt->streams[out_subtitle_index];

        packet->pos = -1;
        packet->stream_index = out_subtitle_index;
        av_packet_rescale_ts(packet, in_stream->time_base, out_stream->time_base);

        //log_info("pts %ld", packet->pts);

        ret = av_write_frame(o_fmt, packet);
        if (ret < 0) {
            log_err("av_write_frame failed(subtitle), ret = %d(%s)", ret, av_err2str(ret));
            av_packet_unref(packet);
            return;
        }
    }

    av_packet_unref(packet);

    while (av_read_frame(v_fmt, packet) >= 0) {
        if (packet->stream_index == in_video_index) {
            AVStream* in_stream  = v_fmt->streams[in_video_index];
            AVStream* out_stream = o_fmt->streams[out_video_index];

            packet->stream_index = out_video_index;
            av_packet_rescale_ts(packet, in_stream->time_base, out_stream->time_base);

            //log_info("packet %ld", packet->pts);

            ret = av_write_frame(o_fmt, packet);
            if (ret < 0) {
                log_err("av_write_frame failed(video), ret = %d(%s)", ret, av_err2str(ret));
                av_packet_unref(packet);
                return;
            }
        } else if (packet->stream_index == in_audio_index) {
            AVStream* in_stream  = v_fmt->streams[in_audio_index];
            AVStream* out_stream = o_fmt->streams[out_audio_index];

            packet->stream_index = out_audio_index;
            av_packet_rescale_ts(packet, in_stream->time_base, out_stream->time_base);

            ret = av_write_frame(o_fmt, packet);
            if (ret < 0) {
                log_err("av_write_frame failed(audio), ret = %d(%s)", ret, av_err2str(ret));
                av_packet_unref(packet);
                return;
            }
        }
    }

    av_packet_unref(packet);
    av_packet_free(&packet);
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
    if (argc < 4) {
        return -1;
    }

    soft_subtitle(argv[1], argv[2], argv[3]);

    return 0;
}
