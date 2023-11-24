#include <getopt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>
#include <libavutil/imgutils.h>
#ifdef __cplusplus
};
#endif

#include "log.h"

AVFrame* sws_frame = NULL;
struct SwsContext *img_convert_ctx;
// #define DUMP_AUDIO 1
// #define DUMP_VIDEO 1
#define OUT_FILE "out"
#define REFRESH_EVENT  (SDL_USEREVENT + 1)
#define BREAK_EVENT    (SDL_USEREVENT + 2)

typedef struct {
    FILE* fp_aout;
    FILE* fp_vout;
    SDL_Rect rect;
    SDL_Window*   screen;
    SDL_Texture*  texture;
    SDL_Renderer* renderer;
} ff_render_info;

int thread_exit = 0;

int refresh_video(void *opaque)
{
    SDL_Event event;

    while (!thread_exit) {
        event.type = REFRESH_EVENT;
        SDL_PushEvent(&event);
        SDL_Delay(40);
    }

    thread_exit = 0;
    event.type = BREAK_EVENT;
    SDL_PushEvent(&event);

    return 0;
}

#ifdef DUMP_VIDEO
/**
 * save yuv420p frame [YUV]
 */
void yuv420p_save(FILE* fp, AVFrame *frame, AVCodecContext *codec_ctx)
{
    int i = 0;

    int width = codec_ctx->width;
    int height = codec_ctx->height;
    int width_half = width / 2;
    int height_half = height / 2;
    int y_wrap = frame->linesize[0];
    int u_wrap = frame->linesize[1];
    int v_wrap = frame->linesize[2];

    unsigned char *y_buf = frame->data[0];
    unsigned char *u_buf = frame->data[1];
    unsigned char *v_buf = frame->data[2];

    // save y
    fprintf(stderr, "=====save Y success=====\n");
    for (i = 0; i < height; i++)
        fwrite(y_buf + i * y_wrap, 1, width, fp);

    // save u
    fprintf(stderr, "=====save U success=====\n");
    for (i = 0; i < height_half; i++)
        fwrite(u_buf + i * u_wrap, 1, width_half, fp);

    // save v
    fprintf(stderr, "=====save V success=====\n");
    for (i = 0; i < height_half; i++)
        fwrite(v_buf + i * v_wrap, 1, width_half, fp);

    fflush(fp);
}

/**
 * save yuv422p frame [YUV]
 */
void yuv422p_save(FILE* fp, AVFrame *frame, AVCodecContext *codec_ctx)
{
    int i = 0;

    int width = codec_ctx->width;
    int height = codec_ctx->height;
    int width_half = width / 2;
    int y_wrap = frame->linesize[0];
    int u_wrap = frame->linesize[1];
    int v_wrap = frame->linesize[2];

    unsigned char *y_buf = frame->data[0];
    unsigned char *u_buf = frame->data[1];
    unsigned char *v_buf = frame->data[2];

    // save y
    fprintf(stderr, "=====save Y success=====\n");
    for (i = 0; i < height; i++)
        fwrite(y_buf + i * y_wrap, 1, width, fp);

    // save u
    fprintf(stderr, "=====save U success=====\n");
    for (i = 0; i < height; i++)
        fwrite(u_buf + i * u_wrap, 1, width_half, fp);

    // save v
    fprintf(stderr, "=====save V success=====\n");
    for (i = 0; i < height; i++)
        fwrite(v_buf + i * v_wrap, 1, width_half, fp);

    fflush(fp);
}

/**
 * save yuv444p frame [YUV]
 */
void yuv444p_save(FILE* fp, AVFrame *frame, AVCodecContext *codec_ctx)
{
    int i = 0;

    int width = codec_ctx->width;
    int height = codec_ctx->height;
    int y_wrap = frame->linesize[0];
    int u_wrap = frame->linesize[1];
    int v_wrap = frame->linesize[2];

    unsigned char *y_buf = frame->data[0];
    unsigned char *u_buf = frame->data[1];
    unsigned char *v_buf = frame->data[2];

    // save y
    fprintf(stderr, "=====save Y success=====\n");
    for (i = 0; i < height; i++)
        fwrite(y_buf + i * y_wrap, 1, width, fp);

    // save u
    fprintf(stderr, "=====save U success=====\n");
    for (i = 0; i < height; i++)
        fwrite(u_buf + i * u_wrap, 1, width, fp);

    // save v
    fprintf(stderr, "=====save V success=====\n");
    for (i = 0; i < height; i++)
        fwrite(v_buf + i * v_wrap, 1, width, fp);

    fflush(fp);
}

