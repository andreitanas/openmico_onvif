// Hardware test of our kernel's H.264 encoder (t20-h264): encode frames
// from the ISP (/dev/video0, as dma-bufs) or from an NV12 file to an Annex B
// stream.
//   v4l2h264 - WxH frames out.h264 [ctrl=value ...]
//   v4l2h264 in.nv12 WxH frames out.h264 [ctrl=value ...]
// A file is read frame after frame and wraps around. Controls are those of
// the encoder by name, lower case letters and digits only (e.g.
// videogopsize=25, videobitrate=2000000, or framelevelratecontrolenable=0
// h264iframeqpvalue=26). Prints the size and time of each frame.
// Build: zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os
//   -static -o v4l2h264 capture/v4l2h264.c

#include <ctype.h>
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

#define NBUF 3

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

static void normalize(char *dst, const char *src, size_t n) {
    size_t j = 0;
    for (; *src && j + 1 < n; src++)
        if (isalnum((unsigned char)*src)) dst[j++] = tolower((unsigned char)*src);
    dst[j] = 0;
}

// name=value: the encoder control with that normalized name
static int set_ctrl(int fd, const char *arg) {
    char want[64], name[64];
    const char *eq = strchr(arg, '=');
    if (!eq) return -1;
    snprintf(name, sizeof(name), "%.*s", (int)(eq - arg), arg);
    normalize(want, name, sizeof(want));
    struct v4l2_queryctrl q = {.id = V4L2_CTRL_FLAG_NEXT_CTRL};
    while (xioctl(fd, VIDIOC_QUERYCTRL, &q) == 0) {
        normalize(name, (char *)q.name, sizeof(name));
        if (!strcmp(name, want)) {
            struct v4l2_control c = {.id = q.id, .value = atoi(eq + 1)};
            if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0) return die(arg);
            return 0;
        }
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    fprintf(stderr, "no control %s\n", arg);
    return -1;
}

