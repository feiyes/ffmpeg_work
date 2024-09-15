#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "log.h"

#define BUFFER_COUNT    4
#define DUMP_JPEG_NAME  "1.jpeg"

int main(int argc, char** argv)
{
    int ret = -1;
    char* dev_name = "/dev/video0";
    unsigned int   buf_len[BUFFER_COUNT];
    unsigned char *buf_addr[BUFFER_COUNT];
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    int fd = open(dev_name, O_RDWR);
    if (fd < 0) {
        log_err("open %s failed", dev_name);
        return -1;
    }

    struct v4l2_format vfmt;
    memset(&vfmt, 0, sizeof(vfmt));
    vfmt.type = type;
    ret = ioctl(fd, VIDIOC_G_FMT, &vfmt);
    if (ret < 0) {
        log_err("%s get format failed", dev_name);
        return -1;
    }

    vfmt.type = type;
    vfmt.fmt.pix.width  = 640;
    vfmt.fmt.pix.height = 480;
    vfmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    ret = ioctl(fd, VIDIOC_S_FMT, &vfmt);
    if (ret < 0) {
        log_err("%s set format failed", dev_name);
        return -1;
    }

    struct v4l2_requestbuffers req_buf;
    req_buf.type   = type;
    req_buf.count  = BUFFER_COUNT;
    req_buf.memory = V4L2_MEMORY_MMAP;
    ret = ioctl(fd, VIDIOC_REQBUFS, &req_buf);
    if (ret < 0) {
        log_err("%s request buffers failed", dev_name);
        return -1;
    }

    struct v4l2_buffer q_buf;
    q_buf.type = type;
    for (int i = 0; i < BUFFER_COUNT; i++) {
        q_buf.index = i;
        ret = ioctl(fd, VIDIOC_QUERYBUF, &q_buf);
        if (ret < 0) {
            log_err("%s request buffers failed", dev_name);
            return -1;
        }

        buf_len[i]  = q_buf.length;
        buf_addr[i] = (unsigned char *)mmap(NULL, q_buf.length, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd, q_buf.m.offset);
        ret = ioctl(fd, VIDIOC_QBUF, &q_buf);
        if (ret < 0) {
            log_err("%s queue buffers failed", dev_name);
            return -1;
        }
    }

    ret = ioctl(fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        log_err("%s video stream on failed", dev_name);
        return -1;
    }

    struct v4l2_buffer d_buf;
    d_buf.type = type;
    ret = ioctl(fd, VIDIOC_DQBUF, &d_buf);
    if (ret < 0) {
        log_err("%s dequeue buffer failed", dev_name);
        return -1;
    }

    FILE *fp = fopen(DUMP_JPEG_NAME, "w+");
    if (!fp) {
        log_err("%s open %s failed", dev_name, DUMP_JPEG_NAME);
        return -1;
    }

    fwrite(buf_addr[d_buf.index], d_buf.length, 1, fp);

    fclose(fp);

    ret = ioctl(fd, VIDIOC_QBUF, &d_buf);
    if (ret < 0) {
        log_err("%s queue buffer failed", dev_name);
        return -1;
    }

    ret = ioctl(fd, VIDIOC_STREAMOFF, &type);
    if (ret < 0) {
        log_err("%s video stream off failed", dev_name);
        return -1;
    }

    for (int i = 0; i < BUFFER_COUNT; i++)
        munmap(buf_addr[i], buf_len[i]);

    close(fd);

    return 0;
}
