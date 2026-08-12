#include "cam_v4l2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#define MAX_BUFS 8
#define NUM_PLANES 1

struct CamFrame {
    void *start;
    size_t length;
    int export_fd;
    MppBuffer buffer;
};

struct CamV4l2 {
    int fd;
    int bufcnt;
    enum v4l2_buf_type type;
    struct CamFrame fbuf[MAX_BUFS];
};

static int xioctl(int fd, unsigned long req, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static uint64_t mono_to_realtime_ns(const struct timeval *tv)
{
    struct timespec mono, real;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);

    int64_t mono_ns = (int64_t)tv->tv_sec * 1000000000LL + (int64_t)tv->tv_usec * 1000LL;
    int64_t now_mono = (int64_t)mono.tv_sec * 1000000000LL + mono.tv_nsec;
    int64_t now_real = (int64_t)real.tv_sec * 1000000000LL + real.tv_nsec;
    return (uint64_t)(mono_ns + (now_real - now_mono));
}

CamV4l2 *cam_v4l2_open(const char *device, int width, int height, int bufcnt)
{
    if (bufcnt <= 0 || bufcnt > MAX_BUFS)
        bufcnt = 4;

    CamV4l2 *cam = calloc(1, sizeof(*cam));
    if (!cam)
        return NULL;

    cam->bufcnt = bufcnt;
    cam->fd = open(device, O_RDWR | O_CLOEXEC | O_NONBLOCK, 0);
    if (cam->fd < 0) {
        perror("open camera");
        free(cam);
        return NULL;
    }

    struct v4l2_capability cap;
    if (xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        goto fail;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    else
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cam->type = fmt.type;

    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (fmt.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
        fmt.fmt.pix_mp.num_planes = NUM_PLANES;

    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        goto fail;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = bufcnt;
    req.type = cam->type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0 || (int)req.count < bufcnt) {
        perror("VIDIOC_REQBUFS");
        goto fail;
    }
    cam->bufcnt = req.count;

    for (int i = 0; i < cam->bufcnt; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[NUM_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = cam->type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length = NUM_PLANES;
        }
        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            goto fail;
        }

        size_t len;
        off_t offset;
        if (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            len = buf.m.planes[0].length;
            offset = buf.m.planes[0].m.mem_offset;
        } else {
            len = buf.length;
            offset = buf.m.offset;
        }

        cam->fbuf[i].length = len;
        cam->fbuf[i].start = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, cam->fd, offset);
        if (cam->fbuf[i].start == MAP_FAILED) {
            perror("mmap");
            goto fail;
        }

        struct v4l2_exportbuffer expbuf;
        memset(&expbuf, 0, sizeof(expbuf));
        expbuf.type = cam->type;
        expbuf.index = i;
        expbuf.flags = O_CLOEXEC;
        if (xioctl(cam->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
            perror("VIDIOC_EXPBUF");
            goto fail;
        }
        cam->fbuf[i].export_fd = expbuf.fd;

        MppBufferInfo info;
        memset(&info, 0, sizeof(info));
        info.type = MPP_BUFFER_TYPE_EXT_DMA;
        info.fd = expbuf.fd;
        info.size = len & 0x07ffffff;
        info.index = (len & 0xf8000000) >> 27;
        if (mpp_buffer_import(&cam->fbuf[i].buffer, &info)) {
            fprintf(stderr, "mpp_buffer_import failed\n");
            goto fail;
        }
    }

    for (int i = 0; i < cam->bufcnt; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[NUM_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = cam->type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length = NUM_PLANES;
        }
        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            goto fail;
        }
    }

    enum v4l2_buf_type type = cam->type;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        goto fail;
    }

    /* Drop a few startup frames (tolerate timeout) */
    for (int i = 0; i < cam->bufcnt; i++) {
        uint64_t pts;
        uint32_t seq;
        int idx = cam_v4l2_get_frame(cam, &pts, &seq);
        if (idx >= 0)
            cam_v4l2_put_frame(cam, idx);
        else
            break;
    }

    return cam;

fail:
    cam_v4l2_close(cam);
    return NULL;
}

void cam_v4l2_close(CamV4l2 *cam)
{
    if (!cam)
        return;

    if (cam->fd >= 0) {
        enum v4l2_buf_type type = cam->type;
        xioctl(cam->fd, VIDIOC_STREAMOFF, &type);

        for (int i = 0; i < cam->bufcnt; i++) {
            if (cam->fbuf[i].buffer)
                mpp_buffer_put(cam->fbuf[i].buffer);
            if (cam->fbuf[i].start && cam->fbuf[i].start != MAP_FAILED)
                munmap(cam->fbuf[i].start, cam->fbuf[i].length);
            if (cam->fbuf[i].export_fd > 0)
                close(cam->fbuf[i].export_fd);
        }
        close(cam->fd);
    }
    free(cam);
}

int cam_v4l2_get_frame(CamV4l2 *cam, uint64_t *pts_ns, uint32_t *sequence)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[NUM_PLANES];
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    int sel = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (sel == 0) {
        fprintf(stderr, "cam DQBUF timeout (no frame)\n");
        return -1;
    }
    if (sel < 0) {
        perror("select");
        return -1;
    }

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = cam->type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = NUM_PLANES;
    }

    if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("VIDIOC_DQBUF");
        return -1;
    }

    if ((int)buf.index >= cam->bufcnt)
        return -1;

    if (pts_ns)
        *pts_ns = mono_to_realtime_ns(&buf.timestamp);
    if (sequence)
        *sequence = buf.sequence;

    return (int)buf.index;
}

int cam_v4l2_put_frame(CamV4l2 *cam, int index)
{
    if (index < 0)
        return 0;

    struct v4l2_buffer buf;
    struct v4l2_plane planes[NUM_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = cam->type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    if (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = NUM_PLANES;
    }

    if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("VIDIOC_QBUF");
        return -1;
    }
    return 0;
}

MppBuffer cam_v4l2_buf(CamV4l2 *cam, int index)
{
    if (!cam || index < 0 || index >= cam->bufcnt)
        return NULL;
    MppBuffer buf = cam->fbuf[index].buffer;
    if (buf)
        mpp_buffer_sync_end(buf);
    return buf;
}
