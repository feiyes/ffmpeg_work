#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include "log.h"

#define MAX_CHANNEL     (4)
#define AV_IO_BUF_SIZE  (96*1024)
#define DEV_TYPE        "video4linux2"
#define DEV_NAME        "/dev/video0"

struct buffer {
    void   *start;
    size_t length;
};

struct usbcamera_node {
    int fd;
    char id[32];
    int channel;
    int usb_port;
    int n_buffers;
    char devname[32];
    struct buffer *buffers;
    struct v4l2_format fmt;
    struct v4l2_streamparm parm;
    struct v4l2_requestbuffers req;
    int poll_index[MAX_CHANNEL];
};

unsigned int frame_len = 0;
unsigned int frame_cnt = 0;
struct usbcamera_node usbcamra;
nfds_t usbcamra_poll_fd_num = 0;
struct pollfd usbcamra_poll_fd[MAX_CHANNEL];

static int xioctl(int fh, int request, void *arg)
{
    int r = -1;

    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static int capture_init(struct usbcamera_node *camera_node)
{
    int ret = 0;
    struct v4l2_capability cap;
    struct v4l2_fmtdesc fmtdesc;

    camera_node->fd = open(camera_node->devname, O_RDWR | O_NONBLOCK, 0);
    if (-1 == camera_node->fd) {
        log_err("open(%s) failed, error = %d(%s)\n", camera_node->devname, errno, strerror(errno));
        return -1;
    }

    if (-1 == xioctl(camera_node->fd, VIDIOC_QUERYCAP, &cap)) {
        log_err("%s is not v4l2 device\n", camera_node->devname);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        log_err("%s is not video capture device\n", camera_node->devname);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        log_err("%s does not support streaming i/o\n", camera_node->devname);
        return -1;
    }

    printf("\nDriver Info:\n");
    printf("\tDriver name    : %s\n", cap.driver);
    printf("\tCard type      : %s\n", cap.card);
    printf("\tBus info       : %s\n", cap.bus_info);
    //printf("\tthe version is: %d\n", cap.version);
    printf("\tCapabilities   : 0x%x\n", cap.capabilities);
    printf("\tDevice Caps    : 0x%x\n\n", cap.device_caps);

    printf("\nFormat Video Capture:\n");
    fmtdesc.index = 0;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (ioctl(camera_node->fd, VIDIOC_ENUM_FMT, &fmtdesc) != -1) {
        printf("\tWidth/Height : %s\n", cap.driver);
        printf("\tPixel Format : %x (%s)\n", fmtdesc.pixelformat, fmtdesc.description);
        printf("\tFlags : %d\n", fmtdesc.flags);

        printf("VIsuccess! fmtdesc.index:%d, fmtdesc.type:%d, fmtdesc.flags:%d, "
               "fmtdesc.description:%s, fmtdesc.pixelformat:%d\n",
               fmtdesc.index, fmtdesc.type, fmtdesc.flags, fmtdesc.description, fmtdesc.pixelformat);
        fmtdesc.index++;
    }

    if (-1 == xioctl(camera_node->fd, VIDIOC_S_FMT, &camera_node->fmt)) {
        log_err("%s set format failed\n", camera_node->devname);
        return -1;
    }

        printf("\tField : %s\n", cap.bus_info);
        printf("\tBytes per Line : %s\n", cap.driver);
        printf("\tSize Image : %d\n", camera_node->fmt.fmt.pix.sizeimage);
        printf("\tColorspace : %d\n", camera_node->fmt.fmt.pix.colorspace);
        printf("\tTransfer Function : %s\n", cap.driver);
        printf("\tYCbCr/HSV Encoding : %s\n", cap.card);
        printf("\tQuantization : %d\n", camera_node->fmt.fmt.pix.quantization);
    printf("VIDIOC_S_FMT success! width:%d, height:%d, pixelformat:%x, field:%d, bytesperline:%d, "
           "sizeimage:%d, colorspace:%d, priv:%d, flags:%x, ycbcr_enc:%d, quantization:%d, xfer_func:%d\n",
           camera_node->fmt.fmt.pix.width, camera_node->fmt.fmt.pix.height, camera_node->fmt.fmt.pix.pixelformat,
           camera_node->fmt.fmt.pix.field, camera_node->fmt.fmt.pix.bytesperline, camera_node->fmt.fmt.pix.sizeimage,
           camera_node->fmt.fmt.pix.colorspace, camera_node->fmt.fmt.pix.priv, camera_node->fmt.fmt.pix.flags,
           camera_node->fmt.fmt.pix.ycbcr_enc, camera_node->fmt.fmt.pix.quantization, camera_node->fmt.fmt.pix.xfer_func);

    struct v4l2_streamparm parm = {0};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(camera_node->fd, VIDIOC_G_PARM, &parm);
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = camera_node->parm.parm.capture.timeperframe.denominator;
    ret = xioctl(camera_node->fd, VIDIOC_S_PARM, &parm);
    if (ret !=0 ) {
        printf("line:%d parm set error, errno:%d, str:%s\n", __LINE__, errno, strerror(errno));
        return -1;
    }

    printf("fd %d ret %d set Frame rate %.3f fps\n", camera_node->fd, ret,
           1.0 * parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator);

    if (-1 == xioctl(camera_node->fd, VIDIOC_REQBUFS, &camera_node->req)) {
        if (EINVAL == errno) {
            log_err("%s does not support memory mapping\n", "USBCAMERA");
            return -1;
        } else {
            return -1;
        }
    }

    for (camera_node->n_buffers = 0; camera_node->n_buffers < camera_node->req.count; ++camera_node->n_buffers)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0x0, sizeof(struct v4l2_buffer));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = camera_node->n_buffers;

        if (-1 == xioctl(camera_node->fd, VIDIOC_QUERYBUF, &buf)) {
            ret = -1;
            break;
        }

        camera_node->buffers[camera_node->n_buffers].length = buf.length;
        camera_node->buffers[camera_node->n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE ,MAP_SHARED, camera_node->fd, buf.m.offset);
        //printf("mmap buffer index:%d buf %p length %d\n", camera_node->n_buffers, camera_node->buffers[camera_node->n_buffers].start, buf.length);

        if (MAP_FAILED == camera_node->buffers[camera_node->n_buffers].start) {
            ret = -1;
            break;
        }
    }

    if ((ret == -1) && (camera_node->n_buffers != 0)) {
        for (ret = 0; ret < camera_node->n_buffers; ret++) {
            munmap(camera_node->buffers[camera_node->n_buffers].start, camera_node->buffers[camera_node->n_buffers].length);
            printf("munmap buffer index:%d buf %p length %ld\n",
                   camera_node->n_buffers, camera_node->buffers[camera_node->n_buffers].start,
                   camera_node->buffers[camera_node->n_buffers].length);
        }

        return -1;
    }

    return 0;
}

