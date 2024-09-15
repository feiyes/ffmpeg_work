#include <fcntl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <libavutil/time.h>
#include <linux/videodev2.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include "log.h"

#define ENC_FPS       30
#define ENC_GOP       30
#define ENC_WIDTH     1280
#define ENC_HEIGHT    720
#define MAX_CHANNEL       (4)
#define REQ_BUF_COUNT     (32)
#define AV_IO_BUF_SIZE    (1280*720*3*10)
#define VERSION_MAJOR(x)  ((x >> 16) & 0xff)
#define VERSION_MINOR(x)  ((x >> 8)  & 0xff)
#define VERSION_MICRO(x)  ((x)       & 0xff)

typedef struct InputContext {
    FILE* fp;
    int a_idx;
    int v_idx;
    AVFrame*  frame;
    AVPacket* packet;
    AVCodecContext*  a_ctx;
    AVCodecContext*  v_ctx;
    AVFormatContext* fmt_ctx;
} InputContext;

typedef struct OutputContext {
    AVStream* stream;
    AVPacket* packet;
    AVCodecContext*  enc_ctx;
    AVFormatContext* fmt_ctx;
} OutputContext;

struct buffer {
    void   *start;
    size_t length;
};

struct usb_camera {
    int fd;
    nfds_t fds;
    char   dev_name[32];
    struct pollfd poll_fd[MAX_CHANNEL];
    struct buffer buffers[REQ_BUF_COUNT];
};

