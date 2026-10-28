// Hardware test of our kernel's camera drivers (jxf2x, t20-isp): stream NV12
// frames from the ISP's video node, run a simple auto exposure on the
// sensor's controls, and save frames.
//   v4l2grab out.nv12 [frames] [video] [sensor-subdev]
// Defaults: 50 frames, /dev/video0, the first /dev/v4l-subdev* with an
// exposure control (the ISP has a subdevice too). The last
// frame goes to out.nv12 (1920x1080 NV12: ffmpeg -f rawvideo -pix_fmt nv12
// -s 1920x1080 -i out.nv12 out.png); the log shows the mean luma and the
// exposure and gain auto exposure picked for each frame. EXPOSURE=lines and
// GAIN=code in the environment fix them instead (auto exposure off).
// CTRLS="name=value ..." sets controls of the video node and subdevices by
// name, lower case without spaces or punctuation (horizontalflip=1,
// whitebalanceautomatic=0, redbalance=300), and lists them all. SIZE=WxH
// sets the frame size (the second stream, /dev/video1, scales).
// Build: zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os
//   -static -o v4l2grab capture/v4l2grab.c

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NBUF 3
#define TARGET_Y 110  // mean luma auto exposure aims for (limited range)

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static int ctrl_get(int fd, unsigned id, int *val) {
    struct v4l2_control c = {.id = id};
    if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0) return -1;
    *val = c.value;
    return 0;
}

static int ctrl_set(int fd, unsigned id, int val) {
    struct v4l2_control c = {.id = id, .value = val};
    return xioctl(fd, VIDIOC_S_CTRL, &c);
}

static int ctrl_range(int fd, unsigned id, int *min, int *max) {
    struct v4l2_queryctrl q = {.id = id};
    if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0) return -1;
    *min = q.minimum;
    *max = q.maximum;
    return 0;
}

// Control name as CTRLS spells it: lower case letters and digits only
static void ctrl_key(const char *name, char *key, size_t n) {
    size_t k = 0;
    for (; *name && k + 1 < n; name++)
        if (isalnum((unsigned char)*name)) key[k++] = tolower((unsigned char)*name);
    key[k] = 0;
}

// Set the controls named in list on fd, and print every control of fd
static void set_ctrls(int fd, const char *path, const char *list) {
    struct v4l2_queryctrl q = {.id = V4L2_CTRL_FLAG_NEXT_CTRL};
    for (; xioctl(fd, VIDIOC_QUERYCTRL, &q) == 0; q.id |= V4L2_CTRL_FLAG_NEXT_CTRL) {
        char key[32], want[48];
        int v;
        if (q.type == V4L2_CTRL_TYPE_CTRL_CLASS) continue;
        ctrl_key((const char *)q.name, key, sizeof(key));
        snprintf(want, sizeof(want), "%s=", key);
        const char *m = strstr(list, want);
        if (m && (m == list || m[-1] == ' ' || m[-1] == ','))
            if (ctrl_set(fd, q.id, atoi(m + strlen(want))) < 0) perror(key);
        if (ctrl_get(fd, q.id, &v) == 0)
            printf("%s: %s = %d (%d..%d)\n", path, key, v, q.minimum, q.maximum);
    }
}

// Mean of every 16th luma sample of every 16th line
static unsigned mean_luma(const uint8_t *y, unsigned w, unsigned h, unsigned stride) {
    unsigned long sum = 0, n = 0;
    for (unsigned r = 0; r < h; r += 16)
        for (unsigned c = 0; c < w; c += 16, n++) sum += y[r * stride + c];
    return n ? sum / n : 0;
}

// Proportional step on exposure (lines) first, analog gain code once the
// exposure is at its maximum, back down the same way.
struct ae {
    int fd, expo, expo_min, expo_max, gain, gain_min, gain_max;
};

