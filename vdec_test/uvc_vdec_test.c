/*
 * UVC 카메라 MJPEG 스트림 -> Rockit VDEC 디코딩 테스트
 *
 * usage: uvc_vdec_test <dev> <width> <height> [frames] [fps] [dump_prefix]
 *   e.g. uvc_vdec_test /dev/video21 640 480 300 30 /tmp/uvc
 *   dump_prefix 를 주면 첫 MJPEG 프레임(.mjpg)과 디코딩 결과(.yuv)를 저장
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "rk_comm_vdec.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"

#define CHN       0
#define V4L2_BUFS 4

struct v4l2_map {
    void *start;
    size_t length;
};

static long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* SOI 이후 마커를 훑어 DHT 존재 여부와 SOF0 크로마 샘플링을 확인 */
static void inspect_jpeg(const unsigned char *p, size_t len, int *has_dht, char *sampling, size_t ssz) {
    size_t i = 2;

    *has_dht = 0;
    snprintf(sampling, ssz, "?");
    while (i + 4 <= len && p[i] == 0xFF) {
        unsigned char m = p[i + 1];
        size_t seglen = (p[i + 2] << 8) | p[i + 3];

        if (m == 0xC4)
            *has_dht = 1;
        if ((m == 0xC0 || m == 0xC1 || m == 0xC2) && i + 10 + 3 * 3 <= len) {
            int n = p[i + 9], k;
            char *o = sampling;
            size_t left = ssz;
            for (k = 0; k < n && k < 3; k++) {
                int w = snprintf(o, left, "%s%dx%d", k ? "," : "", p[i + 11 + 3 * k] >> 4,
                                 p[i + 11 + 3 * k] & 0xF);
                o += w;
                left -= w;
            }
            if (m == 0xC2)
                snprintf(o, left, " (progressive)");
        }
        if (m == 0xDA)
            break;
        i += 2 + seglen;
    }
}

static void dump(const char *path, const void *buf, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(buf, 1, len, fp);
        fclose(fp);
        printf("    dumped %s (%zu bytes)\n", path, len);
    }
}