static int v4l2_ioctl(int fd, int request, void *arg)
{
    int r = -1;

    do {
        r = ioctl(fd, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static void capture_caps_list(const char* dev_name)
{
    int fd = -1;
    int ret = -1;
    struct v4l2_format fmt;
    struct v4l2_fmtdesc vfd;
    struct v4l2_capability cap;

    fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
    if (-1 == fd) {
        log_err("%s open failed, error = %d(%s)\n", dev_name, errno, strerror(errno));
        return;
    }

    ret = v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap);
    if (ret < 0) {
        log_err("%s is not v4l2 device\n", dev_name);
        return;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        log_err("%s is not video capture device\n", dev_name);
        return;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        log_err("%s does not support streaming I/O\n", dev_name);
        return;
    }

    printf("Driver Info:\n");
    printf("\tDriver name       : %s\n",  cap.driver);
    printf("\tCard type         : %s\n",  cap.card);
    printf("\tBus info          : %s\n",  cap.bus_info);
    printf("\tDriver version    : %d.%d.%d\n", VERSION_MAJOR(cap.version), VERSION_MINOR(cap.version), VERSION_MICRO(cap.version));
    printf("\tCapabilities      : %#x\n", cap.capabilities);
    printf("\tDevice Caps       : %#x\n", cap.device_caps);

    printf("\nFormat Video Capture:\n");
    vfd.index = 0;
    vfd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (!v4l2_ioctl(fd, VIDIOC_ENUM_FMT, &vfd)) {
        struct v4l2_frmsizeenum vfse = { .pixel_format = vfd.pixelformat };
        printf("\tPixel Format      : '%s' (%11s)", av_fourcc2str(vfd.pixelformat), vfd.description);

        while (!v4l2_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &vfse)) {
            switch (vfse.type) {
            case V4L2_FRMSIZE_TYPE_DISCRETE:
                printf(" %ux%u", vfse.discrete.width, vfse.discrete.height);
            break;
            case V4L2_FRMSIZE_TYPE_CONTINUOUS:
            case V4L2_FRMSIZE_TYPE_STEPWISE:
                printf(" {%u-%u, %u}x{%u-%u, %u}",
                       vfse.stepwise.min_width,
                       vfse.stepwise.max_width,
                       vfse.stepwise.step_width,
                       vfse.stepwise.min_height,
                       vfse.stepwise.max_height,
                       vfse.stepwise.step_height);
            }

            vfse.index++;
        }

        printf("\n");
        vfd.index++;
    }

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ret = v4l2_ioctl(fd, VIDIOC_G_FMT, &fmt);
    if (ret < 0) {
        log_err("%s get video format failed\n", dev_name);
        return;
    }

    printf("\tWidth/Height      : %d/%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
    printf("\tField             : %d\n",    fmt.fmt.pix.field);
    printf("\tBytes per Line    : %d\n",    fmt.fmt.pix.bytesperline);
    printf("\tSize Image        : %d\n",    fmt.fmt.pix.sizeimage);
    printf("\tColorspace        : %d\n",    fmt.fmt.pix.colorspace);
    printf("\tTransfer Function : %d\n",    fmt.fmt.pix.xfer_func);
    printf("\tYCbCr/HSV Encoding: %d\n",    fmt.fmt.pix.ycbcr_enc);
    printf("\tQuantization      : %d\n",    fmt.fmt.pix.quantization);
}

static int capture_init(int argc, char** argv, struct usb_camera *cam)
{
    int i = 0;
    int fd = -1;
    int ret = 0;
    char* dev_name = argv[1];
    int width  = atoi(argv[2]);
    int height = atoi(argv[3]);
    int denominator = atoi(argv[4]);
    struct v4l2_format fmt = {0};
    struct v4l2_streamparm parm = {0};
    struct v4l2_requestbuffers req = {0};
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // open v4l2 device
    sprintf(cam->dev_name, "%s", dev_name);
    fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
    if (-1 == fd) {
        log_err("%s open failed, error = %d(%s)\n", dev_name, errno, strerror(errno));
        return -1;
    }

    // set v4l2 format
    fmt.type = type;
    ret = v4l2_ioctl(fd, VIDIOC_G_FMT, &fmt);
    if (ret < 0) {
        log_err("%s get video format failed\n", dev_name);
        return -1;
    }

    fmt.fmt.pix.width  = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    ret = v4l2_ioctl(fd, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
        log_err("%s set video format failed\n", dev_name);
        return -1;
    }

    // set v4l2 parm
    parm.type = type;
    ret = v4l2_ioctl(fd, VIDIOC_G_PARM, &parm);
    if (ret < 0) {
        log_err("%s get video param failed\n", dev_name);
        return -1;
    }

    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = denominator;
    ret = v4l2_ioctl(fd, VIDIOC_S_PARM, &parm);
    if (ret < 0) {
        log_err("%s set video param failed\n", dev_name);
        return -1;
    }

    // request vl42 buffer
    req.type   = type;
    req.count  = REQ_BUF_COUNT;
    req.memory = V4L2_MEMORY_MMAP;
    ret = v4l2_ioctl(fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        log_err("%s request buffer failed", dev_name);
        return -1;
    }

    for (i = 0; i < REQ_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .index  = i,
            .type   = type,
            .memory = V4L2_MEMORY_MMAP,
        };

        ret = v4l2_ioctl(fd, VIDIOC_QUERYBUF, &buf);
        if (ret < 0) {
            break;
        }

        cam->buffers[i].length = buf.length;
        cam->buffers[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, buf.m.offset);

        if (MAP_FAILED == cam->buffers[i].start) {
            goto failed;
        }
    }

    cam->fd = fd;
    cam->fds = 1;
    cam->poll_fd[0].fd     = fd;
    cam->poll_fd[0].events = POLLIN;

    return 0;

failed:
    for (int i = 0; i < REQ_BUF_COUNT; i++) {
        if (MAP_FAILED != cam->buffers[i].start)
            munmap(cam->buffers[i].start, cam->buffers[i].length);
    }

    return 0;
}

static int capture_start(struct usb_camera *cam)
{
    int ret = -1;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (int i = 0; i < REQ_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .index  = i,
            .type   = type,
            .memory = V4L2_MEMORY_MMAP,
        };

        ret = v4l2_ioctl(cam->fd, VIDIOC_QBUF, &buf);
        if (ret < 0) {
            log_err("%s VIDIOC_QBUF(fd = %d) faild\n", cam->dev_name, cam->fd);
            return -1;
        }
    }

    ret = v4l2_ioctl(cam->fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        log_err("%s VIDIOC_STREAMON(fd = %d) faild\n", cam->dev_name, cam->fd);
        return -1;
    }

    return 0;
}

