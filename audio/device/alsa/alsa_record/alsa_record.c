#include <sys/time.h>
#include <alsa/asoundlib.h>
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include "log.h"

static int encode_process(AVCodecContext *ctx, AVFrame *frame, AVPacket *packet)
{
    int ret;

    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        } else if (ret < 0) {
            log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
            return -1;
        }

        av_packet_unref(packet);
    }

    return 0;
}

int main()
{
    int size;
    char *buffer;
    snd_pcm_t *handle;
    AVFrame *pframePCM;
	pframePCM = av_frame_alloc();
    
    snd_pcm_uframes_t frames;
	pframePCM->format = AV_SAMPLE_FMT_S16;             //S16格式
    pframePCM->sample_rate = 48000;                    //采样率
    pframePCM->nb_samples = frames;                    //采样点/每帧
    av_channel_layout_copy(&pframePCM->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
    av_frame_get_buffer(pframePCM, 0);
    
    AVFrame *pframeAAC;
	pframeAAC = av_frame_alloc();
    
	pframeAAC->format = AV_SAMPLE_FMT_FLTP;            //fltp格式
    pframeAAC->sample_rate = 44100;                    //采样率
    pframeAAC->nb_samples = 1024;                      //采样点/每帧
    av_channel_layout_copy(&pframeAAC->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
    av_frame_get_buffer(pframeAAC, 0);

    struct SwrContext *aac_convert_ctx  = swr_alloc();
    swr_alloc_set_opts(aac_convert_ctx, 
	                   AV_CH_LAYOUT_STEREO,     //dst目标
	                   AV_SAMPLE_FMT_FLTP,
	                   44100, 
	                   AV_CH_LAYOUT_STEREO,     //src原始
	                   AV_SAMPLE_FMT_S16, 
	                   48000, 
	                   0, 
	                   NULL);

    const AVCodec* encodec = avcodec_find_encoder(AV_CODEC_ID_AAC);

    AVCodecContext* encodec_ctx = NULL;
	encodec_ctx = avcodec_alloc_context3(encodec);

    encodec_ctx->codec_id = encodec->id;
    encodec_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
    encodec_ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    encodec_ctx->bit_rate    = 64000;
    encodec_ctx->sample_rate = 44100;
    av_channel_layout_copy(&encodec_ctx->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
    
    avcodec_open2(encodec_ctx, encodec, NULL);
    
    char *out_name="rtmp://127.0.0.1/live/stream";
    AVFormatContext* outfmt_ctx = NULL;
    avformat_alloc_output_context2(&outfmt_ctx, NULL, "flv",out_name);
    
    AVStream *out_stream = NULL;
    out_stream = avformat_new_stream(outfmt_ctx,NULL);
    
	avcodec_parameters_from_context(out_stream->codecpar, encodec_ctx);

	av_dump_format(outfmt_ctx, 0, out_name, 1);

    //打开rtmp的网络输出IO
    avio_open(&outfmt_ctx->pb, out_name, AVIO_FLAG_WRITE);

    avformat_write_header(outfmt_ctx, NULL);
    
    int vpts=0;
    
    AVPacket *enc_pkt = av_packet_alloc();

    while(1)
    {
         snd_pcm_readi(handle, buffer, frames);
         memcpy(pframePCM->data[0],buffer,size);
         swr_convert(aac_convert_ctx,
                     pframeAAC->data,                       //dst     
                     pframeAAC->nb_samples,
                     (const uint8_t **)pframePCM->data,     //src
                     pframePCM->nb_samples);
                     
         encode_process(encodec_ctx, pframeAAC, enc_pkt);
		 enc_pkt->pts = av_rescale_q(enc_pkt->pts, encodec_ctx->time_base, out_stream->time_base);
         enc_pkt->dts = av_rescale_q(enc_pkt->dts, encodec_ctx->time_base, out_stream->time_base);
         enc_pkt->duration = av_rescale_q(enc_pkt->duration, encodec_ctx->time_base, out_stream->time_base);
         av_interleaved_write_frame(outfmt_ctx, enc_pkt);

         av_packet_free(&enc_pkt);
         
         pframeAAC->pts = vpts;
		 vpts+=pframeAAC->nb_samples;
    }
}

