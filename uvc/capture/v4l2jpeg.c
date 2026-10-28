// Hardware test of our kernel's JPEG encoder (t20-jpeg): encode NV12 to JPEG
// on the VPU.
//   v4l2jpeg in.nv12 WxH out.jpg [quality]   a frame from a file
//   v4l2jpeg - WxH out.jpg [quality]         a frame from the ISP
//                                            (/dev/video0, as a dma-buf)
// The encoder is the video node whose driver is t20-jpeg. From the ISP, a
// few frames go by first for its auto white balance; the exposure stays as
// it is (v4l2grab runs auto exposure).
// Build: zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os
//   -static -o v4l2jpeg capture/v4l2jpeg.c

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ISP_FRAMES 30  // frames the ISP runs before the one encoded

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static int die(const char *what) {
    perror(what);
    return 1;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// The video node whose driver is name, or -1
static int open_driver(const char *name) {
    for (int i = 0; i < 16; i++) {
        char p[32];
        struct v4l2_capability cap;
        snprintf(p, sizeof(p), "/dev/video%d", i);
        int fd = open(p, O_RDWR);
        if (fd < 0) continue;
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && !strcmp((char *)cap.driver, name))
            return fd;
        close(fd);
    }
    return -1;
}

// One frame from the ISP's main node, exported as a dma-buf; the ISP keeps
// streaming (its buffers stay mapped) until the caller closes isp
static int isp_frame(int isp, int *dmabuf) {
    struct v4l2_requestbuffers req = {
        .count = 3, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(isp, VIDIOC_REQBUFS, &req) < 0) return die("ISP REQBUFS");
    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer b = {.type = req.type, .memory = req.memory, .index = i};
        if (xioctl(isp, VIDIOC_QBUF, &b) < 0) return die("ISP QBUF");
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(isp, VIDIOC_STREAMON, &type) < 0) return die("ISP STREAMON");
    struct v4l2_buffer b;
    for (int n = 0;; n++) {
        b = (struct v4l2_buffer){.type = type, .memory = V4L2_MEMORY_MMAP};
        if (xioctl(isp, VIDIOC_DQBUF, &b) < 0) return die("ISP DQBUF");
        if (n == ISP_FRAMES) break;
        if (xioctl(isp, VIDIOC_QBUF, &b) < 0) return die("ISP QBUF");
    }
    struct v4l2_exportbuffer e = {.type = type, .index = b.index, .flags = O_RDONLY};
    if (xioctl(isp, VIDIOC_EXPBUF, &e) < 0) return die("ISP EXPBUF");
    *dmabuf = e.fd;
    return 0;
}

int main(int argc, char **argv) {
    unsigned w, h;
    if (argc < 4 || sscanf(argv[2], "%ux%u", &w, &h) != 2) {
        fprintf(stderr, "usage: %s in.nv12|- WxH out.jpg [quality]\n", argv[0]);
        return 2;
    }
    int fd = open_driver("t20-jpeg");
    if (fd < 0) {
        fprintf(stderr, "no t20-jpeg video node\n");
        return 1;
    }
    if (argc > 4) {
        struct v4l2_control c = {.id = V4L2_CID_JPEG_COMPRESSION_QUALITY, .value = atoi(argv[4])};
        if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0) return die("quality");
    }

    struct v4l2_format out = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    out.fmt.pix.width = w;
    out.fmt.pix.height = h;
    out.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(fd, VIDIOC_S_FMT, &out) < 0) return die("S_FMT output");
    struct v4l2_format cap = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (xioctl(fd, VIDIOC_G_FMT, &cap) < 0) return die("G_FMT capture");
    printf("NV12 %ux%u (%u bytes per line) -> JPEG, up to %u bytes\n", out.fmt.pix.width,
           out.fmt.pix.height, out.fmt.pix.bytesperline, cap.fmt.pix.sizeimage);

    // Raw side: our buffer filled from the file, or the ISP's frame
    int from_isp = !strcmp(argv[1], "-"), isp = -1, dmabuf = -1;
    struct v4l2_requestbuffers req = {.count = 1, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                                      .memory = from_isp ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP};
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) return die("REQBUFS output");
    struct v4l2_buffer ob = {.type = req.type, .memory = req.memory, .index = 0};
    if (xioctl(fd, VIDIOC_QUERYBUF, &ob) < 0) return die("QUERYBUF output");
    if (from_isp) {
        if ((isp = open("/dev/video0", O_RDWR)) < 0) return die("/dev/video0");
        if (isp_frame(isp, &dmabuf)) return 1;
        ob.m.fd = dmabuf;
        ob.bytesused = out.fmt.pix.bytesperline * out.fmt.pix.height * 3 / 2;
    } else {
        void *p = mmap(NULL, ob.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, ob.m.offset);
        if (p == MAP_FAILED) return die("mmap output");
        FILE *in = fopen(argv[1], "rb");
        if (!in) return die(argv[1]);
        size_t n = fread(p, 1, ob.length, in);
        fclose(in);
        ob.bytesused = n;
    }

    // Compressed side
    req = (struct v4l2_requestbuffers){
        .count = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) return die("REQBUFS capture");
    struct v4l2_buffer cb = {.type = req.type, .memory = req.memory, .index = 0};
    if (xioctl(fd, VIDIOC_QUERYBUF, &cb) < 0) return die("QUERYBUF capture");
    void *jpg = mmap(NULL, cb.length, PROT_READ, MAP_SHARED, fd, cb.m.offset);
    if (jpg == MAP_FAILED) return die("mmap capture");

    double t0 = now();
    if (xioctl(fd, VIDIOC_QBUF, &cb) < 0) return die("QBUF capture");
    if (xioctl(fd, VIDIOC_QBUF, &ob) < 0) return die("QBUF output");
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) return die("STREAMON output");
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) return die("STREAMON capture");
    cb = (struct v4l2_buffer){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(fd, VIDIOC_DQBUF, &cb) < 0) return die("DQBUF capture");
    double t1 = now();
    if (cb.flags & V4L2_BUF_FLAG_ERROR) {
        fprintf(stderr, "encoder error (see dmesg)\n");
        return 1;
    }
    FILE *o = fopen(argv[3], "wb");
    if (!o || fwrite(jpg, 1, cb.bytesused, o) != cb.bytesused) return die(argv[3]);
    fclose(o);
    printf("%s: %u bytes in %.1f ms\n", argv[3], cb.bytesused, (t1 - t0) * 1e3);
    if (dmabuf >= 0) close(dmabuf);
    if (isp >= 0) close(isp);
    return 0;
}