static int capture_stop(struct usb_camera *cam)
{
    int ret = -1;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = v4l2_ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    if (ret < 0) {
        log_err("%s VIDIOC_STREAMOFF(fd = %d) faild\n", cam->dev_name, cam->fd);
        return -1;
    }

    return 0;
}

static int read_frame(struct usb_camera *cam, unsigned char *buf, unsigned int ch, struct timeval *tvl)
{
    int len = 0;
    int ret = -1;
    struct v4l2_buffer dbuf = {0};

    dbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dbuf.memory = V4L2_MEMORY_MMAP;
    ret = v4l2_ioctl(cam->fd, VIDIOC_DQBUF, &dbuf);
    if (-1 == ret) {
        switch (errno) {
        case EAGAIN:
            return 0;
        case EIO:
        default:
            log_err("VIDIOC_DQBUF faild\n");
            return -1;
        }
    }

    if (dbuf.index > REQ_BUF_COUNT) {
        log_err("%s VIDIOC_DQBUF buffer not enough(%d < %d)",
                cam->dev_name, dbuf.index, REQ_BUF_COUNT);
        return -1;
    }

    len = dbuf.bytesused;
    memcpy(buf, cam->buffers[dbuf.index].start, len);

    tvl->tv_sec  = dbuf.timestamp.tv_sec;
    tvl->tv_usec = dbuf.timestamp.tv_usec;
    ret = v4l2_ioctl(cam->fd, VIDIOC_QBUF, &dbuf);
    if (-1 == ret) {
        log_err("%s VIDIOC_QBUF faild\n", cam->dev_name);
    }

    return len;
}

void capture_deinit(struct usb_camera* cam)
{
    for (int i = 0; i < REQ_BUF_COUNT; i++) {
        munmap(cam->buffers[i].start, cam->buffers[i].length);
    }
}

int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    int ret = -1;
    struct timeval tvl;
    unsigned int frame_len = 0;
    struct usb_camera* cam = (struct usb_camera*)opaque;

    ret = poll(cam->poll_fd, cam->fds, -1);
    if (ret < 0) {
        log_err("usb_cam poll failed");
        return AVERROR_EXTERNAL;
    }

    if ((cam->poll_fd[0].revents & POLLERR) == POLLERR) {
        log_err("usb_cam revents %#x", cam->poll_fd[0].revents);
        return AVERROR_EXTERNAL;
    }

    if (cam->poll_fd[0].revents && POLLIN) {
        frame_len = read_frame(cam, buf, 0, &tvl);
    }

    cam->poll_fd[0].revents = 0;

    if (frame_len > buf_size) {
        log_err("frame_len(%d) is bigger then buf_size(%d)\n", frame_len, buf_size);
        return buf_size;
    }

    return (int)frame_len;
}