static int capture_start(struct usbcamera_node *camera_node)
{
    unsigned int i;
    int n_buffers = 0;
    enum v4l2_buf_type type;

    n_buffers = camera_node->n_buffers;
    log_info("capture_start fd %d n_buffers %d\n", camera_node->fd, n_buffers);

    for (i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0x0, sizeof(struct v4l2_buffer));

        buf.index  = i;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (-1 == xioctl(camera_node->fd, VIDIOC_QBUF, &buf)) {
            log_err("fd %d VIDIOC_QBUF faild\n", camera_node->fd);
            return -1;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(camera_node->fd, VIDIOC_STREAMON, &type)) {
        log_err("fd %d VIDIOC_STREAMON faild\n", camera_node->fd);
        return -1;
    }

    return 0;
}

static int capture_stop(struct usbcamera_node *camera_node)
{
    enum v4l2_buf_type type;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(camera_node->fd, VIDIOC_STREAMOFF, &type)) {
        printf("fd %d VIDIOC_STREAMOFF faild\n", camera_node->fd);
        return -1;
    }

    printf("fd %d VIDIOC_STREAMOFF Ok!\n", camera_node->fd);

    return 0;
}

static int read_frame(struct usbcamera_node *camera_node, unsigned char *pbuf, unsigned int ch, struct timeval *tvl)
{
    int count = 0;
    int n_buffers = 0;
    struct v4l2_buffer buf;
    memset(&buf, 0x0, sizeof(struct v4l2_buffer));

    n_buffers = camera_node->n_buffers;

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (-1 == xioctl(camera_node->fd, VIDIOC_DQBUF, &buf)) {
        switch (errno) {
        case EAGAIN:
            return 0;
        case EIO:
        default:
            printf("VIDIOC_DQBUF faild\n");
            return -1;
        }
    }

    if (buf.index > n_buffers) {
        printf("buf.indx < n_buffers %d %d\n", buf.index, n_buffers);
        return -1;
    }

    memcpy(pbuf, camera_node->buffers[buf.index].start, buf.bytesused);
    tvl->tv_sec = buf.timestamp.tv_sec;
    tvl->tv_usec = buf.timestamp.tv_usec;
    count = buf.bytesused;

    if (-1 == xioctl(camera_node->fd, VIDIOC_QBUF, &buf)) {
        printf("VIDIOC_QBUF faild\n");
    }

    return count;
}