static void ae_step(struct ae *a, unsigned luma) {
    if (a->fd < 0 || !luma) return;
    double ratio = (double)TARGET_Y / luma;
    if (ratio > 0.9 && ratio < 1.1) return;
    if (ratio > 1 && a->expo >= a->expo_max && a->gain < a->gain_max)
        a->gain++;  // ~0.07 EV per code (jxf2x_again_log2)
    else if (ratio < 1 && a->gain > a->gain_min)
        a->gain--;
    else {
        int e = a->expo * (ratio > 2 ? 2 : ratio < 0.5 ? 0.5 : ratio);
        a->expo = e < a->expo_min ? a->expo_min : e > a->expo_max ? a->expo_max : e;
    }
    ctrl_set(a->fd, V4L2_CID_EXPOSURE, a->expo);
    ctrl_set(a->fd, V4L2_CID_ANALOGUE_GAIN, a->gain);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.nv12 [frames] [video] [sensor-subdev]\n", argv[0]);
        return 2;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 50;
    const char *vpath = argc > 3 ? argv[3] : "/dev/video0";
    char spath[32] = "";
    if (argc > 4) snprintf(spath, sizeof(spath), "%s", argv[4]);
    for (int i = 0; !spath[0] && i < 8; i++) {
        char p[32];
        int v, sfd;
        snprintf(p, sizeof(p), "/dev/v4l-subdev%d", i);
        if ((sfd = open(p, O_RDWR)) < 0) continue;
        if (ctrl_get(sfd, V4L2_CID_EXPOSURE, &v) == 0) memcpy(spath, p, sizeof(p));
        close(sfd);
    }

    int fd = open(vpath, O_RDWR);
    if (fd < 0) {
        perror(vpath);
        return 1;
    }
    struct v4l2_capability cap;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        return 1;
    }
    printf("%s: %s (%s)\n", vpath, (char *)cap.card, (char *)cap.driver);

    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        perror("VIDIOC_G_FMT");
        return 1;
    }
    const char *size = getenv("SIZE");
    if (size && sscanf(size, "%ux%u", &fmt.fmt.pix.width, &fmt.fmt.pix.height) == 2 &&
        xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        return 1;
    }
    struct v4l2_pix_format *pix = &fmt.fmt.pix;
    printf("format %ux%u %.4s, %u bytes per line, %u bytes\n", pix->width, pix->height,
           (char *)&pix->pixelformat, pix->bytesperline, pix->sizeimage);

    struct ae ae = {.fd = open(spath, O_RDWR)};
    if (ae.fd < 0 || ctrl_get(ae.fd, V4L2_CID_EXPOSURE, &ae.expo) < 0 ||
        ctrl_get(ae.fd, V4L2_CID_ANALOGUE_GAIN, &ae.gain) < 0 ||
        ctrl_range(ae.fd, V4L2_CID_EXPOSURE, &ae.expo_min, &ae.expo_max) < 0 ||
        ctrl_range(ae.fd, V4L2_CID_ANALOGUE_GAIN, &ae.gain_min, &ae.gain_max) < 0) {
        fprintf(stderr, "%s: no exposure controls, auto exposure off\n",
                spath[0] ? spath : "/dev/v4l-subdev*");
        if (ae.fd >= 0) close(ae.fd);
        ae.fd = -1;
    }

    const char *ctrls = getenv("CTRLS");
    if (ctrls) {
        set_ctrls(fd, vpath, ctrls);
        for (int i = 0; i < 8; i++) {
            char p[32];
            int sfd;
            snprintf(p, sizeof(p), "/dev/v4l-subdev%d", i);
            if ((sfd = open(p, O_RDWR)) < 0) continue;
            set_ctrls(sfd, p, ctrls);
            close(sfd);
        }
    }

    const char *fe = getenv("EXPOSURE"), *fg = getenv("GAIN");
    if (ae.fd >= 0 && (fe || fg)) {
        if (fe) ctrl_set(ae.fd, V4L2_CID_EXPOSURE, ae.expo = atoi(fe));
        if (fg) ctrl_set(ae.fd, V4L2_CID_ANALOGUE_GAIN, ae.gain = atoi(fg));
        close(ae.fd);
        ae.fd = -1;
    }

    struct v4l2_requestbuffers req = {
        .count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        perror("VIDIOC_REQBUFS");
        return 1;
    }
    void *mem[NBUF];
    size_t len[NBUF];
    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer b = {.type = req.type, .memory = req.memory, .index = i};
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            perror("VIDIOC_QUERYBUF");
            return 1;
        }
        len[i] = b.length;
        mem[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (mem[i] == MAP_FAILED || xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            perror("buffer");
            return 1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return 1;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int got = 0;
    for (; got < frames; got++) {
        struct v4l2_buffer b = {.type = type, .memory = V4L2_MEMORY_MMAP};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = {.tv_sec = 2};
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            fprintf(stderr, "no frame in 2 s after %d frames\n", got);
            break;
        }
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            perror("VIDIOC_DQBUF");
            break;
        }
        unsigned luma = mean_luma(mem[b.index], pix->width, pix->height, pix->bytesperline);
        printf("frame %u: seq %u, mean Y %u, exposure %d, gain %d\n", got, b.sequence, luma,
               ae.expo, ae.gain);
        ae_step(&ae, luma);
        if (got == frames - 1) {
            FILE *fp = fopen(argv[1], "wb");
            if (!fp || fwrite(mem[b.index], 1, pix->sizeimage, fp) != pix->sizeimage)
                perror(argv[1]);
            if (fp) fclose(fp);
        }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            perror("VIDIOC_QBUF");
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d frames in %.1f s (%.1f fps)\n", got, s, s > 0 ? got / s : 0);
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < req.count; i++) munmap(mem[i], len[i]);
    return got == frames ? 0 : 1;
}