int open_input(struct usb_camera* cam, AVIOContext **avio_ctx, InputContext* ic)
{
    int ret = -1;
    size_t avio_ctx_buffer_size = AV_IO_BUF_SIZE;

    ic->fmt_ctx = avformat_alloc_context();
    if (!ic->fmt_ctx) {
        log_err("avformat_alloc_context failed!\n");
        return -1;
    }

    uint8_t* avio_ctx_buffer = (uint8_t*)av_malloc(avio_ctx_buffer_size);
    if (!avio_ctx_buffer) {
        log_err("av_malloc failed!\n");

        goto failed;
    }

    *avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size,
                                   0, cam, read_packet, NULL, NULL);
    if (!*avio_ctx) {
        log_err("avio_alloc_context failed");

        goto failed;
    }

    ic->fmt_ctx->pb    = *avio_ctx;
    ic->fmt_ctx->flags = AVFMT_FLAG_CUSTOM_IO;
    ret = avformat_open_input(&ic->fmt_ctx, NULL, NULL, NULL);
    if (ret < 0) {
        log_err("avformat_open_input failed, error(%s)", av_err2str(ret));

        goto failed;
    }

    ret = avformat_find_stream_info(ic->fmt_ctx, NULL);
    if (ret != 0) {
        log_err("avformat_find_stream_info failed, error(%s)", av_err2str(ret));

        goto failed;
    }

    av_dump_format(ic->fmt_ctx, 0, NULL, 0);

    for (int i = 0; i < ic->fmt_ctx->nb_streams; i++) {
        AVStream* stream = ic->fmt_ctx->streams[i];
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

        codec_ctx->time_base = stream->time_base;

        if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ic->a_idx = i;
            ic->a_ctx = codec_ctx;
            log_info("audio codec %s", avcodec_get_name(codecpar->codec_id));
        } else if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ic->v_idx = i;
            ic->v_ctx = codec_ctx;
            log_info("video codec %s, delay %d", avcodec_get_name(codecpar->codec_id), codecpar->video_delay);
        }
    }

    ic->packet = av_packet_alloc();
    if (!ic->packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    ic->frame = av_frame_alloc();
    if (!ic->frame) {
        log_err("av_frame_alloc failed\n");
        return -1;
    }

    return 0;

failed:

    return -1;
}