void free_camra_resource(struct usbcamera_node *camera_node)
{
    int cnt = 0;

    for (cnt = 0; cnt < camera_node->n_buffers; cnt++) {
        munmap(camera_node->buffers[cnt].start, camera_node->buffers[cnt].length);
        printf("munmap buffer index:%d buf %p length %ld\n",
               cnt, camera_node->buffers[cnt].start,
               camera_node->buffers[cnt].length);
    }
}

int read_buffer(void *opaque, uint8_t *pbuf, int buf_size)
{
    struct timeval tvl;

    if (poll(usbcamra_poll_fd, usbcamra_poll_fd_num, -1) == -1) {
        printf("usbcamra poll failed !!!!!!!!!!!!!\n");
        return AVERROR_EXTERNAL;
    }

    if ((usbcamra_poll_fd[0].revents & POLLERR) == POLLERR) {
        printf("usbcamra_poll_fd[0].revents 0x%x\n", usbcamra_poll_fd[0].revents);
        return AVERROR_EXTERNAL;
    }

    if (usbcamra_poll_fd[0].revents && POLLIN) {
        frame_len = read_frame(&usbcamra, pbuf, 0, &tvl);
        //printf("frame_cnt:%d, frame_len:%d, tvl.tv_sec:%ld ", frame_cnt, frame_len, tvl.tv_sec);
        //printf("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
        //       "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x \n",
        //       pbuf[0],pbuf[1],pbuf[2],pbuf[3],pbuf[4],pbuf[5],pbuf[6],pbuf[7],pbuf[8],pbuf[9],pbuf[10],pbuf[11],
        //       pbuf[12],pbuf[13],pbuf[14],pbuf[15],pbuf[16],pbuf[17],pbuf[18],pbuf[19],pbuf[20],pbuf[21],pbuf[22],
        //       pbuf[23],pbuf[24],pbuf[25],pbuf[26],pbuf[27],pbuf[28],pbuf[29],pbuf[30],pbuf[31]);
    }

    frame_cnt++;
    usbcamra_poll_fd[0].revents = 0;

    if (frame_len > buf_size) {
        printf("frame_len is too big then buf_size\n");
        return buf_size;
    }

    return (int)frame_len;
}