int main(int argc, char **argv) {
    const char *dev, *dump_prefix = NULL;
    int width, height, frames = 300, fps = 30;
    int fd, i, ok = 0, fail = 0, has_dht = 0;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_streamparm parm;
    struct v4l2_requestbuffers req;
    struct v4l2_map maps[V4L2_BUFS];
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    VDEC_CHN_ATTR_S attr;
    MB_BLK mb = RK_NULL;
    char sampling[64];
    long long t_start, t_dec = 0, t_dec_max = 0, bytes = 0;
    RK_S32 ret;

    if (argc < 4) {
        printf("usage: %s <dev> <width> <height> [frames] [fps] [dump_prefix]\n", argv[0]);
        return 1;
    }
    dev = argv[1];
    width = atoi(argv[2]);
    height = atoi(argv[3]);
    if (argc > 4)
        frames = atoi(argv[4]);
    if (argc > 5)
        fps = atoi(argv[5]);
    if (argc > 6)
        dump_prefix = argv[6];

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror(dev);
        return 1;
    }
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
        printf("camera: %s (%s, bus %s)\n", cap.card, cap.driver, cap.bus_info);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        return 1;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        printf("camera does not support MJPEG\n");
        return 1;
    }
    width = fmt.fmt.pix.width;
    height = fmt.fmt.pix.height;

    memset(&parm, 0, sizeof(parm));
    parm.type = type;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    xioctl(fd, VIDIOC_S_PARM, &parm);
    xioctl(fd, VIDIOC_G_PARM, &parm);
    printf("format: MJPEG %dx%d @ %u/%u fps\n", width, height,
           parm.parm.capture.timeperframe.denominator, parm.parm.capture.timeperframe.numerator);

    memset(&req, 0, sizeof(req));
    req.count = V4L2_BUFS;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        perror("VIDIOC_REQBUFS");
        return 1;
    }
    for (i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        xioctl(fd, VIDIOC_QUERYBUF, &b);
        maps[i].length = b.length;
        maps[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        xioctl(fd, VIDIOC_QBUF, &b);
    }

    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        printf("RK_MPI_SYS_Init failed: 0x%x\n", ret);
        return 1;
    }
    memset(&attr, 0, sizeof(attr));
    attr.enMode = VIDEO_MODE_FRAME;
    attr.enType = RK_VIDEO_ID_MJPEG;
    attr.u32PicWidth = width;
    attr.u32PicHeight = height;
    attr.u32FrameBufCnt = 3;
    attr.u32StreamBufCnt = 2;
    attr.u32FrameBufSize = width * height * 3; /* 4:4:4 대비 */
    ret = RK_MPI_VDEC_CreateChn(CHN, &attr);
    printf("RK_MPI_VDEC_CreateChn: 0x%x\n", ret);
    if (ret != RK_SUCCESS)
        goto exit_sys;
    ret = RK_MPI_VDEC_StartRecvStream(CHN);
    if (ret != RK_SUCCESS)
        goto destroy;

    /* 스트림 입력용 MB: 프레임 크기 상한 (w*h*2) */
    ret = RK_MPI_MMZ_Alloc(&mb, width * height * 2, RK_MMZ_ALLOC_CACHEABLE);
    if (ret != RK_SUCCESS) {
        printf("RK_MPI_MMZ_Alloc failed: 0x%x\n", ret);
        goto stop;
    }

    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        goto stop;
    }

    t_start = now_us();
    for (i = 0; i < frames; i++) {
        struct v4l2_buffer b;
        VDEC_STREAM_S stream;
        VIDEO_FRAME_INFO_S frame;
        long long t0, dt;
        size_t len;

        memset(&b, 0, sizeof(b));
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            perror("VIDIOC_DQBUF");
            break;
        }
        len = b.bytesused;
        if (len > (size_t)width * height * 2)
            len = width * height * 2;
        memcpy(RK_MPI_MB_Handle2VirAddr(mb), maps[b.index].start, len);
        RK_MPI_SYS_MmzFlushCache(mb, RK_FALSE);
        bytes += len;

        if (i == 0) {
            const unsigned char *p = maps[b.index].start;
            inspect_jpeg(p, len, &has_dht, sampling, sizeof(sampling));
            printf("first frame: %zu bytes, SOI=%02x%02x, DHT=%s, sampling(HxV Y,Cb,Cr)=%s\n", len, p[0],
                   p[1], has_dht ? "yes" : "NO (default Huffman table needed)", sampling);
            if (dump_prefix) {
                char path[256];
                snprintf(path, sizeof(path), "%s.mjpg", dump_prefix);
                dump(path, p, len);
            }
        }
        xioctl(fd, VIDIOC_QBUF, &b);

        memset(&stream, 0, sizeof(stream));
        stream.pMbBlk = mb;
        stream.u32Len = len;
        stream.u64PTS = i;
        stream.bEndOfFrame = RK_TRUE;

        t0 = now_us();
        ret = RK_MPI_VDEC_SendStream(CHN, &stream, 1000);
        if (ret == RK_SUCCESS) {
            memset(&frame, 0, sizeof(frame));
            ret = RK_MPI_VDEC_GetFrame(CHN, &frame, 1000);
        }
        dt = now_us() - t0;
        if (ret != RK_SUCCESS) {
            if (fail++ < 5)
                printf("[%d] decode failed: 0x%x (%zu bytes)\n", i, ret, len);
            continue;
        }
        t_dec += dt;
        if (dt > t_dec_max)
            t_dec_max = dt;
        if (ok == 0) {
            printf("decoded: %ux%u pixfmt=%d buf=%llu\n", frame.stVFrame.u32Width, frame.stVFrame.u32Height,
                   frame.stVFrame.enPixelFormat,
                   (unsigned long long)RK_MPI_MB_GetSize(frame.stVFrame.pMbBlk));
            if (dump_prefix) {
                char path[256];
                snprintf(path, sizeof(path), "%s.yuv", dump_prefix);
                RK_MPI_SYS_MmzFlushCache(frame.stVFrame.pMbBlk, RK_TRUE);
                dump(path, RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk),
                     RK_MPI_MB_GetSize(frame.stVFrame.pMbBlk));
            }
        }
        ok++;
        RK_MPI_VDEC_ReleaseFrame(CHN, &frame);
    }

    {
        double secs = (now_us() - t_start) / 1e6;
        printf("RESULT: %d ok / %d fail / %d frames in %.2f s -> %.1f fps, avg %.1f KB/frame\n", ok, fail, i,
               secs, i / secs, i ? bytes / 1024.0 / i : 0);
        if (ok)
            printf("decode time: avg %.2f ms, max %.2f ms\n", t_dec / 1000.0 / ok, t_dec_max / 1000.0);
    }

    xioctl(fd, VIDIOC_STREAMOFF, &type);
stop:
    if (mb)
        RK_MPI_MB_ReleaseMB(mb);
    RK_MPI_VDEC_StopRecvStream(CHN);
destroy:
    RK_MPI_VDEC_DestroyChn(CHN);
exit_sys:
    RK_MPI_SYS_Exit();
    close(fd);
    return ok > 0 ? 0 : 1;
}
