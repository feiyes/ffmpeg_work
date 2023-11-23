#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#ifdef __cplusplus
};
#endif

#define MAX_AUDIO_FRAME_SIZE 192000

static  int     audio_len;
static  Uint8*  audio_chunk;

void fill_audio(void *udata,Uint8 *stream,int len)
{
    SDL_memset(stream, 0, len);
    if (audio_len==0)
        return;

    len=(len>audio_len?audio_len:len);    /*  Mix  as  much  data  as  possible  */

    SDL_MixAudio(stream,audio_chunk,len,SDL_MIX_MAXVOLUME);
    audio_chunk += len;
    audio_len -= len;
}

int main(int argc, char* argv[])
{
    int ret;
    unsigned int                i;
    int audioStream;
    AVFormatContext    *pFormatCtx;
    AVCodecContext    *pCodecCtx;
    const AVCodec            *pCodec;
    AVPacket        *packet;
    uint8_t            *out_buffer;
    AVFrame            *pFrame;
    SDL_AudioSpec wanted_spec;
    struct SwrContext *au_convert_ctx;

    char url[]="../../../stream/audio/xiaoqingge.mp3";

    pFormatCtx = avformat_alloc_context();
    if (avformat_open_input(&pFormatCtx,url,NULL,NULL)!=0) {
        printf("Couldn't open input stream.\n");
        return -1;
    }

    if (avformat_find_stream_info(pFormatCtx,NULL) < 0) {
        printf("Couldn't find stream information.\n");
        return -1;
    }

    av_dump_format(pFormatCtx, 0, url, false);

    audioStream=-1;
    for(i=0; i < pFormatCtx->nb_streams; i++)
        if (pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO) {
            audioStream=i;
            break;
        }

    if (audioStream==-1) {
        printf("Didn't find a audio stream.\n");
        return -1;
    }

    pCodec=avcodec_find_decoder(pFormatCtx->streams[i]->codecpar->codec_id);
    if (pCodec==NULL) {
        printf("Codec not found.\n");
        return -1;
    }

    pCodecCtx = avcodec_alloc_context3(pCodec);
    ret = avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[i]->codecpar);

    if (avcodec_open2(pCodecCtx, pCodec,NULL) < 0) {
        printf("Could not open codec.\n");
        return -1;
    }

    packet = av_packet_alloc();

    int out_nb_samples=pCodecCtx->frame_size;
    enum AVSampleFormat out_sample_fmt=AV_SAMPLE_FMT_S16;
    int out_sample_rate=44100;
    int out_buffer_size=av_samples_get_buffer_size(NULL,pCodecCtx->ch_layout.nb_channels,out_nb_samples,out_sample_fmt, 1);

    out_buffer=(uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE*2);
    pFrame=av_frame_alloc();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        printf( "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }

    wanted_spec.freq = out_sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = pCodecCtx->ch_layout.nb_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = out_nb_samples;
    wanted_spec.callback = fill_audio;
    wanted_spec.userdata = pCodecCtx;

    if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
        printf("can't open audio.\n");
        return -1;
    }

    au_convert_ctx = swr_alloc();
    av_opt_set_chlayout  (au_convert_ctx, "in_chlayout",       &pCodecCtx->ch_layout,      0);
    av_opt_set_int       (au_convert_ctx, "in_sample_rate",     pCodecCtx->sample_rate,    0);
    av_opt_set_sample_fmt(au_convert_ctx, "in_sample_fmt",      pCodecCtx->sample_fmt, 0);
    av_opt_set_chlayout  (au_convert_ctx, "out_chlayout",      &pCodecCtx->ch_layout,      0);
    av_opt_set_int       (au_convert_ctx, "out_sample_rate",    pCodecCtx->sample_rate,    0);
    av_opt_set_sample_fmt(au_convert_ctx, "out_sample_fmt",     out_sample_fmt,     0);
    swr_init(au_convert_ctx);

    SDL_PauseAudio(0);

    while (av_read_frame(pFormatCtx, packet) >= 0) {
        if (packet->stream_index==audioStream) {
            ret = avcodec_send_packet(pCodecCtx, packet);
            if (ret < 0) {
                printf("Error in decoding audio frame.\n");
                return -1;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(pCodecCtx, pFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    continue;
                else if (ret < 0) {
                    continue;
                }

                swr_convert(au_convert_ctx,&out_buffer, MAX_AUDIO_FRAME_SIZE,(const uint8_t **)pFrame->data , pFrame->nb_samples);

                while (audio_len > 0)
                    SDL_Delay(1);

                audio_chunk = (Uint8 *) out_buffer;
                audio_len =out_buffer_size;
            }
        }
    }

    swr_free(&au_convert_ctx);

    SDL_CloseAudio();
    SDL_Quit();

    av_packet_free(&packet);
    av_free(out_buffer);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);

    return 0;
}