//ffmpeg -f v4l2 -list_formats all -i /dev/video0
//程序执行：./ffmpeg_usb_rtmp /dev/video0 1280 720 30 1500000
int main(int argc, char* argv[])
{
    int videoindex = -1;
    unsigned int frame_rate = 0;

    avformat_network_init();

    if (argc != 5) {
        usbcamra.fmt.fmt.pix.width  = 1280;
        usbcamra.fmt.fmt.pix.height = 720;
        frame_rate = 30;
    } else {
        usbcamra.fmt.fmt.pix.width  = atoi(argv[2]);
        usbcamra.fmt.fmt.pix.height = atoi(argv[3]);
        frame_rate = atoi(argv[4]);
    }

    sprintf(usbcamra.devname, "%s", argv[1]);
    printf("width:%d, height:%d, dev:%s", usbcamra.fmt.fmt.pix.width, usbcamra.fmt.fmt.pix.height, usbcamra.devname);

    usbcamra.fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    usbcamra.fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    usbcamra.fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    usbcamra.parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    usbcamra.parm.parm.capture.timeperframe.numerator = 1;
    usbcamra.parm.parm.capture.timeperframe.denominator = frame_rate;

    usbcamra.req.count = 16;
    usbcamra.req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    usbcamra.req.memory = V4L2_MEMORY_MMAP;
    usbcamra.buffers = calloc(usbcamra.req.count, sizeof(struct buffer));
    if (!usbcamra.buffers) {
        log_err("calloc faild, errno:%d, str:%s\n", errno, strerror(errno));
        return -1;
    }

    log_info("capture init");
    capture_init(&usbcamra);

    log_info("capture start");
    capture_start(&usbcamra);

    usbcamra_poll_fd[0].fd = usbcamra.fd;
    usbcamra_poll_fd[0].events = POLLIN;
    usbcamra_poll_fd_num = 1;

    const char *outUrl = "rtmp://192.168.174.128:1935/live";

    AVFormatContext *ifmt_ctx = NULL;

    ifmt_ctx = avformat_alloc_context();
    unsigned char* inbuffer = NULL;
    inbuffer = (unsigned char*)av_malloc(AV_IO_BUF_SIZE);
    if (!inbuffer) {
        avformat_free_context(ifmt_ctx);
        printf("line:%d av_malloc failed!\n", __LINE__);
        return -1;
    }

    AVIOContext *avio_in = avio_alloc_context(inbuffer, AV_IO_BUF_SIZE, 0, NULL, read_buffer, NULL, NULL);
    if (!avio_in) {
        avformat_free_context(ifmt_ctx);
        av_free((void*)inbuffer);
        log_err("avio_alloc_context failed");

        return -1;
    }

    ifmt_ctx->pb = avio_in;
    ifmt_ctx->flags = AVFMT_FLAG_CUSTOM_IO;

    int ret = avformat_open_input(&ifmt_ctx, NULL, NULL, NULL);
    if (ret < 0) {
        avformat_free_context(ifmt_ctx);
        av_free((void*)inbuffer);
        avio_context_free(&avio_in);
        log_err("avformat_open_input failed, error(%s)", av_err2str(ret));

        return -1;
    }

    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    if (ret != 0) {
        avformat_free_context(ifmt_ctx);
        av_free((void*)inbuffer);
        avio_context_free(&avio_in);
        log_err("avformat_open_input failed, error(%s)", av_err2str(ret));

        return -1;
    }

    av_dump_format(ifmt_ctx, 0, NULL, 0);

    AVFormatContext * ofmt_ctx = NULL;
    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", outUrl);
    if (ret < 0) {
        avformat_free_context(ifmt_ctx);
        av_free((void*)inbuffer);
        avio_context_free(&avio_in);
        avformat_free_context(ofmt_ctx);
        log_err("error(%s)", av_err2str(ret));

        return -1;
    }

    for (unsigned int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];
        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoindex = i;
        }

        AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            log_err("avformat_new_stream failed\n");
            ret = AVERROR_UNKNOWN;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            log_err("avcodec_parameters_copy failed, error(%s)\n", av_err2str(ret));
        }

        log_info("codec_id %d", out_stream->codecpar->codec_id);
        out_stream->codecpar->codec_id = AV_CODEC_ID_H264;
        out_stream->codecpar->codec_tag = 0;
    }

    av_dump_format(ofmt_ctx, 0, outUrl, 1);

    log_info("avio_open\n");
    ret = avio_open(&ofmt_ctx->pb, outUrl, AVIO_FLAG_WRITE);
    if (ret < 0) {
        avformat_free_context(ifmt_ctx);
        av_free((void*)inbuffer);
        avio_context_free(&avio_in);
        avformat_free_context(ofmt_ctx);
        log_err("avio_open failed, error(%s)", av_err2str(ret));

        return -1;
    }

    log_info("avformat_write_header\n");
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        avformat_free_context(ifmt_ctx);
        //if (inbuffer) av_free((void*)inbuffer);
        avio_context_free(&avio_in);
        avformat_free_context(ofmt_ctx);
        log_err("avformat_write_header failed, error(%s)\n", av_err2str(ret));

        return -1;
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        log_err("av_packet_alloc failed\n");
        return -1;
    }

    long long start_time = av_gettime();
    long long frame_index = 0;
    while (1)
    {
        AVStream *in_stream, *out_stream;
        ret = av_read_frame(ifmt_ctx, pkt);
        //if (ret < 0) break;

        //if (pkt->pts == AV_NOPTS_VALUE)
        //{
        //    AVRational time_base1 = ifmt_ctx->streams[videoindex]->time_base;
        //    int64_t calc_duration = (double)AV_TIME_BASE / av_q2d(ifmt_ctx->streams[videoindex]->r_frame_rate);
        //    pkt->pts = (double)(frame_index*calc_duration) / (double)(av_q2d(time_base1)*AV_TIME_BASE);
        //    pkt->dts = pkt->pts;
        //    pkt->duration = (double)calc_duration / (double)(av_q2d(time_base1)*AV_TIME_BASE);
        //}

        //if (pkt->stream_index == videoindex)
        //{
        //    AVRational time_base = ifmt_ctx->streams[videoindex]->time_base;
        //    AVRational time_base_q = { 1,AV_TIME_BASE };
        //    int64_t pts_time = av_rescale_q(pkt->dts, time_base, time_base_q);
        //    int64_t now_time = av_gettime() - start_time;

        //    AVRational avr = ifmt_ctx->streams[videoindex]->time_base;
        //    printf("avr.num:%d, avr.den:%d, pkt.dts:%ld, pkt.pts:%ld, pts_time:%ld\n",
        //            avr.num,    avr.den,    pkt->dts,     pkt->pts,     pts_time);
        //    if (pts_time > now_time)
        //    {
        //        printf("pts_time:%ld, now_time:%ld\n", pts_time, now_time);
        //        av_usleep((unsigned int)(pts_time - now_time));
        //    }
        //}

        //in_stream = ifmt_ctx->streams[pkt->stream_index];
        //out_stream = ofmt_ctx->streams[pkt->stream_index];

        //pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base, (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        //pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base, (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        //pkt->duration = (int)av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
        //pkt->pos = -1;
        //if (pkt->stream_index == videoindex) {
        //    printf("Send %8lld video frames to output URL\n", frame_index);
        //    frame_index++;
        //}

        //ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        //if (ret < 0) {
        //    printf("av_interleaved_write_frame failed\n");
        //    break;
        //}

        av_packet_unref(pkt);
    }

    log_info("capture_stop");
    capture_stop(&usbcamra);

    av_packet_free(&pkt);
    free_camra_resource(&usbcamra);
    avformat_free_context(ifmt_ctx);
    av_free((void*)inbuffer);
    avio_context_free(&avio_in);
    avformat_free_context(ofmt_ctx);

    return 0;
}