int open_output(InputContext* ic, int* video_index, OutputContext* oc)
{
    int ret = -1;
    char* venc_name = "libx264";
    const char *out_format = "flv";
    const char *url = "rtmp://192.168.174.128:1936/live/test";

    oc->packet = av_packet_alloc();
    if (!oc->packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    oc->fmt_ctx = avformat_alloc_context();
    if (!oc->fmt_ctx) {
        log_err("avformat_alloc_context failed");
        return -1;
    }

    ret = avformat_alloc_output_context2(&oc->fmt_ctx, NULL, out_format, url);
    if (ret < 0) {
        log_err("avformat_alloc_output_context2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    oc->stream = avformat_new_stream(oc->fmt_ctx, NULL);
    if (!oc->stream) {
        log_err("avformat_new_stream failed");
        return -1;
    }

    const AVCodec *enc_codec = avcodec_find_encoder_by_name(venc_name);
    if (!enc_codec) {
        log_err("avcodec_find_encoder failed");
        return -1;
    }

    oc->enc_ctx = avcodec_alloc_context3(enc_codec);
    if (!oc->enc_ctx) {
        log_err("avcodec_alloc_context3 failed");
        return -1;
    }

    oc->enc_ctx->bit_rate   = 110000;
    oc->enc_ctx->gop_size   = ENC_GOP;
    oc->enc_ctx->width      = ENC_WIDTH;
    oc->enc_ctx->height     = ENC_HEIGHT;
    oc->enc_ctx->codec_id   = enc_codec->id;
    oc->enc_ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
    oc->enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    oc->enc_ctx->time_base  = (AVRational){ 1, ENC_FPS };

    if (oc->enc_ctx->codec_id == AV_CODEC_ID_H264 ||
        oc->enc_ctx->codec_id == AV_CODEC_ID_H265) {
        oc->enc_ctx->qmin         = 10;
        oc->enc_ctx->qmax         = 51;
        oc->enc_ctx->qcompress    = 0.6;
        oc->enc_ctx->max_b_frames = 2;
    } else if (oc->enc_ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        oc->enc_ctx->max_b_frames = 2;
    } else if (oc->enc_ctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        oc->enc_ctx->mb_decision  = 2;
    }

    if (oc->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        oc->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(oc->enc_ctx, enc_codec, NULL);
    if (ret < 0) {
        log_err("avcodec_open2 failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    ret = avcodec_parameters_from_context(oc->stream->codecpar, oc->enc_ctx);
    if (ret < 0) {
        log_err("avcodec_parameters_from_context failed, error(%s)", av_err2str(ret));
        return -1;
    }

    oc->stream->time_base    = oc->enc_ctx->time_base;
    oc->stream->r_frame_rate = av_inv_q(oc->enc_ctx->time_base);

    log_info("rtmp stream: %s", url);
    av_dump_format(oc->fmt_ctx, 0, url, 1);

    if (!(oc->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&oc->fmt_ctx->pb, url, AVIO_FLAG_READ_WRITE);
        if (ret < 0) {
            log_err("avio_open failed, error=%d(%s)", ret, av_err2str(ret));
            return -1;
        }
    }

    ret = avformat_write_header(oc->fmt_ctx, NULL);
    if (ret < 0) {
        log_err("avformat_write_header failed, error=%d(%s)", ret, av_err2str(ret));
        return -1;
    }

    return 0;
}

void encode_process(AVFormatContext *fmt_ctx, AVCodecContext *enc_ctx,
                    AVStream *stream, AVFrame* sws_frame, AVPacket* packet)
{
    int ret = -1;

    ret = avcodec_send_frame(enc_ctx, sws_frame);
    if (ret < 0) {
        //log_err("avcodec_send_frame failed, error(%s)\n", av_err2str(ret));
        return;
    }

    ret = avcodec_receive_packet(enc_ctx, packet);
    if (ret < 0) {
        //log_err("avcodec_receive_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    av_packet_rescale_ts(packet, enc_ctx->time_base, stream->time_base);

    //log_info("receive video packet, pts(%ss %s) dts(%ss %s)",
    //          av_ts2timestr(packet->pts, &stream->time_base), av_ts2str(packet->pts),
    //          av_ts2timestr(packet->dts, &stream->time_base), av_ts2str(packet->dts));

    packet->pos = -1;
    packet->stream_index = stream->index;
    av_interleaved_write_frame(fmt_ctx, packet);

    av_packet_unref(packet);
}

void decode_process(InputContext* i, OutputContext* o)
{
    int ret = -1;

    ret = avcodec_send_packet(i->v_ctx, i->packet);
    if (ret < 0) {
        log_err("avcodec_send_packet failed, error(%s)\n", av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(i->v_ctx, i->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            log_err("avcodec_receive_frame failed, error(%s)\n", av_err2str(ret));
            return;
        }

        encode_process(o->fmt_ctx, o->enc_ctx, o->stream, i->frame, o->packet);
    }
}

int remux(InputContext* ic, OutputContext* oc, int video_index)
{
    while (av_read_frame(ic->fmt_ctx, ic->packet) >= 0) {
        if (ic->packet->stream_index == ic->v_idx) {
            ic->packet->pts = ic->v_ctx->frame_number;
            decode_process(ic, oc);
        } else if (ic->packet->stream_index == ic->a_idx) {
            ic->packet->pts = ic->a_ctx->frame_number;
            decode_process(ic, oc);
        }

        av_packet_unref(ic->packet);
    }

    if (ic->v_ctx) {
        ic->packet = NULL;
        decode_process(ic, oc);
    } else if (ic->a_ctx) {
        ic->packet = NULL;
        decode_process(ic, oc);
    }

    return 0;
}

int remux_debug(InputContext* ic, OutputContext* oc, int video_index)
{
    int ret = -1;
    long long frame_index = 0;
    AVStream *in_stream  = NULL;
    AVStream *out_stream = NULL;
    long long start_time = av_gettime();

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    while (1) {
        ret = av_read_frame(ic->fmt_ctx, packet);
        if (ret < 0) break;

        if (packet->pts == AV_NOPTS_VALUE) {
            AVRational time_base = ic->fmt_ctx->streams[video_index]->time_base;
            int64_t calc_duration = (double)AV_TIME_BASE / av_q2d(ic->fmt_ctx->streams[video_index]->r_frame_rate);

            packet->pts = (double)(frame_index*calc_duration) / (double)(av_q2d(time_base)*AV_TIME_BASE);
            packet->dts = packet->pts;
            packet->duration = (double)calc_duration / (double)(av_q2d(time_base)*AV_TIME_BASE);
        }

        if (packet->stream_index == video_index) {
            AVRational time_base = ic->fmt_ctx->streams[video_index]->time_base;
            AVRational time_base_q = { 1, AV_TIME_BASE };
            int64_t pts_time = av_rescale_q(packet->dts, time_base, time_base_q);
            int64_t now_time = av_gettime() - start_time;

            AVRational avr = ic->fmt_ctx->streams[video_index]->time_base;
            printf("avr.num:%d, avr.den:%d, packet.dts:%ld, packet.pts:%ld, pts_time:%ld\n",
                    avr.num,    avr.den,    packet->dts,    packet->pts,    pts_time);

            if (pts_time > now_time) {
                printf("pts_time:%ld, now_time:%ld\n", pts_time, now_time);
                av_usleep((unsigned int)(pts_time - now_time));
            }
        }

        in_stream  = ic->fmt_ctx->streams[packet->stream_index];
        out_stream = oc->fmt_ctx->streams[packet->stream_index];

        packet->pos = -1;
        packet->pts = av_rescale_q_rnd(packet->pts, in_stream->time_base, out_stream->time_base, (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        packet->dts = av_rescale_q_rnd(packet->dts, in_stream->time_base, out_stream->time_base, (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        packet->duration = (int)av_rescale_q(packet->duration, in_stream->time_base, out_stream->time_base);
        if (packet->stream_index == video_index) {
            log_info("send %lld video frames", frame_index);
            frame_index++;
        }

        ret = av_interleaved_write_frame(oc->fmt_ctx, packet);
        if (ret < 0) {
            log_err("av_interleaved_write_frame failed, error(%s)\n", av_err2str(ret));
            break;
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    return 0;
}

// TODO decode MJPEG + encode H26x
// ffmpeg -f v4l2 -list_formats all -i /dev/video0
// ./ffmpeg_usb_rtmp /dev/video0 1280 720 30 1500000
int main(int argc, char* argv[])
{
    int ret = -1;
    int video_index = -1;
    InputContext  ic = {0};
    OutputContext oc = {0};
    AVIOContext *avio_ctx = NULL;
    struct usb_camera usb_cam = {0};

    if (argc < 6) {
        return -1;
    }

    avformat_network_init();

    capture_caps_list(argv[1]);

    log_info("capture init");
    ret = capture_init(argc, argv, &usb_cam);
    if (ret < 0) {
        return -1;
    }

    log_info("capture start");
    ret = capture_start(&usb_cam);
    if (ret < 0) {
        return -1;
    }

    ret = open_input(&usb_cam, &avio_ctx, &ic);
    if (ret < 0) {
        goto end;
    }

    ret = open_output(&ic, &video_index, &oc);
    if (ret < 0) {
        goto end;
    }

    ret = remux(&ic, &oc, video_index);
    if (ret < 0) {
        goto end;
    }

    log_info("capture_stop");
    ret = capture_stop(&usb_cam);
    if (ret < 0) {
        goto end;
    }

end:
    if (oc.fmt_ctx) avformat_free_context(oc.fmt_ctx);
    if (avio_ctx) avio_context_free(&avio_ctx);
    if (ic.fmt_ctx) avformat_free_context(ic.fmt_ctx);

    log_info("capture_deinit");
    capture_deinit(&usb_cam);

    return ret;
}