/**
 * save rgb24 frame [PPM]
 */
void rgb24_save(FILE* fp, AVFrame *frame, AVCodecContext *codec_ctx)
{
    int i = 0;
    int width = codec_ctx->width;
    int height = codec_ctx->height;

    // write PPM header
    fprintf(fp, "P6n%d %dn255\n", width, height);

    // write pixel data
    for(i = 0; i < height; i++)
        fwrite(frame->data[0] + i * frame->linesize[0], 1, width * 3, fp);

    fflush(fp);
}
#endif

void render_video(SDL_Renderer* renderer, SDL_Texture* texture, SDL_Rect* rect, AVFrame *frame)
{
    //sws_scale(img_convert_ctx, (const unsigned char* const*)sws_frame->data, sws_frame->linesize,
    //          0, codec_ctx->height, sws_frame->data, sws_frame->linesize);

    SDL_UpdateYUVTexture(texture, rect,
                         frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1],
                         frame->data[2], frame->linesize[2]);

    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, rect);
    SDL_RenderPresent(renderer);

    SDL_Delay(40);
}

static void decode_process(AVCodecContext* codec_ctx, AVFrame* frame, AVPacket* packet, ff_render_info* render, enum AVMediaType type)
{
    int ret = -1;

    ret = avcodec_send_packet(codec_ctx, packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
            return;
        }

        if (type == AVMEDIA_TYPE_VIDEO) {
            log_info("receive video frame %d %d", codec_ctx->frame_number, frame->linesize[0]);

#ifdef DUMP_VIDEO
            switch (frame->format) {
                case AV_PIX_FMT_YUV420P:
                case AV_PIX_FMT_YUVJ420P:
                    yuv420p_save(render->fp, frame, codec_ctx);
                    break;
                case AV_PIX_FMT_RGB24:
                    rgb24_save(render->fp, frame, codec_ctx);
                    break;
                case AV_PIX_FMT_YUV422P:
                case AV_PIX_FMT_YUVJ422P:
                    yuv422p_save(render->fp, frame, codec_ctx);
                    break;
                case AV_PIX_FMT_YUV444P:
                case AV_PIX_FMT_YUVJ444P:
                    yuv444p_save(render->fp, frame, codec_ctx);
                    break;
                default:
                    log_err("unsupport YUV format %d\n", frame->format);
                    break;
            }
#else
            render_video(render->renderer, render->texture, &render->rect, frame);
#endif
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            log_info("receive audio frame %d", codec_ctx->frame_number);

            int data_size = av_get_bytes_per_sample(codec_ctx->sample_fmt);
            if (data_size < 0) {
                log_err("av_get_bytes_per_sample failed\n");
                return;
            }

#ifdef DUMP_AUDIO
            for (int i = 0; i < frame->nb_samples; i++) {
                for (int ch = 0; ch < codec_ctx->ch_layout.nb_channels; ch++) {
                    fwrite(frame->data[ch] + data_size * i, 1, data_size, render->fp);
                }
            }
#endif
        }
    }
}

int main(int argc, char* argv[])
{
    int ret = -1;
    int video_index = -1;
    int audio_index = -1;
    AVFrame* frame = NULL;
    AVPacket* packet = NULL;
    char* stream_file = NULL;
    ff_render_info render = {0};
    AVFormatContext* fmt_ctx = NULL;
    AVCodecContext* video_ctx = NULL;
    AVCodecContext* audio_ctx = NULL;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input file>\n", argv[0]);
        return 0;
    }

    stream_file = argv[1];

#ifdef DUMP_VIDEO
    char video_file[80] = {'\0'};
    sprintf(video_file, "%s.%s", OUT_FILE, "yuv");
    render.fp_vout = fopen(video_file, "wb+");
    if (!render.fp_vout) {
        log_err("fopen %s failed\n", video_file);
        return -1;
    }
#endif

#ifdef DUMP_AUDIO
    char audio_file[80] = {'\0'};
    sprintf(audio_file, "%s.%s", OUT_FILE, "pcm");
    render.fp_aout = fopen(audio_file, "wb+");
    if (!render.fp_aout) {
        log_err("fopen %s failed\n", audio_file);
        return -1;
    }