int main(int argc, char **argv) {
    unsigned w, h;
    int frames;
    if (argc < 5 || sscanf(argv[2], "%ux%u", &w, &h) != 2 || (frames = atoi(argv[3])) <= 0) {
        fprintf(stderr, "usage: %s in.nv12|- WxH frames out.h264 [ctrl=value ...]\n", argv[0]);
        return 2;
    }
    int fd = open_driver("t20-h264");
    if (fd < 0) {
        fprintf(stderr, "no t20-h264 video node\n");
        return 1;
    }
    for (int i = 5; i < argc; i++)
        if (set_ctrl(fd, argv[i])) return 1;

    struct v4l2_format out = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    out.fmt.pix.width = w;
    out.fmt.pix.height = h;
    out.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    out.fmt.pix.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    if (xioctl(fd, VIDIOC_S_FMT, &out) < 0) return die("S_FMT output");
    struct v4l2_format cap = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (xioctl(fd, VIDIOC_G_FMT, &cap) < 0) return die("G_FMT capture");
    unsigned frame_size = out.fmt.pix.bytesperline * out.fmt.pix.height * 3 / 2;
    printf("NV12 %ux%u -> H.264, up to %u bytes a frame\n", out.fmt.pix.width,
           out.fmt.pix.height, cap.fmt.pix.sizeimage);

    // Raw side: the ISP's buffers as dma-bufs, or ours filled from the file
    int from_isp = !strcmp(argv[1], "-"), isp = -1, dmabuf[NBUF];
    FILE *in = NULL;
    void *raw[NBUF];
    struct v4l2_requestbuffers req = {.count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                                      .memory = from_isp ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP};
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) return die("REQBUFS output");
    if (from_isp) {
        if ((isp = open("/dev/video0", O_RDWR)) < 0) return die("/dev/video0");
        struct v4l2_requestbuffers ir = {
            .count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (xioctl(isp, VIDIOC_REQBUFS, &ir) < 0) return die("ISP REQBUFS");
        for (unsigned i = 0; i < NBUF; i++) {
            struct v4l2_exportbuffer e = {.type = ir.type, .index = i, .flags = O_RDONLY};
            if (xioctl(isp, VIDIOC_EXPBUF, &e) < 0) return die("ISP EXPBUF");
            dmabuf[i] = e.fd;
            struct v4l2_buffer b = {.type = ir.type, .memory = ir.memory, .index = i};
            if (xioctl(isp, VIDIOC_QBUF, &b) < 0) return die("ISP QBUF");
        }
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(isp, VIDIOC_STREAMON, &t) < 0) return die("ISP STREAMON");
    } else {
        if (!(in = fopen(argv[1], "rb"))) return die(argv[1]);
        for (unsigned i = 0; i < NBUF; i++) {
            struct v4l2_buffer b = {.type = req.type, .memory = req.memory, .index = i};
            if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) return die("QUERYBUF output");
            raw[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
            if (raw[i] == MAP_FAILED) return die("mmap output");
        }
    }

    // Compressed side
    req = (struct v4l2_requestbuffers){
        .count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) return die("REQBUFS capture");
    void *bs[NBUF];
    for (unsigned i = 0; i < NBUF; i++) {
        struct v4l2_buffer b = {.type = req.type, .memory = req.memory, .index = i};
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) return die("QUERYBUF capture");
        bs[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, fd, b.m.offset);
        if (bs[i] == MAP_FAILED) return die("mmap capture");
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) return die("QBUF capture");
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) return die("STREAMON output");
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) return die("STREAMON capture");

    FILE *o = fopen(argv[4], "wb");
    if (!o) return die(argv[4]);
    double t0 = now();
    unsigned long total = 0;
    for (int n = 0; n < frames; n++) {
        struct v4l2_buffer ob = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
        struct v4l2_buffer ib = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (from_isp) {
            if (xioctl(isp, VIDIOC_DQBUF, &ib) < 0) return die("ISP DQBUF");
            ob.memory = V4L2_MEMORY_DMABUF;
            ob.index = ib.index;
            ob.m.fd = dmabuf[ib.index];
        } else {
            ob.memory = V4L2_MEMORY_MMAP;
            ob.index = n % NBUF;
            if (fread(raw[ob.index], 1, frame_size, in) != frame_size) {
                rewind(in);
                if (fread(raw[ob.index], 1, frame_size, in) != frame_size) return die("read");
            }
        }
        ob.bytesused = frame_size;
        double f0 = now();
        if (xioctl(fd, VIDIOC_QBUF, &ob) < 0) return die("QBUF output");
        struct v4l2_buffer cb = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (xioctl(fd, VIDIOC_DQBUF, &cb) < 0) return die("DQBUF capture");
        double f1 = now();
        ob = (struct v4l2_buffer){.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = ob.memory};
        if (xioctl(fd, VIDIOC_DQBUF, &ob) < 0) return die("DQBUF output");
        if (from_isp && xioctl(isp, VIDIOC_QBUF, &ib) < 0) return die("ISP QBUF");
        if (cb.flags & V4L2_BUF_FLAG_ERROR) {
            fprintf(stderr, "frame %d: encoder error (see dmesg)\n", n);
            return 1;
        }
        printf("frame %d: %s %u bytes, %.1f ms\n", n,
               cb.flags & V4L2_BUF_FLAG_KEYFRAME ? "I" : "P", cb.bytesused, (f1 - f0) * 1e3);
        if (fwrite(bs[cb.index], 1, cb.bytesused, o) != cb.bytesused) return die(argv[4]);
        total += cb.bytesused;
        if (xioctl(fd, VIDIOC_QBUF, &cb) < 0) return die("QBUF capture");
    }
    double t1 = now();
    fclose(o);
    printf("%s: %d frames, %lu bytes, %.1f fps, %.0f kbit/s at 25 fps\n", argv[4], frames,
           total, frames / (t1 - t0), total * 8.0 / frames * 25 / 1000);
    return 0;
}