#endif

    ret = avformat_open_input(&fmt_ctx, stream_file, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input %s failed, error(%s)", stream_file, av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_find_stream_info %s failed, error(%s)", stream_file, av_err2str(ret));
        return -1;
    }

    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* stream = fmt_ctx->streams[i];
        AVCodecParameters* codecpar = stream->codecpar;

        if (codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }

        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            log_err("avcodec_find_decoder %s failed\n", avcodec_get_name(codecpar->codec_id));
            return -1;
        }

        AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            log_err("avcodec_alloc_context3 failed\n");
            return -1;
        }

        ret = avcodec_parameters_to_context(codec_ctx, codecpar);
        if (ret < 0) {
            log_err("avcodec_parameters_to_context failed");
            return -1;
        }

        ret = avcodec_open2(codec_ctx, codec, NULL);
        if (ret < 0) {
            log_err("avcodec_open2 failed, error(%s)", av_err2str(ret));
            return -1;
        }

        if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_index = i;
            audio_ctx = codec_ctx;
            log_info("video codec %s", avcodec_get_name(codecpar->codec_id));
        } else if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_index = i;
            video_ctx = codec_ctx;
            log_info("audio codec %s", avcodec_get_name(codecpar->codec_id));
        }
    }

    if ((video_index == -1) && (audio_index == -1)) {
        log_err("video or audio stream index invalid\n");
        return -1;
    }

    packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    frame = av_frame_alloc();
    if (!frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    av_dump_format(fmt_ctx, 0, stream_file, 0);

    SDL_Event event;
    render.rect.x = 0;
    render.rect.y = 0;
    render.rect.w = video_ctx->width;
    render.rect.h = video_ctx->height;
    int screen_w = video_ctx->width;
    int screen_h = video_ctx->height;
    render.screen = SDL_CreateWindow("ffmpeg player Window", SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED, screen_w, screen_h, SDL_WINDOW_OPENGL);
    if (!render.screen) {
        printf("SDL: could not create window - exiting:%s\n",SDL_GetError());
        return -1;
    }

    SDL_Thread *refresh_thread = SDL_CreateThread(refresh_video, NULL, NULL);

    render.renderer = SDL_CreateRenderer(render.screen, -1, 0);
    render.texture  = SDL_CreateTexture(render.renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                                        video_ctx->width, video_ctx->height);

    sws_frame = av_frame_alloc();
    if (!sws_frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        return -1;
    }

    sws_frame->format = video_ctx->pix_fmt;
    sws_frame->width  = video_ctx->width;
    sws_frame->height = video_ctx->height;

    ret = av_frame_get_buffer(sws_frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate the video frame data\n");
        return -1;
    }

    img_convert_ctx = sws_getContext(video_ctx->width, video_ctx->height, video_ctx->pix_fmt,
    video_ctx->width, video_ctx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

    for (;;) {
        SDL_WaitEvent(&event);

        if (event.type == REFRESH_EVENT) {
            ret = av_read_frame(fmt_ctx, packet);
            if (ret >= 0) {
                if (packet->stream_index == video_index) {
                    decode_process(video_ctx, frame, packet, &render, AVMEDIA_TYPE_VIDEO);
                } else if (packet->stream_index == audio_index) {
                    decode_process(audio_ctx, frame, packet, &render, AVMEDIA_TYPE_AUDIO);
                }

                av_packet_unref(packet);
            } else {
                if (video_ctx) decode_process(video_ctx, frame, NULL, &render, AVMEDIA_TYPE_VIDEO);
                if (audio_ctx) decode_process(audio_ctx, frame, NULL, &render, AVMEDIA_TYPE_AUDIO);
            }
        } else if (event.type == SDL_WINDOWEVENT) {
            SDL_GetWindowSize(render.screen, &screen_w, &screen_h);
        } else if (event.type == SDL_QUIT) {
            thread_exit = 1;
        } else if (event.type == BREAK_EVENT) {
            break;
        }
    }

    log_info("demuxing completed");

#ifdef DUMP_VIDEO
    if (render.fp_vout) fclose(render.fp_vout);
#endif

#ifdef DUMP_AUDIO
    if (render.fp_aout) fclose(render.fp_aout);
#endif

    if (video_ctx) avcodec_free_context(&video_ctx);
    if (audio_ctx) avcodec_free_context(&audio_ctx);

    SDL_Quit();
    av_frame_free(&frame);
    av_packet_free(&packet);

    return 0;
}
