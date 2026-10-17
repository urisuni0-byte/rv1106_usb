/*
 * camhelper - UVC 카메라 캡처 + RV1106 하드웨어 H.264 인코딩 도우미
 *
 * scannerd(Go)가 자식 프로세스로 띄워 두고 stdin 명령으로 모드를 바꾼다. 카메라와 인코더는
 * C 쪽(V4L2 헤더, rockit)에서 다루고, 결과만 stdout 으로 레코드 단위로 넘긴다.
 *
 *   camhelper run [-d /dev/videoN] [-W 640 -H 480 -f 20 -b 2000 -g 20]   미리보기 크기/화질
 *                 [-A 0-700] [-P far] [-M continuous]                    AF 탐색 범위 (MIPI 렌즈 VCM 0~1023), 먼 쪽(두피) 우선,
 *                                                                        미리보기 중 연속 AF(촬영은 바로) 또는 snap(촬영 때 AF)
 *                 [-X 2592 -Y 1944 -s 4]                                 사진 크기 / snap 전에 버릴 프레임
 *     stdin 명령 (한 줄씩)
 *       preview          YUYV(또는 NV12) 캡처 -> NV12 -> VENC H.264 -> 'H'
 *       photo            MJPEG 고해상도 캡처. 프레임마다 소프트웨어 디코딩 -> 축소 -> VENC 로
 *                        미리보기를 계속 낸다 (CPU 가 허락하는 만큼, 약 5fps)
 *       photo nopreview  MJPEG 캡처만 (미리보기를 보는 앱이 없을 때)
 *       snap             (photo 모드) -s 장을 버린 뒤 다음 프레임을 'J' 로
 *       idle             카메라 스트리밍 중지 (프로세스와 인코더는 유지)
 *       idr              키프레임 요청
 *       ctrls <seq>      카메라 컨트롤 목록 -> 'C'
 *       ctrl <seq> <id> <value>   컨트롤 설정 -> 'C' (바뀐 뒤의 전체 목록)
 *       focus on|off     초점 맞춤 보조: 미리보기 가운데 영역의 선명도를 200ms 마다 'F' 로
 *       quit             종료
 *     모드가 바뀔 때마다 'I' 레코드 {"mode":...} 를 낸다.
 *     'C' 는 {"seq":N,"ok":bool,"error":"...","controls":[...]} 이다. 카메라가 끊겼다 다시
 *     잡히면 {"seq":0,"reopened":true} 를 내서 scannerd 가 저장된 설정을 다시 넣게 한다.
 *
 *   camhelper list [-d /dev/videoN]     지원 포맷/해상도 출력 (사람용)
 *
 * stdout 레코드: [type u8][len u32 BE][pts_us u64 BE][payload]
 *   'I' 정보 JSON, 'H' H.264 Annex-B 접근 단위, 'J' JPEG, 'E' 오류 문자열, 'C' 컨트롤 JSON,
 *   'F' 선명도 JSON {"score","brightness","frames","source":"preview|photo","roi":0.5}
 * 사람이 읽을 로그는 stderr 로만 쓴다.
 *
 * UVC 카메라가 없고 rkisp 에 MIPI 센서(IMX708 등)가 붙어 있으면 MIPI 모드로 돈다 (run_mipi 참고).
 * 명령과 레코드는 같고, 미리보기는 ISP selfpath, 사진은 ISP mainpath + 하드웨어 JPEG 이다.
 * 렌즈 VCM 이 있으면 콘트라스트 AF 를 한다: 자동 초점이 켜져 있으면 미리보기 시작과 snap 직전에 맞추고,
 * 'af' 명령으로 언제든 다시 맞춘다. 컨트롤은 "자동 초점"(0x009a090c)과 "초점 위치"(0x009a090a, 0~1023).
 * 초점 상태는 'A' 레코드 {"auto","state":"searching|locked|manual","pos"} 로 낸다.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/i2c-dev.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#include "rk_comm_vdec.h"
#include "rk_comm_venc.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"

#define MAX_BUFS 8

struct buf {
    void *start;
    size_t len;
};

static int cam_fd = -1;
static struct buf bufs[MAX_BUFS];
static unsigned nbufs;
static int streaming;
static int out_fd = 1;
static uint64_t t0;

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[camhelper] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static int write_all(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(out_fd, b, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) { /* stdout 이 non-blocking 으로 물려받은 경우 */
                struct pollfd p = {out_fd, POLLOUT, 0};
                poll(&p, 1, 1000);
                continue;
            }
            logf_("stdout 쓰기 실패: %s", strerror(errno));
            return -1;
        }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* 레코드 하나를 쓴다. stdout 이 닫히면(scannerd 종료) 우리도 끝낸다. */
static void emit(char type, const void *data, uint32_t len) {
    uint64_t pts = now_us() - t0;
    uint8_t h[13];
    h[0] = (uint8_t)type;
    h[1] = len >> 24; h[2] = len >> 16; h[3] = len >> 8; h[4] = len;
    for (int i = 0; i < 8; i++)
        h[5 + i] = (uint8_t)(pts >> (56 - 8 * i));
    if (write_all(h, sizeof h) || (len && write_all(data, len))) {
        logf_("stdout 닫힘 - 종료");
        exit(0);
    }
}

static void emitf(char type, const char *fmt, ...) {
    char s[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof s)
        n = sizeof s - 1;
    emit(type, s, (uint32_t)n);
}

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static const char *fourcc(uint32_t f, char s[5]) {
    s[0] = f & 0xff; s[1] = (f >> 8) & 0xff; s[2] = (f >> 16) & 0xff; s[3] = f >> 24; s[4] = 0;
    return s;
}

/* ------------------------------------------------------------------ V4L2 */

static int is_uvc_capture(const char *path) {
    struct v4l2_capability cap;
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return 0;
    memset(&cap, 0, sizeof cap);
    int ok = xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
    close(fd);
    if (!ok)
        return 0;
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    return strcmp((char *)cap.driver, "uvcvideo") == 0 && (caps & V4L2_CAP_VIDEO_CAPTURE) &&
           (caps & V4L2_CAP_STREAMING);
}

/* /dev/videoN 중 UVC 캡처 노드 (RV1106 의 rkisp/rkcif 노드는 건너뛴다) */
static int find_uvc(char *out, size_t n) {
    for (int i = 0; i < 64; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/video%d", i);
        if (access(p, F_OK) == 0 && is_uvc_capture(p)) {
            snprintf(out, n, "%s", p);
            return 0;
        }
    }
    return -1;
}

static int has_format(uint32_t pix) {
    struct v4l2_fmtdesc d;
    for (unsigned i = 0;; i++) {
        memset(&d, 0, sizeof d);
        d.index = i;
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(cam_fd, VIDIOC_ENUM_FMT, &d) < 0)
            return 0;
        if (d.pixelformat == pix)
            return 1;
    }
}

static int cam_open(const char *dev) {
    cam_fd = open(dev, O_RDWR | O_NONBLOCK);
    if (cam_fd < 0) {
        logf_("%s 열기 실패: %s", dev, strerror(errno));
        return -1;
    }
    return 0;
}

/* 포맷/해상도/fps 설정. 드라이버가 고친 실제 값을 *w,*h,*fps 로 돌려준다. */
static int cam_config(uint32_t pix, unsigned *w, unsigned *h, unsigned *fps, unsigned *stride) {
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = *w;
    f.fmt.pix.height = *h;
    f.fmt.pix.pixelformat = pix;
    f.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(cam_fd, VIDIOC_S_FMT, &f) < 0) {
        logf_("S_FMT 실패: %s", strerror(errno));
        return -1;
    }
    if (f.fmt.pix.pixelformat != pix) {
        char a[5], b[5];
        logf_("요청 포맷 %s 대신 %s", fourcc(pix, a), fourcc(f.fmt.pix.pixelformat, b));
        return -1;
    }
    *w = f.fmt.pix.width;
    *h = f.fmt.pix.height;
    if (stride)
        *stride = f.fmt.pix.bytesperline;

    struct v4l2_streamparm p;
    memset(&p, 0, sizeof p);
    p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam_fd, VIDIOC_G_PARM, &p) == 0 && (p.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        p.parm.capture.timeperframe.numerator = 1;
        p.parm.capture.timeperframe.denominator = *fps;
        xioctl(cam_fd, VIDIOC_S_PARM, &p);
        if (xioctl(cam_fd, VIDIOC_G_PARM, &p) == 0 && p.parm.capture.timeperframe.numerator)
            *fps = p.parm.capture.timeperframe.denominator / p.parm.capture.timeperframe.numerator;
    }
    return 0;
}

static int cam_start(unsigned count) {
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = count;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        logf_("REQBUFS 실패: %s", strerror(errno));
        return -1;
    }
    nbufs = rb.count > MAX_BUFS ? MAX_BUFS : rb.count;
    for (unsigned i = 0; i < nbufs; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(cam_fd, VIDIOC_QUERYBUF, &b) < 0)
            return -1;
        bufs[i].len = b.length;
        bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam_fd, b.m.offset);
        if (bufs[i].start == MAP_FAILED)
            return -1;
        if (xioctl(cam_fd, VIDIOC_QBUF, &b) < 0)
            return -1;
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam_fd, VIDIOC_STREAMON, &t) < 0) {
        logf_("STREAMON 실패: %s", strerror(errno));
        return -1;
    }
    streaming = 1;
    return 0;
}

/* 스트리밍만 멈추고 버퍼를 돌려준다 (fd 는 유지 -> 초점 같은 컨트롤 설정이 그대로 남는다) */
static void cam_halt(void) {
    if (cam_fd < 0)
        return;
    if (streaming) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(cam_fd, VIDIOC_STREAMOFF, &t);
        streaming = 0;
    }
    for (unsigned i = 0; i < nbufs; i++)
        munmap(bufs[i].start, bufs[i].len);
    if (nbufs) {
        struct v4l2_requestbuffers rb;
        memset(&rb, 0, sizeof rb);
        rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        rb.memory = V4L2_MEMORY_MMAP;
        xioctl(cam_fd, VIDIOC_REQBUFS, &rb); /* count=0: 이걸 해야 S_FMT 가 EBUSY 없이 된다 */
    }
    nbufs = 0;
}

/* 프레임 하나를 꺼낸다. 0 성공, 1 아직 없음, -1 오류. 다 쓰면 cam_release() */
static int cam_dequeue(struct v4l2_buffer *b) {
    memset(b, 0, sizeof *b);
    b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam_fd, VIDIOC_DQBUF, b) < 0)
        return errno == EAGAIN ? 1 : -1;
    return 0;
}

static void cam_release(struct v4l2_buffer *b) { xioctl(cam_fd, VIDIOC_QBUF, b); }

/* ----------------------------------------------------------------- stdin */

static char cmdbuf[256];
static size_t cmdlen;

/* 완성된 명령 한 줄을 line 에 담아 1, 아직이면 0, EOF 면 -1 */
static int read_cmd(char *line, size_t n) {
    for (;;) {
        char *nl = memchr(cmdbuf, '\n', cmdlen);
        if (nl) {
            size_t k = (size_t)(nl - cmdbuf);
            snprintf(line, n, "%.*s", (int)k, cmdbuf);
            memmove(cmdbuf, nl + 1, cmdlen - k - 1);
            cmdlen -= k + 1;
            return 1;
        }
        if (cmdlen == sizeof cmdbuf)
            cmdlen = 0; /* 너무 긴 줄은 버린다 */
        ssize_t r = read(0, cmdbuf + cmdlen, sizeof cmdbuf - cmdlen);
        if (r == 0)
            return -1;
        if (r < 0)
            return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
        cmdlen += (size_t)r;
    }
}

/* ----------------------------------------------------------------- 인코더 */

static unsigned pw = 640, ph = 480, pfps = 20, kbps = 2000, gop = 20; /* 미리보기 */
static unsigned xw = 2592, xh = 1944, skip = 4;                        /* 사진 */

static MB_POOL pool = MB_INVALID_POOLID;
static int venc_ready, vdec_ready;
static unsigned vdec_w, vdec_h;

/* 미리보기 인코더: 입력은 언제나 pw x ph NV12 */
static int venc_open(void) {
    VENC_CHN_ATTR_S a;
    memset(&a, 0, sizeof a);
    a.stVencAttr.enType = RK_VIDEO_ID_AVC;
    a.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    /* 브라우저/안드로이드 WebRTC 가 모두 받는 Baseline (profile-level-id 42e0xx) */
    a.stVencAttr.u32Profile = H264E_PROFILE_BASELINE;
    a.stVencAttr.u32PicWidth = pw;
    a.stVencAttr.u32PicHeight = ph;
    a.stVencAttr.u32VirWidth = pw;
    a.stVencAttr.u32VirHeight = ph;
    a.stVencAttr.u32MaxPicWidth = pw;
    a.stVencAttr.u32MaxPicHeight = ph;
    a.stVencAttr.u32StreamBufCnt = 3;
    a.stVencAttr.u32BufSize = pw * ph * 3 / 2;
    a.stVencAttr.stAttrH264e.u32Level = 31;
    a.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    a.stRcAttr.stH264Cbr.u32Gop = gop;
    a.stRcAttr.stH264Cbr.u32BitRate = kbps;
    a.stRcAttr.stH264Cbr.u32SrcFrameRateNum = pfps;
    a.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    a.stRcAttr.stH264Cbr.fr32DstFrameRateNum = pfps;
    a.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    a.stRcAttr.stH264Cbr.u32StatTime = 2;
    a.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;

    RK_S32 r = RK_MPI_VENC_CreateChn(0, &a);
    if (r != RK_SUCCESS) {
        logf_("VENC_CreateChn 실패 0x%x", r);
        return -1;
    }
    VENC_RECV_PIC_PARAM_S rp;
    memset(&rp, 0, sizeof rp);
    rp.s32RecvPicNum = -1;
    if ((r = RK_MPI_VENC_StartRecvFrame(0, &rp)) != RK_SUCCESS) {
        logf_("VENC_StartRecvFrame 실패 0x%x", r);
        RK_MPI_VENC_DestroyChn(0);
        return -1;
    }
    MB_POOL_CONFIG_S pc;
    memset(&pc, 0, sizeof pc);
    pc.u64MBSize = (RK_U64)pw * ph * 3 / 2;
    pc.u32MBCnt = 3;
    pc.enAllocType = MB_ALLOC_TYPE_DMA;
    pc.enRemapMode = MB_REMAP_MODE_CACHED;
    pool = RK_MPI_MB_CreatePool(&pc);
    if (pool == MB_INVALID_POOLID) {
        logf_("MB 풀 생성 실패");
        return -1;
    }
    venc_ready = 1;
    return 0;
}

static void venc_close(void) {
    if (venc_ready) {
        RK_MPI_VENC_StopRecvFrame(0);
        RK_MPI_VENC_DestroyChn(0);
        venc_ready = 0;
    }
    if (pool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(pool);
        pool = MB_INVALID_POOLID;
    }
}

/* ------------------------------------------- 초점 맞춤 보조 (선명도) */

/* 인코더에 넣기 직전의 pw x ph Y 평면에서 가운데 1/2 x 1/2 영역의 라플라시안 분산을 잰다.
 * 초점이 맞을수록 커진다. 값 자체는 장면·조명에 따라 달라지므로 렌즈를 돌리며 상대 비교용으로 쓴다.
 * 200ms 동안의 평균을 'F' 로 낸다. 비용은 640x480 기준 프레임당 약 8만 화소. */
static int focus_on;
static const char *focus_src = "preview";
static double focus_sum, focus_bright;
static unsigned focus_n;
static uint64_t focus_last;

static void focus_reset(void) {
    focus_sum = focus_bright = 0;
    focus_n = 0;
    focus_last = now_us();
}

static void focus_measure(const uint8_t *y) {
    unsigned x0 = pw / 4, y0 = ph / 4, x1 = pw - pw / 4, y1 = ph - ph / 4;
    int64_t s = 0;
    uint64_t s2 = 0, sy = 0;
    for (unsigned r = y0; r < y1; r++) {
        const uint8_t *p = y + (size_t)r * pw, *u = p - pw, *d = p + pw;
        for (unsigned c = x0; c < x1; c++) {
            int l = 4 * p[c] - p[c - 1] - p[c + 1] - u[c] - d[c];
            s += l;
            s2 += (uint64_t)(l * l);
            sy += p[c];
        }
    }
    double n = (double)(x1 - x0) * (y1 - y0), m = (double)s / n;
    focus_sum += (double)s2 / n - m * m;
    focus_bright += (double)sy / n;
    focus_n++;
    if (now_us() - focus_last >= 200000) {
        emitf('F', "{\"score\":%.1f,\"brightness\":%.0f,\"frames\":%u,\"source\":\"%s\",\"roi\":0.5}",
              focus_sum / focus_n, focus_bright / focus_n, focus_n, focus_src);
        focus_reset();
    }
}

/* fill 이 MB 에 NV12 를 채우면 인코딩해서 'H' 로 낸다 */
static int encode(void (*fill)(uint8_t *dst, const void *arg), const void *arg) {
    MB_BLK mb = RK_MPI_MB_GetMB(pool, (RK_U64)pw * ph * 3 / 2, RK_TRUE);
    if (!mb) {
        logf_("MB_GetMB 실패");
        return -1;
    }
    uint8_t *dst = RK_MPI_MB_Handle2VirAddr(mb);
    fill(dst, arg);
    if (focus_on)
        focus_measure(dst);
    RK_MPI_SYS_MmzFlushCache(mb, RK_FALSE);

    VIDEO_FRAME_INFO_S f;
    memset(&f, 0, sizeof f);
    f.stVFrame.pMbBlk = mb;
    f.stVFrame.u32Width = pw;
    f.stVFrame.u32Height = ph;
    f.stVFrame.u32VirWidth = pw;
    f.stVFrame.u32VirHeight = ph;
    f.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    f.stVFrame.u64PTS = now_us() - t0;
    RK_S32 r = RK_MPI_VENC_SendFrame(0, &f, 1000);
    RK_MPI_MB_ReleaseMB(mb); /* 인코더가 자기 참조를 잡고 있다 */
    if (r != RK_SUCCESS) {
        logf_("VENC_SendFrame 실패 0x%x", r);
        return -1;
    }

    VENC_PACK_S pack;
    VENC_STREAM_S st;
    memset(&st, 0, sizeof st);
    st.pstPack = &pack;
    r = RK_MPI_VENC_GetStream(0, &st, 1000);
    if (r != RK_SUCCESS) {
        logf_("VENC_GetStream 실패 0x%x", r);
        return -1;
    }
    for (RK_U32 i = 0; i < st.u32PackCount; i++) {
        /* rockit 샘플과 같이 VirAddr 부터 u32Len 바이트 (u32Offset 은 쓰지 않는다) */
        uint8_t *p = RK_MPI_MB_Handle2VirAddr(st.pstPack[i].pMbBlk);
        emit('H', p, st.pstPack[i].u32Len);
    }
    RK_MPI_VENC_ReleaseStream(0, &st);
    return 0;
}

/* ------------------------------------------------------- 미리보기 (YUYV) */

struct raw {
    const uint8_t *p;
    unsigned stride;
    uint32_t pix;
};

static void fill_from_camera(uint8_t *dst, const void *arg) {
    const struct raw *s = arg;
    uint8_t *y = dst, *uv = dst + pw * ph;
    if (s->pix == V4L2_PIX_FMT_NV12) {
        for (unsigned r = 0; r < ph * 3 / 2; r++)
            memcpy(dst + r * pw, s->p + r * s->stride, pw);
        return;
    }
    /* YUYV(4:2:2 packed) -> NV12 */
    for (unsigned r = 0; r < ph; r++) {
        const uint8_t *row = s->p + r * s->stride;
        uint8_t *yr = y + r * pw;
        for (unsigned c = 0; c < pw; c += 2) {
            yr[c] = row[2 * c];
            yr[c + 1] = row[2 * c + 2];
        }
        if (!(r & 1)) {
            uint8_t *u = uv + (r / 2) * pw;
            for (unsigned c = 0; c < pw; c += 2) {
                u[c] = row[2 * c + 1];
                u[c + 1] = row[2 * c + 3];
            }
        }
    }
}

/* ------------------------------------------- 사진 모드 미리보기 (MJPEG 디코딩) */

static int vdec_open(unsigned w, unsigned h) {
    VDEC_CHN_ATTR_S a;
    memset(&a, 0, sizeof a);
    a.enMode = VIDEO_MODE_FRAME;
    a.enType = RK_VIDEO_ID_MJPEG;
    a.u32PicWidth = w;
    a.u32PicHeight = h;
    a.u32FrameBufCnt = 2;
    a.u32StreamBufCnt = 2;
    RK_S32 r = RK_MPI_VDEC_CreateChn(0, &a);
    if (r != RK_SUCCESS) {
        logf_("VDEC_CreateChn 실패 0x%x", r);
        return -1;
    }
    if ((r = RK_MPI_VDEC_StartRecvStream(0)) != RK_SUCCESS) {
        logf_("VDEC_StartRecvStream 실패 0x%x", r);
        RK_MPI_VDEC_DestroyChn(0);
        return -1;
    }
    vdec_ready = 1;
    vdec_w = w;
    vdec_h = h;
    return 0;
}

static void vdec_close(void) {
    if (vdec_ready) {
        RK_MPI_VDEC_StopRecvStream(0);
        RK_MPI_VDEC_DestroyChn(0);
        vdec_ready = 0;
    }
}

/* 디코딩된 큰 YUV 를 pw x ph NV12 로 축소 (가로/세로 2탭 평균 + 최근접) */
static void fill_from_decoded(uint8_t *dst, const void *arg) {
    const VIDEO_FRAME_S *v = arg;
    const uint8_t *src = RK_MPI_MB_Handle2VirAddr(v->pMbBlk);
    unsigned sw = v->u32Width, sh = v->u32Height, vw = v->u32VirWidth, vh = v->u32VirHeight;
    const uint8_t *sy = src;
    uint8_t *y = dst, *uv = dst + pw * ph;
    static unsigned xs[4096];
    for (unsigned c = 0; c < pw; c++)
        xs[c] = (unsigned)((uint64_t)c * sw / pw);
    for (unsigned r = 0; r < ph; r++) {
        unsigned yy = (unsigned)((uint64_t)r * sh / ph);
        const uint8_t *row = sy + (size_t)yy * vw;
        uint8_t *o = y + r * pw;
        for (unsigned c = 0; c < pw; c++) {
            unsigned x = xs[c];
            o[c] = (uint8_t)((row[x] + row[x + 1 < sw ? x + 1 : x] + 1) >> 1);
        }
    }

    /* 색차 평면 위치와 표본 간격 */
    const uint8_t *cu, *cv;
    unsigned cstride, cdivy, interleaved = 0;
    switch (v->enPixelFormat) {
    case RK_FMT_YUV420SP: /* Y + UVUV (세로 1/2) */
        cu = src + (size_t)vw * vh; cv = cu + 1; cstride = vw; cdivy = 2; interleaved = 1; break;
    case RK_FMT_YUV422SP: /* Y + UVUV (세로 그대로) */
        cu = src + (size_t)vw * vh; cv = cu + 1; cstride = vw; cdivy = 1; interleaved = 1; break;
    case RK_FMT_YUV420P:
        cu = src + (size_t)vw * vh; cv = cu + (size_t)(vw / 2) * (vh / 2); cstride = vw / 2; cdivy = 2; break;
    case RK_FMT_YUV422P: /* 카메라 MJPEG(4:2:2) 를 디코딩하면 이 형식 */
    default:
        cu = src + (size_t)vw * vh; cv = cu + (size_t)(vw / 2) * vh; cstride = vw / 2; cdivy = 1; break;
    }
    for (unsigned r = 0; r < ph / 2; r++) {
        unsigned yy = (unsigned)((uint64_t)(2 * r) * sh / ph) / cdivy;
        uint8_t *o = uv + r * pw;
        for (unsigned c = 0; c < pw / 2; c++) {
            unsigned cx = xs[2 * c] / 2;
            if (interleaved) {
                o[2 * c] = cu[(size_t)yy * cstride + 2 * cx];
                o[2 * c + 1] = cv[(size_t)yy * cstride + 2 * cx];
            } else {
                o[2 * c] = cu[(size_t)yy * cstride + cx];
                o[2 * c + 1] = cv[(size_t)yy * cstride + cx];
            }
        }
    }
}

/* JPEG 한 장을 디코딩해 미리보기로 인코딩한다 (약 150ms @ 5MP) */
static int preview_from_jpeg(const uint8_t *jpg, uint32_t len, unsigned w, unsigned h) {
    if (!vdec_ready && vdec_open(w, h))
        return -1;
    MB_BLK mb = RK_NULL;
    if (RK_MPI_MMZ_Alloc(&mb, len, RK_MMZ_ALLOC_CACHEABLE) != RK_SUCCESS)
        return -1;
    memcpy(RK_MPI_MB_Handle2VirAddr(mb), jpg, len);
    RK_MPI_SYS_MmzFlushCache(mb, RK_FALSE);

    VDEC_STREAM_S s;
    memset(&s, 0, sizeof s);
    s.pMbBlk = mb;
    s.u32Len = len;
    s.bEndOfFrame = RK_TRUE;
    RK_S32 r = RK_MPI_VDEC_SendStream(0, &s, 1000);
    RK_MPI_MB_ReleaseMB(mb);
    if (r != RK_SUCCESS) {
        logf_("VDEC_SendStream 실패 0x%x", r);
        return -1;
    }
    VIDEO_FRAME_INFO_S f;
    memset(&f, 0, sizeof f);
    if ((r = RK_MPI_VDEC_GetFrame(0, &f, 1000)) != RK_SUCCESS) {
        logf_("VDEC_GetFrame 실패 0x%x", r);
        return -1;
    }
    RK_MPI_SYS_MmzFlushCache(f.stVFrame.pMbBlk, RK_TRUE);
    int er = encode(fill_from_decoded, &f.stVFrame);
    RK_MPI_VDEC_ReleaseFrame(0, &f);
    return er;
}

/* ------------------------------------------------------------------- run */

enum { M_IDLE, M_PREVIEW, M_PHOTO };
static int mode = M_IDLE;
static int photo_preview;
static unsigned cw, ch, cfps, cstride_;
static uint32_t cpix;
static char dev[64];

static int dev_auto; /* -d 없이 자동으로 찾은 장치면 다시 찾을 수 있다 */

static const char *mode_name(int m) { return m == M_PREVIEW ? "preview" : m == M_PHOTO ? "photo" : "idle"; }

/* USB 카메라는 전원이 흔들리면 끊겼다 다시 잡히고 노드 번호도 바뀐다 (video0 -> video1).
 * 열어 둔 fd 가 죽었으면 다시 찾아 연다. report 면 실패를 'E' 로 알린다. */
static int cam_ensure(int report) {
    struct v4l2_capability cap;
    if (cam_fd >= 0 && xioctl(cam_fd, VIDIOC_QUERYCAP, &cap) == 0)
        return 0;
    if (cam_fd >= 0)
        close(cam_fd);
    cam_fd = -1;
    streaming = 0;
    nbufs = 0;
    if (dev_auto && find_uvc(dev, sizeof dev)) {
        if (report)
            emitf('E', "UVC 카메라를 찾지 못했습니다 (연결 확인)");
        return -1;
    }
    if (cam_open(dev)) {
        if (report)
            emitf('E', "카메라 %s 를 열지 못했습니다", dev);
        return -1;
    }
    logf_("카메라 다시 열기: %s", dev);
    emitf('C', "{\"seq\":0,\"reopened\":true,\"ok\":true}"); /* 컨트롤 설정이 초기화됐다 */
    return 0;
}

/* ------------------------------------------------------------- 컨트롤 */

static char jb[48 * 1024];
static size_t jl;

static void jadd(const char *fmt, ...) {
    if (jl >= sizeof jb)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(jb + jl, sizeof jb - jl, fmt, ap);
    va_end(ap);
    if (n > 0)
        jl = jl + (size_t)n < sizeof jb ? jl + (size_t)n : sizeof jb;
}

static void jstr(const char *s) {
    jadd("\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            jadd("\\%c", c);
        else if (c < 0x20)
            jadd("\\u%04x", c);
        else
            jadd("%c", c);
    }
    jadd("\"");
}

static const char *ctrl_type(uint32_t t) {
    switch (t) {
    case V4L2_CTRL_TYPE_INTEGER: return "int";
    case V4L2_CTRL_TYPE_BOOLEAN: return "bool";
    case V4L2_CTRL_TYPE_MENU: return "menu";
    case V4L2_CTRL_TYPE_INTEGER_MENU: return "menu";
    default: return NULL;
    }
}

/* 'C' 레코드: 지원하는 컨트롤 전체 (정수/켜고 끄기/메뉴) */
static void emit_ctrls(unsigned seq, int ok, const char *err) {
    jl = 0;
    jadd("{\"seq\":%u,\"ok\":%s,\"error\":", seq, ok ? "true" : "false");
    jstr(err ? err : "");
    jadd(",\"controls\":[");
    int first = 1;
    struct v4l2_queryctrl q;
    memset(&q, 0, sizeof q);
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (cam_fd >= 0 && xioctl(cam_fd, VIDIOC_QUERYCTRL, &q) == 0) {
        const char *t = ctrl_type(q.type);
        if (t && !(q.flags & V4L2_CTRL_FLAG_DISABLED)) {
            struct v4l2_control c = {q.id, q.default_value};
            xioctl(cam_fd, VIDIOC_G_CTRL, &c);
            jadd("%s{\"id\":%u,\"name\":", first ? "" : ",", q.id);
            jstr((const char *)q.name);
            jadd(",\"type\":\"%s\",\"min\":%d,\"max\":%d,\"step\":%d,\"default\":%d,\"value\":%d,"
                 "\"inactive\":%s,\"readonly\":%s",
                 t, q.minimum, q.maximum, q.step, q.default_value, c.value,
                 (q.flags & V4L2_CTRL_FLAG_INACTIVE) ? "true" : "false",
                 (q.flags & (V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_GRABBED)) ? "true" : "false");
            if (q.type == V4L2_CTRL_TYPE_MENU || q.type == V4L2_CTRL_TYPE_INTEGER_MENU) {
                jadd(",\"menu\":[");
                int mf = 1;
                for (int i = q.minimum; i <= q.maximum && i - q.minimum < 64; i++) {
                    struct v4l2_querymenu m;
                    memset(&m, 0, sizeof m);
                    m.id = q.id;
                    m.index = (uint32_t)i;
                    if (xioctl(cam_fd, VIDIOC_QUERYMENU, &m) < 0)
                        continue;
                    jadd("%s{\"v\":%d,\"name\":", mf ? "" : ",", i);
                    if (q.type == V4L2_CTRL_TYPE_MENU) {
                        jstr((const char *)m.name);
                    } else {
                        char s[32];
                        snprintf(s, sizeof s, "%lld", (long long)m.value);
                        jstr(s);
                    }
                    jadd("}");
                    mf = 0;
                }
                jadd("]");
            }
            jadd("}");
            first = 0;
        }
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    jadd("]}");
    if (jl >= sizeof jb) { /* 넘치면 목록 없이 */
        jl = 0;
        jadd("{\"seq\":%u,\"ok\":false,\"error\":\"컨트롤 목록이 너무 깁니다\",\"controls\":[]}", seq);
    }
    emit('C', jb, (uint32_t)jl);
}

static void ctrl_cmd(const char *line) {
    unsigned seq = 0, id = 0;
    int val = 0;
    if (!strncmp(line, "ctrls", 5)) {
        sscanf(line + 5, "%u", &seq);
        if (cam_ensure(0))
            emit_ctrls(seq, 0, "카메라가 연결되어 있지 않습니다");
        else
            emit_ctrls(seq, 1, NULL);
        return;
    }
    if (sscanf(line + 4, "%u %u %d", &seq, &id, &val) != 3) {
        emit_ctrls(seq, 0, "형식: ctrl <seq> <id> <value>");
        return;
    }
    if (cam_ensure(0)) {
        emit_ctrls(seq, 0, "카메라가 연결되어 있지 않습니다");
        return;
    }
    struct v4l2_control c = {id, val};
    if (xioctl(cam_fd, VIDIOC_S_CTRL, &c) < 0) {
        char e[128];
        snprintf(e, sizeof e, "설정 실패 (0x%08x=%d): %s", id, val, strerror(errno));
        logf_("%s", e);
        emit_ctrls(seq, 0, e);
        return;
    }
    emit_ctrls(seq, 1, NULL);
}

static int set_mode(int m, int with_preview) {
    cam_halt();
    mode = M_IDLE;
    photo_preview = 0;
    if (m != M_IDLE && cam_ensure(1))
        return -1;
    if (m == M_PREVIEW) {
        cpix = has_format(V4L2_PIX_FMT_YUYV) ? V4L2_PIX_FMT_YUYV : has_format(V4L2_PIX_FMT_NV12) ? V4L2_PIX_FMT_NV12 : 0;
        if (!cpix) {
            emitf('E', "카메라가 YUYV/NV12 를 지원하지 않습니다");
            return -1;
        }
        cw = pw, ch = ph, cfps = pfps, cstride_ = 0;
        if (cam_config(cpix, &cw, &ch, &cfps, &cstride_) || cw != pw || ch != ph) {
            emitf('E', "카메라가 미리보기 %ux%u 를 지원하지 않습니다 (%ux%u)", pw, ph, cw, ch);
            return -1;
        }
        if (!cstride_)
            cstride_ = cpix == V4L2_PIX_FMT_YUYV ? cw * 2 : cw;
        if (cam_start(4)) {
            emitf('E', "카메라 스트리밍 시작 실패");
            return -1;
        }
    } else if (m == M_PHOTO) {
        cpix = has_format(V4L2_PIX_FMT_MJPEG) ? V4L2_PIX_FMT_MJPEG : has_format(V4L2_PIX_FMT_JPEG) ? V4L2_PIX_FMT_JPEG : 0;
        if (!cpix) {
            emitf('E', "카메라가 MJPEG 를 지원하지 않습니다");
            return -1;
        }
        cw = xw, ch = xh, cfps = 30;
        if (cam_config(cpix, &cw, &ch, &cfps, NULL) || cam_start(3)) {
            emitf('E', "사진 모드 시작 실패");
            return -1;
        }
        if (vdec_ready && (vdec_w != cw || vdec_h != ch))
            vdec_close();
        photo_preview = with_preview;
    }
    mode = m;
    focus_src = m == M_PHOTO ? "photo" : "preview"; /* 사진 모드 미리보기는 축소본이라 값의 크기가 다르다 */
    focus_reset();
    char a[5];
    emitf('I', "{\"mode\":\"%s\",\"device\":\"%s\",\"width\":%u,\"height\":%u,\"fps\":%u,\"camera_format\":\"%s\","
               "\"preview\":%s,\"preview_width\":%u,\"preview_height\":%u,\"kbps\":%u}",
          mode_name(m), dev, m == M_IDLE ? 0 : cw, m == M_IDLE ? 0 : ch, m == M_IDLE ? 0 : cfps,
          m == M_IDLE ? "" : fourcc(cpix, a), (m == M_PREVIEW || photo_preview) ? "true" : "false", pw, ph, kbps);
    logf_("모드 %s %ux%u@%u%s", mode_name(m), cw, ch, cfps, photo_preview ? " (+미리보기)" : "");
    return 0;
}

/* UVC MJPEG 은 EOI 뒤에 0 패딩이 붙어 오기도 한다 */
static uint32_t jpeg_len(const uint8_t *p, uint32_t n) {
    for (uint32_t i = n; i >= 2 && n - i <= 4096; i--)
        if (p[i - 2] == 0xFF && p[i - 1] == 0xD9)
            return i;
    return n;
}

static volatile sig_atomic_t stop_flag;
static void on_sig(int s) {
    (void)s;
    stop_flag = 1;
}

static int run(void) {
    if (cam_open(dev))
        return 1;
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        emitf('E', "RK_MPI_SYS_Init 실패");
        return 1;
    }
    if (venc_open()) {
        emitf('E', "H.264 인코더를 열지 못했습니다");
        RK_MPI_SYS_Exit();
        return 1;
    }
    set_mode(M_IDLE, 0);

    int rc = 0, timeouts = 0, snap_want = 0, snap_drop = 0;
    while (!stop_flag) {
        struct pollfd p[2] = {{0, POLLIN, 0}, {cam_fd, POLLIN, 0}};
        int n = poll(p, streaming ? 2 : 1, 2000);
        if (n < 0 && errno != EINTR)
            break;
        if (p[0].revents) {
            char line[64];
            int k;
            while ((k = read_cmd(line, sizeof line)) == 1) {
                if (!strcmp(line, "preview"))
                    set_mode(M_PREVIEW, 1);
                else if (!strcmp(line, "photo"))
                    set_mode(M_PHOTO, 1);
                else if (!strcmp(line, "photo nopreview"))
                    set_mode(M_PHOTO, 0);
                else if (!strcmp(line, "idle"))
                    set_mode(M_IDLE, 0);
                else if (!strcmp(line, "snap")) {
                    if (mode != M_PHOTO)
                        emitf('E', "snap 은 photo 모드에서만 됩니다");
                    snap_want = 1;
                    snap_drop = (int)skip;
                } else if (!strcmp(line, "idr"))
                    RK_MPI_VENC_RequestIDR(0, RK_TRUE);
                else if (!strcmp(line, "focus on"))
                    focus_on = 1, focus_reset();
                else if (!strcmp(line, "focus off"))
                    focus_on = 0;
                else if (!strncmp(line, "ctrls", 5) || !strncmp(line, "ctrl ", 5))
                    ctrl_cmd(line);
                else if (!strcmp(line, "quit"))
                    stop_flag = 1;
                timeouts = 0;
            }
            if (k < 0)
                stop_flag = 1;
        }
        if (!streaming)
            continue;
        if (n == 0) {
            if (++timeouts >= 3) {
                emitf('E', "카메라 프레임이 6초 동안 없습니다");
                rc = 1;
                break;
            }
            continue;
        }
        if (!(p[1].revents & POLLIN))
            continue;

        /* 밀린 프레임은 다 꺼내고 가장 최근 것만 쓴다 (디코딩이 카메라보다 느리므로) */
        struct v4l2_buffer b, last;
        int have = 0, err = 0;
        for (;;) {
            int d = cam_dequeue(&b);
            if (d > 0)
                break;
            if (d < 0) {
                err = 1;
                break;
            }
            if (mode == M_PHOTO && snap_want && b.bytesused > 0) {
                if (snap_drop > 0) {
                    snap_drop--;
                } else {
                    const uint8_t *jp = bufs[b.index].start;
                    emit('J', jp, jpeg_len(jp, b.bytesused));
                    snap_want = 0;
                }
            }
            if (have)
                cam_release(&last);
            last = b;
            have = 1;
        }
        if (err) {
            emitf('E', "DQBUF 실패: %s", strerror(errno));
            rc = 1;
            break;
        }
        if (!have)
            continue;
        timeouts = 0;
        int er = 0;
        if (mode == M_PREVIEW) {
            struct raw r = {bufs[last.index].start, cstride_, cpix};
            er = encode(fill_from_camera, &r);
        } else if (mode == M_PHOTO && photo_preview && last.bytesused > 0) {
            const uint8_t *jp = bufs[last.index].start;
            if (preview_from_jpeg(jp, jpeg_len(jp, last.bytesused), cw, ch))
                photo_preview = 0, logf_("사진 모드 미리보기 디코딩 실패 - 이번 촬영에서는 끈다");
        }
        cam_release(&last);
        if (er) {
            emitf('E', "인코딩 실패");
            rc = 1;
            break;
        }
    }
    cam_halt();
    close(cam_fd);
    vdec_close();
    venc_close();
    RK_MPI_SYS_Exit();
    return rc;
}

/* ------------------------------------------------------------------ MIPI */

/* MIPI 센서(IMX708 등): UVC 카메라가 없고 rkisp 에 센서가 붙어 있을 때 쓴다.
 *   미리보기 rkisp_selfpath (ISP 가 pw x ph NV12 로 축소) -> 위의 H.264 인코더
 *   사진     rkisp_mainpath (센서 해상도 NV12) -> VENC JPEG 채널 1
 * 노출/화이트밸런스는 rkaiq 가 맡으므로 카메라를 쓰는 동안 rkaiq 를 돌린다 (rkipc 와 동시에 쓸 수 없다).
 * rkaiq 헤더가 SDK 에 없어 쓰는 함수만 선언한다 (보드 /oem/usr/lib/librkaiq.so, AIQ v5.0). */
typedef struct rk_aiq_sys_ctx_s rk_aiq_sys_ctx_t;
extern rk_aiq_sys_ctx_t *rk_aiq_uapi2_sysctl_init(const char *sns_ent_name, const char *iq_file_dir,
                                                  void *err_cb, void *metas_cb);
extern int rk_aiq_uapi2_sysctl_preInit_devBufCnt(const char *sns_ent_name, const char *dev_ent, int buf_cnt);
extern int rk_aiq_uapi2_sysctl_prepare(const rk_aiq_sys_ctx_t *ctx, uint32_t w, uint32_t h, int mode);
extern int rk_aiq_uapi2_sysctl_start(const rk_aiq_sys_ctx_t *ctx);
extern int rk_aiq_uapi2_sysctl_stop(const rk_aiq_sys_ctx_t *ctx, int keep_ext_hw_st);
extern void rk_aiq_uapi2_sysctl_deinit(rk_aiq_sys_ctx_t *ctx);
typedef struct { float rgain, grgain, gbgain, bgain; } rk_aiq_wb_gain_t;
extern int rk_aiq_uapi2_setMWBGain(const rk_aiq_sys_ctx_t *ctx, rk_aiq_wb_gain_t *gain);
/* 수동 WB 속성 (rk_aiq_wb_mwb_attrib_t): sync{모드, done} + mwb 모드 + 게인.
 * setMWBGain 은 부를 때마다 WB 동작 모드도 '수동'으로 다시 넣고 동기(SYNC)로 기다리는데, 스트리밍 중에
 * 이렇게 하면 rkaiq 분석기가 "no free item for sofInfo" 로 막히고 종료 때 멈춘다.
 * 그래서 게인만 비동기(ASYNC)로 넣는다. 헤더가 없어 처음에 Get 으로 읽어 배치가 맞는지 확인한다. */
typedef struct {
    int sync_mode; /* 0 기본, 1 SYNC, 2 ASYNC */
    uint8_t done;
    int mode; /* 1 CCT, 2 WBGAIN, 3 SCENE */
    rk_aiq_wb_gain_t gain;
    uint32_t pad[8];
} rk_aiq_wb_mwb_attrib_t;
extern int rk_aiq_user_api2_awb_GetMwbAttrib(const rk_aiq_sys_ctx_t *ctx, rk_aiq_wb_mwb_attrib_t *attr);
extern int rk_aiq_user_api2_awb_SetMwbAttrib(const rk_aiq_sys_ctx_t *ctx, rk_aiq_wb_mwb_attrib_t attr);

#define IQ_DIR "/oem/usr/share/iqfiles"
/* 고정 화이트밸런스 "R게인 B게인" (예: 2.36 1.52). 있으면 이것을 쓰고 AWB 를 하지 않는다. */
#define MWB_FILE IQ_DIR "/mwb_gain.txt"
/* AWB 색온도 곡선 (한 줄에 "색온도 R/G B/G", RPi libcamera imx708 튜닝에서 가져옴).
 * 있으면 camhelper 가 이 곡선 위에서 화이트밸런스를 맞춘다 (rkaiq 의 AWB 는 수동 모드로 둔다). */
#define CT_CURVE_FILE IQ_DIR "/awb_ct_curve.txt"

static char sns_name[64];       /* "m00_b_imx708 4-0030" */
static unsigned sns_w, sns_h;   /* 센서 출력 = 사진 크기 */
static rk_aiq_sys_ctx_t *aiq;

/* v4l-subdev 이름이 mNN_ 로 시작하면 rkisp 에 붙은 센서다 */
static int find_mipi_sensor(void) {
    for (int i = 0; i < 16; i++) {
        char p[64], s[64] = "";
        snprintf(p, sizeof p, "/sys/class/video4linux/v4l-subdev%d/name", i);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        if (fgets(s, sizeof s, f))
            s[strcspn(s, "\n")] = 0;
        fclose(f);
        if (s[0] == 'm' && s[1] >= '0' && s[1] <= '9' && s[3] == '_') {
            snprintf(sns_name, sizeof sns_name, "%s", s);
            /* 센서가 지금 내는 크기 */
            snprintf(p, sizeof p, "/dev/v4l-subdev%d", i);
            int fd = open(p, O_RDWR);
            struct v4l2_subdev_format sf;
            memset(&sf, 0, sizeof sf);
            sf.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            if (fd >= 0 && xioctl(fd, VIDIOC_SUBDEV_G_FMT, &sf) == 0)
                sns_w = sf.format.width, sns_h = sf.format.height;
            if (fd >= 0)
                close(fd);
            return 0;
        }
    }
    return -1;
}

/* /sys/class/video4linux/videoN/name 으로 노드를 찾는다 */
static int find_video_by_name(const char *name, char *out, size_t n) {
    for (int i = 0; i < 64; i++) {
        char p[64], s[64] = "";
        snprintf(p, sizeof p, "/sys/class/video4linux/video%d/name", i);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        if (fgets(s, sizeof s, f))
            s[strcspn(s, "\n")] = 0;
        fclose(f);
        if (!strcmp(s, name)) {
            snprintf(out, n, "/dev/video%d", i);
            return 0;
        }
    }
    return -1;
}

/* rkisp 출력 노드 (multi-planar, NV12 한 평면) */
struct mcam {
    const char *name;
    char path[32];
    int fd;
    unsigned w, h, stride, n;
    struct buf b[MAX_BUFS];
    int on;
};
static struct mcam sp = {"rkisp_selfpath", "", -1, 0, 0, 0, 0, {{0}}, 0};
static struct mcam mp = {"rkisp_mainpath", "", -1, 0, 0, 0, 0, {{0}}, 0};

static int mcam_start(struct mcam *c, unsigned w, unsigned h, unsigned count) {
    if (c->fd < 0) {
        if (find_video_by_name(c->name, c->path, sizeof c->path)) {
            logf_("%s 노드 없음", c->name);
            return -1;
        }
        c->fd = open(c->path, O_RDWR | O_NONBLOCK);
        if (c->fd < 0) {
            logf_("%s 열기 실패: %s", c->path, strerror(errno));
            return -1;
        }
    }
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(c->fd, VIDIOC_S_FMT, &f) < 0) {
        logf_("%s S_FMT 실패: %s", c->name, strerror(errno));
        return -1;
    }
    c->w = f.fmt.pix_mp.width;
    c->h = f.fmt.pix_mp.height;
    c->stride = f.fmt.pix_mp.plane_fmt[0].bytesperline ? f.fmt.pix_mp.plane_fmt[0].bytesperline : c->w;

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = count;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        logf_("%s REQBUFS 실패: %s", c->name, strerror(errno));
        return -1;
    }
    c->n = rb.count > MAX_BUFS ? MAX_BUFS : rb.count;
    for (unsigned i = 0; i < c->n; i++) {
        struct v4l2_plane pl[VIDEO_MAX_PLANES];
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.m.planes = pl;
        b.length = VIDEO_MAX_PLANES;
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0)
            return -1;
        c->b[i].len = pl[0].length;
        c->b[i].start = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, pl[0].m.mem_offset);
        if (c->b[i].start == MAP_FAILED)
            return -1;
        if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0)
            return -1;
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(c->fd, VIDIOC_STREAMON, &t) < 0) {
        logf_("%s STREAMON 실패: %s", c->name, strerror(errno));
        return -1;
    }
    c->on = 1;
    logf_("%s %ux%u (stride %u) 시작", c->name, c->w, c->h, c->stride);
    return 0;
}

static void mcam_stop(struct mcam *c) {
    if (c->fd < 0)
        return;
    if (c->on) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(c->fd, VIDIOC_STREAMOFF, &t);
        c->on = 0;
    }
    for (unsigned i = 0; i < c->n; i++)
        munmap(c->b[i].start, c->b[i].len);
    c->n = 0;
    /* 노드를 닫아야 ISP 가 이 출력을 완전히 놓는다 */
    close(c->fd);
    c->fd = -1;
}

/* 가장 최근 프레임 하나만 남기고 꺼낸다. 0 있음, 1 없음, -1 오류 */
static int mcam_latest(struct mcam *c, struct v4l2_buffer *last, struct v4l2_plane *lpl, unsigned *cnt) {
    int have = 0;
    *cnt = 0;
    for (;;) {
        struct v4l2_plane pl[VIDEO_MAX_PLANES];
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl;
        b.length = VIDEO_MAX_PLANES;
        if (xioctl(c->fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN)
                break;
            return -1;
        }
        if (have)
            xioctl(c->fd, VIDIOC_QBUF, last);
        *last = b;
        memcpy(lpl, pl, sizeof pl);
        last->m.planes = lpl;
        have = 1;
        (*cnt)++;
    }
    return have ? 0 : 1;
}

static void mcam_release(struct mcam *c, struct v4l2_buffer *b) { xioctl(c->fd, VIDIOC_QBUF, b); }

static void awb_init(void);

static int aiq_start(void) {
    if (aiq)
        return 0;
    rk_aiq_uapi2_sysctl_preInit_devBufCnt(sns_name, "rkraw_rx", 2);
    aiq = rk_aiq_uapi2_sysctl_init(sns_name, IQ_DIR, NULL, NULL);
    if (!aiq) {
        logf_("rkaiq init 실패 (%s, %s)", sns_name, IQ_DIR);
        return -1;
    }
    if (rk_aiq_uapi2_sysctl_prepare(aiq, sns_w, sns_h, 0 /* NORMAL */) ||
        rk_aiq_uapi2_sysctl_start(aiq)) {
        logf_("rkaiq prepare/start 실패");
        rk_aiq_uapi2_sysctl_deinit(aiq);
        aiq = NULL;
        return -1;
    }
    logf_("rkaiq 시작: %s %ux%u", sns_name, sns_w, sns_h);
    awb_init();
    return 0;
}

static void aiq_stop(void) {
    if (!aiq)
        return;
    rk_aiq_uapi2_sysctl_stop(aiq, 0);
    rk_aiq_uapi2_sysctl_deinit(aiq);
    aiq = NULL;
}

/* --------------------------------------------- 렌즈 VCM + 콘트라스트 AF */

/* Camera Module 3 의 렌즈 VCM (DW9807 계열, 센서와 같은 I2C 버스의 0x0c).
 *   0x02 제어 (0 = 동작, 1 = 절전), 0x03/0x04 위치 상위 2비트/하위 8비트 (0~1023, 클수록 가까운 곳)
 * 센서 전원과 함께 꺼지므로 스트림을 켤 때마다 다시 쓴다. 카메라 단자 I2C 가 불안정해서 여러 번 시도한다. */
#define VCM_ADDR 0x0c
#define VCM_MAX 1023
#define CID_FOCUS_ABS 0x009a090a  /* V4L2_CID_FOCUS_ABSOLUTE: 앱에서 "초점 위치" */
#define CID_FOCUS_AUTO 0x009a090c /* V4L2_CID_FOCUS_AUTO: 앱에서 "자동 초점" */

static int vcm_fd = -1;
static int af_auto = 1, af_pos = 400;
/* 자동 초점 방식 (-M): continuous = 미리보기 중에 맞춰 두고 초점이 흐려지면 다시 맞춘다. 촬영은 AF 없이 바로.
 *                     snap = 촬영할 때마다 맞춘다 (이전 방식). 자동 초점을 끄면 고정 초점(af_pos)이다 */
static int af_cont = 1;
/* 고정 초점 위치를 저장하는 파일: 자동 초점을 끄거나 위치를 바꾸면 쓰고, 시작할 때 읽는다 */
#define AF_POS_FILE "/userdata/scanner/af_fixed_pos"

static void af_pos_save(void) {
    FILE *f = fopen(AF_POS_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", af_pos);
        fclose(f);
    }
}

static void af_pos_load(void) {
    FILE *f = fopen(AF_POS_FILE, "r");
    int v;
    if (f && fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 1023)
        af_pos = v;
    if (f)
        fclose(f);
}

static int vcm_write(uint8_t reg, const uint8_t *val, int n) {
    uint8_t b[4] = {reg};
    memcpy(b + 1, val, (size_t)n);
    for (int t = 0; t < 20; t++) {
        if (write(vcm_fd, b, (size_t)n + 1) == n + 1)
            return 0;
        usleep(1000);
    }
    return -1;
}

/* 렌즈를 한 번에 크게 옮기면 끝에 부딪히거나 출렁이며 "딸깍" 소리가 난다.
 * VCM_RAMP 보다 크게 움직일 때는 VCM_RAMP 씩 VCM_RAMP_US 간격으로 나눠 옮긴다 (0 -> 1023 이 약 80ms) */
#define VCM_RAMP 40
#define VCM_RAMP_US 3000
#define VCM_LOW 32 /* AF 탐색과 대기 때 내려가는 가장 낮은 위치 (0 은 렌즈가 바닥에 닿는다) */
static int vcm_last = -1; /* 마지막으로 쓴 위치 (-1: 모름) */

static int vcm_write_pos(int pos) {
    uint8_t v[2] = {(uint8_t)(pos >> 8), (uint8_t)pos};
    if (vcm_write(0x03, v, 2)) {
        logf_("VCM 위치 %d 쓰기 실패", pos);
        vcm_last = -1;
        return -1;
    }
    vcm_last = pos;
    return 0;
}

static int vcm_set(int pos) {
    if (vcm_fd < 0)
        return -1;
    pos = pos < 0 ? 0 : pos > VCM_MAX ? VCM_MAX : pos;
    while (vcm_last >= 0 && abs(pos - vcm_last) > VCM_RAMP) {
        if (vcm_write_pos(vcm_last + (pos > vcm_last ? VCM_RAMP : -VCM_RAMP)))
            break;
        usleep(VCM_RAMP_US);
    }
    return vcm_write_pos(pos);
}

/* 센서 i2c 클라이언트 이름(예 "4-0030")에서 버스 번호를 얻어 VCM 을 연다 */
static void vcm_open(void) {
    if (vcm_fd >= 0)
        return;
    const char *sp_ = strchr(sns_name, ' ');
    int bus = sp_ ? atoi(sp_ + 1) : -1;
    char p[32];
    snprintf(p, sizeof p, "/dev/i2c-%d", bus);
    if (bus < 0 || (vcm_fd = open(p, O_RDWR)) < 0) {
        logf_("VCM: %s 열기 실패", p);
        return;
    }
    if (ioctl(vcm_fd, I2C_SLAVE_FORCE, VCM_ADDR) < 0) {
        logf_("VCM: 주소 0x%02x 설정 실패", VCM_ADDR);
        close(vcm_fd);
        vcm_fd = -1;
    }
}

/* 스트림을 켠 뒤 (센서와 함께 VCM 전원이 들어온 상태) 동작 모드로 두고 마지막 위치로 */
static void vcm_on(void) {
    vcm_open();
    uint8_t on = 0;
    vcm_last = 0; /* 막 켜진 렌즈는 바닥(0)에 있다 -> af_pos 까지 천천히 올린다 */
    if (vcm_fd >= 0 && (vcm_write(0x02, &on, 1) || vcm_set(af_pos)))
        logf_("VCM 초기화 실패");
}

/* 초점 평가값: 가운데 1/2 x 1/2 영역의 라플라시안 분산 (2화소 간격으로 표본).
 * 라플라시안은 af_clip 에서 자른다: 두피 위 모발은 진하고 반짝여서 경계 하나가 수백이 되고, 제곱하면
 * 몇 가닥이 점수를 다 차지해 AF 가 모발에 맞는다. 잘라 두면 화면 넓게 깔린 두피의 잔결이 점수에 반영된다.
 * 0 이면 자르지 않는다 (환경변수 CAMHELPER_AF_CLIP 로 바꿔 시험). 20 은 너무 낮아 초점이 안 맞은 화면의 잡음도
 * 거의 같은 점수가 되어 곡선이 평평해졌다. raw 에는 자르지 않은 값을 돌려준다 (로그로 비교) */
static int af_clip = 48;

static double sharpness(const uint8_t *y, unsigned w, unsigned h, unsigned stride, double *raw) {
    unsigned x0 = w / 4, y0 = h / 4, x1 = w - w / 4, y1 = h - h / 4;
    int64_t s = 0, rs = 0;
    uint64_t s2 = 0, rs2 = 0, n = 0;
    for (unsigned r = y0; r < y1; r += 2) {
        const uint8_t *p = y + (size_t)r * stride, *u = p - stride, *d = p + stride;
        for (unsigned c = x0; c < x1; c += 2) {
            int l = 4 * p[c] - p[c - 1] - p[c + 1] - u[c] - d[c];
            rs += l;
            rs2 += (uint64_t)(l * l);
            if (af_clip > 0)
                l = l > af_clip ? af_clip : l < -af_clip ? -af_clip : l;
            s += l;
            s2 += (uint64_t)(l * l);
            n++;
        }
    }
    double m = (double)s / (double)n, rm = (double)rs / (double)n;
    if (raw)
        *raw = (double)rs2 / (double)n - rm * rm;
    return (double)s2 / (double)n - m * m;
}

/* AF 상태: 넓게 훑다가 최고점을 지나 크게 떨어지면 멈추고, 최고점 주변을 16 간격으로 다시 훑는다.
 * 렌즈를 옮긴 뒤 af_settle 프레임은 버리고 다음 프레임을 잰다 (VCM 안정 + ISP 버퍼 지연). */
enum { AF_IDLE, AF_COARSE, AF_FINE, AF_FINAL };
static int af_state, af_cur, af_best, af_wait, af_hi, af_step, af_frames;
static double af_bestv;
static int af_settling, af_settle_n; /* 아래로 움직인 뒤 선명도가 멈출 때까지 기다리는 중 */
static double af_prevv;
static uint64_t af_t0;

static int af_settle = 2; /* 렌즈를 옮긴 뒤 버릴 프레임 수. 환경변수 CAMHELPER_AF_SETTLE 로 바꿔 시험한다 */
/* 연속 AF: 맞춘 직후의 선명도(caf_ref)와 비교해 흐려지거나(60% 아래) 확 선명해지면(2배 위, 처음엔 대상이
 * 없다가 두피에 댄 경우) 그 상태가 약 1초 이어지고 화면이 안정됐을 때(최근 5번 값이 30% 안) 다시 맞춘다 */
static double caf_ref = -1, caf_hist[5];
static unsigned caf_n, caf_off;
static uint64_t caf_last;

/* ------------------------------------------------- 화이트밸런스 (AWB) */

/* RPi 색온도 곡선을 기준으로 한 회색 세계 AWB.
 * 미리보기 프레임(ISP 출력 NV12)에서 채도가 낮은(회색에 가까운) 화소만 모아 평균 색 치우침(U, V)을 보고
 * 푸르면(U+) B 게인을, 붉으면(V+) R 게인을 줄인다(반대도 마찬가지). R 게인은 곡선 범위(2500~10000K) 안에서
 * 움직이고 거기서 색온도를 읽는다. B 게인은 그 색온도의 곡선 값에서 +-15% 까지만 벗어날 수 있다:
 * 백색 LED 처럼 곡선에서 조금 벗어난 조명은 따라가고, 한 가지 색이 화면을 채워도 멀리 가지 않는다. */
#define CT_MAX_PTS 32
#define AWB_B_BAND 0.15f
static float ct_tab[CT_MAX_PTS], r_tab[CT_MAX_PTS], b_tab[CT_MAX_PTS];
static int ct_n;
static int awb_on;                 /* 0: 고정(파일) 또는 IQ 그대로 */
static int awb_async_ok;           /* 수동 WB 속성 배치 확인됨 */
static float awb_ct = 5000;        /* 현재 색온도 (모드가 바뀌어도 유지) */
static float awb_r, awb_b;         /* 현재 R, B 게인 (0 이면 awb_ct 의 곡선 값으로 시작) */
static float awb_applied_r, awb_applied_b;
static unsigned awb_frame_n;
static uint64_t awb_last_emit;
static int awb_emitted_ct;

static float ct_interp(const float *ys, float ct) {
    if (ct <= ct_tab[0])
        return ys[0];
    for (int i = 1; i < ct_n; i++)
        if (ct <= ct_tab[i])
            return ys[i - 1] + (ct - ct_tab[i - 1]) / (ct_tab[i] - ct_tab[i - 1]) * (ys[i] - ys[i - 1]);
    return ys[ct_n - 1];
}

/* R 게인(=1/r)은 색온도가 오를수록 커진다. 그 역함수 */
static float ct_from_rgain(float gr) {
    float r = 1 / gr;
    if (r >= r_tab[0])
        return ct_tab[0];
    for (int i = 1; i < ct_n; i++)
        if (r >= r_tab[i])
            return ct_tab[i - 1] + (r_tab[i - 1] - r) / (r_tab[i - 1] - r_tab[i]) * (ct_tab[i] - ct_tab[i - 1]);
    return ct_tab[ct_n - 1];
}

static void awb_apply(void) {
    rk_aiq_wb_gain_t g = {awb_r, 1, 1, awb_b};
    /* 1% 이상 바뀔 때만 넣는다 (rkaiq 에 너무 자주 넣으면 내부 큐가 막힌다) */
    if (fabsf(g.rgain - awb_applied_r) < awb_applied_r * 0.01f && fabsf(g.bgain - awb_applied_b) < awb_applied_b * 0.01f)
        return;
    if (getenv("CAMHELPER_AWB_DRY") || !aiq || !awb_async_ok) {
        awb_applied_r = g.rgain, awb_applied_b = g.bgain;
        return;
    }
    rk_aiq_wb_mwb_attrib_t a;
    memset(&a, 0, sizeof a);
    a.sync_mode = 2; /* ASYNC */
    a.mode = 2;      /* WBGAIN */
    a.gain = g;
    if (rk_aiq_user_api2_awb_SetMwbAttrib(aiq, a) == 0)
        awb_applied_r = g.rgain, awb_applied_b = g.bgain;
}

/* rkaiq 를 새로 시작할 때마다 부른다 */
static void awb_init(void) {
    awb_on = 0;
    awb_applied_r = awb_applied_b = 0;
    FILE *f = fopen(MWB_FILE, "r");
    if (f) {
        rk_aiq_wb_gain_t g = {1, 1, 1, 1};
        if (fscanf(f, "%f %f", &g.rgain, &g.bgain) == 2 && g.rgain > 0 && g.bgain > 0) {
            int r = rk_aiq_uapi2_setMWBGain(aiq, &g);
            logf_("고정 화이트밸런스 R %.2f B %.2f (%s) %s", g.rgain, g.bgain, MWB_FILE, r ? "실패" : "");
        }
        fclose(f);
        return;
    }
    if (!ct_n && (f = fopen(CT_CURVE_FILE, "r"))) {
        char line[128];
        while (ct_n < CT_MAX_PTS && fgets(line, sizeof line, f))
            if (line[0] != '#' && sscanf(line, "%f %f %f", &ct_tab[ct_n], &r_tab[ct_n], &b_tab[ct_n]) == 3 &&
                r_tab[ct_n] > 0 && b_tab[ct_n] > 0)
                ct_n++;
        fclose(f);
    }
    if (ct_n < 2) {
        logf_("AWB 색온도 곡선(%s)이 없어 IQ 파일의 화이트밸런스를 그대로 씁니다", CT_CURVE_FILE);
        return;
    }
    /* 수동 WB 속성 배치 확인: IQ 의 수동 게인(mode 2 = WBGAIN)이 그대로 읽혀야 한다 */
    {
        rk_aiq_wb_mwb_attrib_t a;
        memset(&a, 0, sizeof a);
        int r = rk_aiq_user_api2_awb_GetMwbAttrib(aiq, &a);
        awb_async_ok = r == 0 && a.mode >= 1 && a.mode <= 3 && a.gain.rgain > 0.5f && a.gain.rgain < 8 &&
                       a.gain.bgain > 0.5f && a.gain.bgain < 8;
        logf_("AWB: 수동 WB 속성 읽기 r=%d sync %d mode %d R %.2f Gr %.2f Gb %.2f B %.2f -> %s", r, a.sync_mode, a.mode,
              a.gain.rgain, a.gain.grgain, a.gain.gbgain, a.gain.bgain, awb_async_ok ? "비동기 설정 사용" : "게인 적용 안 함");
    }
    if (awb_r <= 0) /* 처음: 곡선 위의 시작 색온도 */
        awb_r = 1 / ct_interp(r_tab, awb_ct), awb_b = 1 / ct_interp(b_tab, awb_ct);
    awb_on = 1;
    awb_frame_n = 0;
    awb_apply();
    logf_("AWB: 색온도 곡선 %d점 (%.0f~%.0fK), 시작 %.0fK (R %.2f B %.2f)", ct_n, ct_tab[0], ct_tab[ct_n - 1],
          awb_ct, awb_r, awb_b);
}

static void emit_af_now(void);

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ISP 출력 NV12 한 장으로 게인을 한 걸음 고친다. 15프레임(0.5초)마다: 게인 반영에 몇 프레임 걸리고,
 * setMWBGain 을 자주 부르면 rkaiq 가 "no free item for sofInfo" 로 막혀 종료도 못 한다 */
static void awb_frame(const uint8_t *y, unsigned w, unsigned h, unsigned stride) {
    if (!awb_on || ++awb_frame_n % 15)
        return;
    const uint8_t *uv = y + (size_t)stride * h;
    unsigned step = w >= 2000 ? 32 : 16;
    /* 1차: 밝은 화소의 평균 색 (어두운 곳 - 렌즈 가장자리 등 - 은 색이 틀어져 있어 뺀다)
     * 2차: 그 평균 색에서 가까운 화소만 회색으로 보고 다시 평균. 색 치우침이 커도 회색을 찾고,
     *      화면 일부의 진한 색 물체는 빠진다 */
    int64_t su = 0, sv = 0;
    unsigned n = 0, total = 0;
    for (int pass = 0; pass < 2; pass++) {
        int u0 = n ? (int)(su / n) : 0, v0 = n ? (int)(sv / n) : 0;
        if (pass && !n)
            return;
        su = sv = 0, n = total = 0;
        for (unsigned r = step / 2; r < h; r += step) {
            const uint8_t *yr = y + (size_t)r * stride, *cr = uv + (size_t)(r / 2) * stride;
            for (unsigned c = step / 2; c < w; c += step) {
                int yy = yr[c], u = cr[c & ~1u] - 128, v = cr[(c & ~1u) + 1] - 128;
                total++;
                if (yy < 80 || yy > 235)
                    continue;
                if (pass && abs(u - u0) + abs(v - v0) > 24)
                    continue;
                su += u, sv += v, n++;
            }
        }
    }
    if (n < total / 50 || n < 50) /* 회색으로 볼 화소가 너무 적으면 그대로 둔다 */
        return;
    float du = (float)su / n, dv = (float)sv / n;
    if (fabsf(du) < 0.5f && fabsf(dv) < 0.5f)
        return;
    /* 로그 눈금 비례 제어, 한 번에 최대 8% */
    awb_r *= expf(clampf(-dv * 0.01f, -0.08f, 0.08f));
    awb_r = clampf(awb_r, 1 / r_tab[0], 1 / r_tab[ct_n - 1]);
    awb_ct = ct_from_rgain(awb_r);
    float bc = 1 / ct_interp(b_tab, awb_ct);
    awb_b = clampf(awb_b * expf(clampf(-du * 0.01f, -0.08f, 0.08f)), bc * (1 - AWB_B_BAND), bc * (1 + AWB_B_BAND));
    awb_apply();
    if (getenv("CAMHELPER_AWB_LOG"))
        logf_("AWB 회색 %u/%u 치우침 U %+.1f V %+.1f -> %.0fK R %.2f B %.2f (곡선 B %.2f)", n, total, du, dv,
              awb_ct, awb_r, awb_b, bc);
    /* 앱 표시: 100K 이상 바뀌었고 0.5초 지났을 때 */
    uint64_t t = now_us();
    if (abs((int)awb_ct - awb_emitted_ct) >= 100 && t - awb_last_emit > 500000)
        emit_af_now();
}

/* 'A' 레코드: 앱 화면에 보여 줄 초점/화이트밸런스 상태
 * {"auto":0|1,"state":"searching|locked|manual","pos":N,"ct":색온도(AWB 일 때만)} */
static void emit_af(const char *state, int pos) {
    char ct[32] = "";
    if (awb_on) {
        snprintf(ct, sizeof ct, ",\"ct\":%d", (int)awb_ct);
        awb_emitted_ct = (int)awb_ct;
        awb_last_emit = now_us();
    }
    if (vcm_fd >= 0 || awb_on)
        emitf('A', "{\"auto\":%d,\"state\":\"%s\",\"pos\":%d%s}", af_auto, vcm_fd >= 0 ? state : "none", pos, ct);
}

static void af_move(int pos) {
    /* 렌즈를 0 쪽(먼 곳)으로 내릴 때는 스프링 힘으로만 돌아와서 0.8~1.2초 걸린다 (올릴 때는 0.2초 안).
     * 그래서 아래로 움직이면 정해진 프레임 수 대신 선명도가 더 변하지 않을 때까지(최대 약 1초) 기다린다.
     * 탐색은 언제나 위로 올라가며 하므로 이렇게 기다리는 것은 AF 한 번에 2~3번이다 */
    int down = af_cur - pos;
    af_settling = down > 16;
    af_settle_n = 0;
    af_prevv = -1;
    af_cur = pos;
    vcm_set(pos);
    /* 내릴 때는 렌즈가 움직이기 시작하기 전 몇 프레임 동안 선명도가 그대로라 '멈춤'으로 잘못 볼 수 있다
     * (두피 시험: 0 으로 내린 직후 지난 위치의 값이 읽혀 먼 쪽 우선이 틀린 봉우리를 골랐다).
     * 그래서 내린 거리에 비례해 최소한 기다린 뒤 멈춤을 본다: 1023 -> 0 이면 약 1초, 100 이면 약 0.3초 */
    af_wait = af_settle + (down > 16 ? (down / 34 < 30 ? down / 34 : 30) + 6 : 0);
    emit_af("searching", pos);
}

/* AF 탐색 범위 (-A 최소-최대, 기본 전체). 넓게 훑는 간격은 범위의 약 1/8 (32~96),
 * 좁게는 그 최고점 +-간격/2 를 16 간격으로. 좁힌 범위의 끝에서 최고점이 나오면(초점이 범위 밖일 수 있다)
 * 전체 범위를 한 번 더 넓게 훑는다. */
static int af_min = VCM_LOW, af_max = VCM_MAX, af_cstep = 96;
static int af_rmin, af_rmax;      /* 이번 탐색의 범위 (넓게 다시 훑으면 전체) */
static int af_retry;              /* 전체 범위로 다시 훑을 수 있는가 */
static char af_trace[640];        /* 위치:선명도 기록 (로그용) */
/* 먼 쪽 우선(-P far): 두피 위에 뜬 모발은 대비가 강해서 '가장 선명한 곳'은 모발에 맞기 쉽다.
 * 모발은 언제나 두피보다 렌즈 가까이(위치 값이 큼) 있으므로, 넓게 훑은 곡선에서 최댓값의 50% 이상인
 * 봉우리 중 가장 먼 쪽(위치 값이 가장 작은 것)을 고른다. 이때는 곡선 전체를 보도록 중간에 멈추지 않는다 */
static int af_far;
static int af_cp[32];
static double af_cv[32];
static int af_cn;
static size_t af_tracel;

static void af_set_range(int lo, int hi) {
    lo = lo < VCM_LOW ? VCM_LOW : lo > VCM_MAX ? VCM_MAX : lo; /* 바닥(0)까지는 내리지 않는다 */
    hi = hi < lo ? lo : hi > VCM_MAX ? VCM_MAX : hi;
    af_min = lo, af_max = hi;
    int s = ((hi - lo) / 8 + 15) / 16 * 16;
    af_cstep = s < 32 ? 32 : s > 96 ? 96 : s;
}

static void af_coarse(int lo, int hi, int step) {
    af_state = AF_COARSE;
    af_rmin = lo, af_rmax = hi;
    af_step = step;
    af_cn = 0;
    af_hi = hi;
    af_move(lo);
}

/* delay: 시작 전에 버릴 프레임 수 (미리보기 직후에는 노출이 자리 잡을 때까지) */
static void af_start(int delay) {
    if (vcm_fd < 0)
        return;
    af_bestv = -1;
    af_best = af_pos;
    af_frames = 0;
    af_tracel = 0;
    af_trace[0] = 0;
    af_retry = af_min > VCM_LOW || af_max < VCM_MAX;
    af_t0 = now_us();
    af_coarse(af_min, af_max, af_cstep);
    af_wait += delay;
}

/* 연속 AF 로 다시 맞출 때: 거리는 대개 조금만 변하므로 지금 위치 +-160 만 48 간격으로 훑는다 (약 1초).
 * 그 범위의 끝에서 가장 선명하면(많이 변함) af_retry 로 전체 범위를 한 번 더 훑는다 */
#define CAF_SPAN 160

static void af_start_local(void) {
    if (vcm_fd < 0)
        return;
    int lo = af_pos - CAF_SPAN < af_min ? af_min : af_pos - CAF_SPAN;
    int hi = af_pos + CAF_SPAN > af_max ? af_max : af_pos + CAF_SPAN;
    af_bestv = -1;
    af_best = af_pos;
    af_frames = 0;
    af_tracel = 0;
    af_trace[0] = 0;
    af_retry = 1;
    af_t0 = now_us();
    af_coarse(lo, hi, 48);
}

static int af_busy(void) { return af_state != AF_IDLE; }

static void emit_af_now(void) {
    emit_af(af_busy() ? "searching" : af_auto ? "locked" : "manual", af_busy() ? af_cur : af_pos);
}

static void af_start(int delay);
static void af_start_local(void);

/* 미리보기 프레임마다 부른다 (연속 AF). 3프레임마다 잰다 */
static void caf_frame(const uint8_t *y, unsigned w, unsigned h, unsigned stride) {
    if (!af_cont || !af_auto || af_busy() || caf_ref < 0 || vcm_fd < 0 || ++caf_n % 3)
        return;
    double s = sharpness(y, w, h, stride, NULL);
    caf_hist[(caf_n / 3) % 5] = s;
    int off = s < caf_ref * 0.6 || s > caf_ref * 2 + 5;
    caf_off = off ? caf_off + 1 : 0;
    if (!off && s > caf_ref) /* 더 선명해진 것은 기준으로 삼는다 */
        caf_ref = s;
    if (caf_off < 10 || now_us() - caf_last < 1500000)
        return;
    double mn = 1e30, mx = 0;
    for (int i = 0; i < 5; i++)
        mn = caf_hist[i] < mn ? caf_hist[i] : mn, mx = caf_hist[i] > mx ? caf_hist[i] : mx;
    if (mx > mn * 1.3 + 2) /* 아직 움직이는 중 */
        return;
    logf_("연속 AF: 선명도 %.0f (맞춘 직후 %.0f) -> 다시 맞춤", s, caf_ref);
    af_start_local();
}

/* AF 를 쓰는 스트림의 프레임마다 부른다. 끝나면 1 */
static int af_frame(const uint8_t *y, unsigned w, unsigned h, unsigned stride) {
    if (af_state == AF_IDLE)
        return 0;
    if (af_wait > 0) {
        af_wait--;
        return 0;
    }
    double raw;
    double v = sharpness(y, w, h, stride, &raw);
    if (af_settling) {
        /* 이웃한 두 프레임의 선명도 차이가 3% 안이면 렌즈가 멈춘 것으로 본다 (최대 30프레임) */
        int still = af_prevv >= 0 && fabs(v - af_prevv) <= 0.03 * (v > af_prevv ? v : af_prevv);
        if (!still && ++af_settle_n < 30) {
            af_prevv = v;
            return 0;
        }
        af_settling = 0;
    }
    if (af_state == AF_FINAL) { /* 최종 위치에 자리 잡음 */
        af_state = AF_IDLE;
        logf_("AF 완료: 위치 %d (선명도 %.0f -> 자리 잡은 뒤 %.0f, %u장, %.2f초)", af_pos, af_bestv, v, af_frames,
              (double)(now_us() - af_t0) / 1e6);
        logf_("AF 기록(위치:선명도): %s", af_trace);
        emit_af("locked", af_pos);
        caf_ref = v, caf_off = 0, caf_n = 0, caf_last = now_us();
        return 1;
    }
    af_frames++;
    if (af_tracel < sizeof af_trace - 16)
        af_tracel += (size_t)snprintf(af_trace + af_tracel, sizeof af_trace - af_tracel, "%s%d:%.0f/%.0f",
                                      af_tracel ? " " : "", af_cur, v, raw);
    if (v > af_bestv)
        af_bestv = v, af_best = af_cur;
    if (af_state == AF_COARSE && af_cn < 32)
        af_cp[af_cn] = af_cur, af_cv[af_cn++] = v;
    int next = af_cur + af_step;
    /* 넓게 훑을 때: 최고점에서 3칸 이상 지나 30% 아래로 떨어지면 더 볼 필요가 없다 (먼 쪽 우선이면 끝까지 본다) */
    int past_peak = !af_far && af_state == AF_COARSE && af_cur - af_best >= 3 * af_step && v < af_bestv * 0.3;
    if (next <= af_hi && !past_peak) {
        af_move(next);
        return 0;
    }
    if (af_cur < af_hi && !past_peak && af_cur != af_hi) { /* 범위 끝점까지 */
        af_move(af_hi);
        return 0;
    }
    if (af_state == AF_COARSE) {
        if (af_far && af_cn >= 2) {
            /* 봉우리: 양옆보다 높고 적어도 한쪽보다 5% 넘게 높다 (평평한 구간은 봉우리가 아니다).
             * 바닥(초점이 안 맞은 수준)에서 최고점까지의 50% 이상 올라온 봉우리만 본다.
             * 곡선이 바닥 대비 30% 도, 10 도 안 오르면(대상이 없거나 잡음뿐) 가장 선명한 곳을 그대로 쓴다 */
            double mx = 0, mn = 1e30;
            for (int i = 0; i < af_cn; i++) {
                mx = af_cv[i] > mx ? af_cv[i] : mx;
                mn = af_cv[i] < mn ? af_cv[i] : mn;
            }
            for (int i = 0; mx - mn > mn * 0.3 && mx - mn > 10 && i < af_cn; i++) {
                double v0 = i > 0 ? af_cv[i - 1] : -1, v2 = i < af_cn - 1 ? af_cv[i + 1] : -1, vi = af_cv[i];
                int peak = vi >= v0 && vi >= v2 && (vi > v0 * 1.05 || vi > v2 * 1.05) &&
                           (i > 0 || vi > v2 * 1.1) && (i < af_cn - 1 || vi > v0 * 1.1); /* 범위 양 끝은 이웃보다 10% 넘게 */
                if (peak && vi - mn >= (mx - mn) * 0.5) {
                    if (af_cp[i] != af_best)
                        logf_("AF: 먼 쪽 우선 -> %d (선명도 %.0f, 가장 선명한 곳은 %d:%.0f)", af_cp[i], af_cv[i],
                              af_best, af_bestv);
                    if (i == 0 && af_cp[0] <= VCM_LOW)
                        logf_("AF: 가장 먼 위치(%d)가 봉우리 - 두피가 초점 범위보다 멀 수 있다", af_cp[0]);
                    af_best = af_cp[i], af_bestv = af_cv[i];
                    break;
                }
            }
        }
        /* 좁힌 범위의 끝이 최고점이면 초점이 범위 밖일 수 있다 -> 전체 범위를 한 번 더 */
        if (af_retry && ((af_best == af_rmin && af_rmin > VCM_LOW) || (af_best == af_rmax && af_rmax < VCM_MAX))) {
            af_retry = 0;
            logf_("AF: 범위(%d~%d) 끝 %d 에서 가장 선명 -> 전체 범위로 다시", af_rmin, af_rmax, af_best);
            af_coarse(VCM_LOW, VCM_MAX, 96);
            return 0;
        }
        int half = af_step / 2;
        int lo = af_best - half < af_rmin ? af_rmin : af_best - half;
        af_hi = af_best + half > af_rmax ? af_rmax : af_best + half;
        af_step = 16;
        af_state = AF_FINE;
        af_move(lo);
        return 0;
    }
    /* 최고점으로 돌아간다. 대개 아래로 내려가므로 렌즈가 멈출 때까지 기다린 뒤 끝낸다
     * (그 전에 끝내면 촬영이 아직 움직이는 렌즈로 찍힌다) */
    af_pos = af_best;
    af_state = AF_FINAL;
    af_move(af_pos);
    if (!af_settling) /* 위로 가거나 제자리면 af_settle 프레임 뒤 바로 끝 */
        af_settling = 1, af_settle_n = 25;
    return 0;
}

/* 'C' 레코드: MIPI 모드에서 앱에 보여 줄 컨트롤은 초점 두 가지뿐 (노출/색은 ISP 자동) */
static void mipi_emit_ctrls(unsigned seq, int ok, const char *err) {
    jl = 0;
    jadd("{\"seq\":%u,\"ok\":%s,\"error\":", seq, ok ? "true" : "false");
    jstr(err ? err : "");
    jadd(",\"controls\":[");
    if (vcm_fd >= 0 || sns_name[0]) {
        jadd("{\"id\":%u,\"name\":\"Focus, Automatic Continuous\",\"type\":\"bool\",\"min\":0,\"max\":1,"
             "\"step\":1,\"default\":1,\"value\":%d,\"inactive\":false,\"readonly\":false},",
             CID_FOCUS_AUTO, af_auto);
        jadd("{\"id\":%u,\"name\":\"Focus, Absolute\",\"type\":\"int\",\"min\":0,\"max\":%d,"
             "\"step\":1,\"default\":400,\"value\":%d,\"inactive\":%s,\"readonly\":false}",
             CID_FOCUS_ABS, VCM_MAX, af_pos, af_auto ? "true" : "false");
    }
    jadd("]}");
    emit('C', jb, (uint32_t)jl);
}

static void mipi_ctrl_cmd(const char *line) {
    unsigned seq = 0, id = 0;
    int val = 0;
    if (!strncmp(line, "ctrls", 5)) {
        sscanf(line + 5, "%u", &seq);
        mipi_emit_ctrls(seq, 1, NULL);
        return;
    }
    if (sscanf(line + 4, "%u %u %d", &seq, &id, &val) != 3) {
        mipi_emit_ctrls(seq, 0, "형식: ctrl <seq> <id> <value>");
        return;
    }
    if (id == CID_FOCUS_AUTO) {
        af_auto = val != 0;
        if (af_auto && mode != M_IDLE)
            af_start(0); /* 켜는 순간 한 번 맞춘다 */
        else if (!af_auto) {
            af_state = AF_IDLE, vcm_set(af_pos);
            af_pos_save(); /* 지금 위치를 고정 초점으로 */
        }
    } else if (id == CID_FOCUS_ABS) {
        af_pos = val < 0 ? 0 : val > VCM_MAX ? VCM_MAX : val;
        if (!af_auto && mode != M_IDLE)
            vcm_set(af_pos);
        if (!af_auto)
            af_pos_save();
    } else {
        mipi_emit_ctrls(seq, 0, "MIPI 카메라에서 지원하지 않는 컨트롤입니다");
        return;
    }
    logf_("초점: 자동 %d, 위치 %d", af_auto, af_pos);
    if (!af_busy() && mode != M_IDLE)
        emit_af(af_auto ? "locked" : "manual", af_pos);
    mipi_emit_ctrls(seq, 1, NULL);
}

/* 사진: VENC 채널 1 을 JPEG 로 */
static int jpeg_ready;
static unsigned jpeg_w, jpeg_h, jpeg_vw;

static int jpeg_open(unsigned w, unsigned h, unsigned vw) {
    if (jpeg_ready && jpeg_w == w && jpeg_h == h && jpeg_vw == vw)
        return 0;
    if (jpeg_ready) {
        RK_MPI_VENC_StopRecvFrame(1);
        RK_MPI_VENC_DestroyChn(1);
        jpeg_ready = 0;
    }
    VENC_CHN_ATTR_S a;
    memset(&a, 0, sizeof a);
    a.stVencAttr.enType = RK_VIDEO_ID_JPEG;
    a.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    a.stVencAttr.u32PicWidth = w;
    a.stVencAttr.u32PicHeight = h;
    a.stVencAttr.u32VirWidth = vw;
    a.stVencAttr.u32VirHeight = h;
    a.stVencAttr.u32MaxPicWidth = w;
    a.stVencAttr.u32MaxPicHeight = h;
    a.stVencAttr.u32StreamBufCnt = 1;
    a.stVencAttr.u32BufSize = w * h;
    a.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;
    a.stRcAttr.stMjpegFixQp.u32Qfactor = 92;
    RK_S32 r = RK_MPI_VENC_CreateChn(1, &a);
    if (r != RK_SUCCESS) {
        logf_("JPEG VENC_CreateChn 실패 0x%x", r);
        return -1;
    }
    VENC_JPEG_PARAM_S jp;
    memset(&jp, 0, sizeof jp);
    jp.u32Qfactor = 92;
    RK_MPI_VENC_SetJpegParam(1, &jp);
    VENC_RECV_PIC_PARAM_S rp;
    memset(&rp, 0, sizeof rp);
    rp.s32RecvPicNum = -1;
    if ((r = RK_MPI_VENC_StartRecvFrame(1, &rp)) != RK_SUCCESS) {
        logf_("JPEG VENC_StartRecvFrame 실패 0x%x", r);
        RK_MPI_VENC_DestroyChn(1);
        return -1;
    }
    jpeg_ready = 1, jpeg_w = w, jpeg_h = h, jpeg_vw = vw;
    return 0;
}

static void jpeg_close(void) {
    if (jpeg_ready) {
        RK_MPI_VENC_StopRecvFrame(1);
        RK_MPI_VENC_DestroyChn(1);
        jpeg_ready = 0;
    }
}

/* mainpath NV12 한 장 -> JPEG -> 'J' */
static int snap_jpeg(const uint8_t *src, unsigned w, unsigned h, unsigned stride) {
    if (jpeg_open(w, h, stride))
        return -1;
    size_t sz = (size_t)stride * h * 3 / 2;
    MB_BLK mb = RK_NULL;
    if (RK_MPI_MMZ_Alloc(&mb, sz, RK_MMZ_ALLOC_CACHEABLE) != RK_SUCCESS) {
        logf_("JPEG 버퍼 할당 실패 (%zu)", sz);
        return -1;
    }
    memcpy(RK_MPI_MB_Handle2VirAddr(mb), src, sz);
    RK_MPI_SYS_MmzFlushCache(mb, RK_FALSE);
    VIDEO_FRAME_INFO_S f;
    memset(&f, 0, sizeof f);
    f.stVFrame.pMbBlk = mb;
    f.stVFrame.u32Width = w;
    f.stVFrame.u32Height = h;
    f.stVFrame.u32VirWidth = stride;
    f.stVFrame.u32VirHeight = h;
    f.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    f.stVFrame.u64PTS = now_us() - t0;
    RK_S32 r = RK_MPI_VENC_SendFrame(1, &f, 2000);
    RK_MPI_MB_ReleaseMB(mb);
    if (r != RK_SUCCESS) {
        logf_("JPEG SendFrame 실패 0x%x", r);
        return -1;
    }
    VENC_PACK_S pack;
    VENC_STREAM_S st;
    memset(&st, 0, sizeof st);
    st.pstPack = &pack;
    if ((r = RK_MPI_VENC_GetStream(1, &st, 3000)) != RK_SUCCESS) {
        logf_("JPEG GetStream 실패 0x%x", r);
        return -1;
    }
    for (RK_U32 i = 0; i < st.u32PackCount; i++)
        emit('J', RK_MPI_MB_Handle2VirAddr(st.pstPack[i].pMbBlk), st.pstPack[i].u32Len);
    RK_MPI_VENC_ReleaseStream(1, &st);
    return 0;
}

/* ISP 출력이 모두 멈추면 ISP 프레임 번호가 처음부터 다시 시작한다. rkaiq 를 그대로 두면 새 프레임을
 * 지난 것으로 보고 버려서("sof disorder") 노출/화이트밸런스가 멈추고 화면이 어두워진다.
 * 그래서 미리보기 <-> 사진 전환 때는 selfpath 를 켠 채로 mainpath 만 붙였다 떼고,
 * 출력이 모두 멈추는 경우에는 rkaiq 도 함께 내렸다가 다시 올린다. */
static int mipi_set_mode(int m, int with_preview) {
    int want_sp = m == M_PREVIEW || (m == M_PHOTO && with_preview);
    int want_mp = m == M_PHOTO;
    int keep_sp = sp.on && want_sp;

    if (!want_mp)
        mcam_stop(&mp);
    if (!keep_sp) {
        /* 센서 전원이 꺼지면 VCM 전류도 끊겨 렌즈가 바닥으로 떨어진다. 미리 낮은 곳까지 천천히 내려 둔다 */
        if (vcm_last > VCM_LOW)
            vcm_set(VCM_LOW);
        mcam_stop(&mp);
        mcam_stop(&sp);
        aiq_stop();
    }
    mode = M_IDLE;
    photo_preview = 0;
    if (m != M_IDLE) {
        if (aiq_start()) {
            emitf('E', "ISP(rkaiq)를 시작하지 못했습니다 (rkipc 가 카메라를 쓰고 있는지 확인)");
            return -1;
        }
        if (want_sp && !sp.on && mcam_start(&sp, pw, ph, 4)) {
            emitf('E', "미리보기(selfpath) 시작 실패");
            return -1;
        }
        if (want_sp && (sp.w != pw || sp.h != ph)) {
            emitf('E', "ISP 가 미리보기 %ux%u 를 지원하지 않습니다 (%ux%u)", pw, ph, sp.w, sp.h);
            return -1;
        }
        if (want_mp && !mp.on && mcam_start(&mp, sns_w, sns_h, 3)) {
            emitf('E', "사진 모드(mainpath) 시작 실패");
            return -1;
        }
        photo_preview = m == M_PHOTO && with_preview;
        /* 새로 붙은 시청자가 다음 GOP 까지 기다리지 않게 첫 프레임을 키프레임으로 */
        if (want_sp && !keep_sp)
            RK_MPI_VENC_RequestIDR(0, RK_TRUE);
        if (!keep_sp) {
            /* 스트림을 새로 켰으면 센서와 함께 VCM 도 막 켜진 상태다 */
            af_state = AF_IDLE;
            vcm_on();
            /* 미리보기를 새로 시작하면 노출이 자리 잡은 뒤(약 10프레임) 한 번 맞춘다.
             * 사진 모드는 snap 직전에 맞춘다 */
            caf_ref = -1;
            if (af_auto && m == M_PREVIEW)
                af_start(10);
            else
                emit_af(af_auto ? "locked" : "manual", af_pos);
        }
    } else {
        af_state = AF_IDLE;
    }
    mode = m;
    cw = m == M_PHOTO ? sns_w : pw;
    ch = m == M_PHOTO ? sns_h : ph;
    focus_src = "preview";
    focus_reset();
    emitf('I', "{\"mode\":\"%s\",\"device\":\"%s\",\"width\":%u,\"height\":%u,\"fps\":%u,\"camera_format\":\"%s\","
               "\"preview\":%s,\"preview_width\":%u,\"preview_height\":%u,\"kbps\":%u}",
          mode_name(m), sns_name, m == M_IDLE ? 0 : cw, m == M_IDLE ? 0 : ch, m == M_IDLE ? 0 : pfps,
          m == M_IDLE ? "" : "NV12", (m == M_PREVIEW || photo_preview) ? "true" : "false", pw, ph, kbps);
    logf_("모드 %s %ux%u%s", mode_name(m), cw, ch, photo_preview ? " (+미리보기)" : "");
    return 0;
}

static int run_mipi(void) {
    /* rkaiq 는 로그를 stdout 으로 찍어 레코드 스트림을 깨뜨린다.
     * 레코드는 복제한 fd 로 내고, fd 1 은 stderr 로 돌린다. */
    fflush(stdout);
    out_fd = dup(1);
    af_pos_load(); /* 고정 초점 위치 (자동 초점을 끄면 이 위치를 쓴다) */
    dup2(2, 1);
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        emitf('E', "RK_MPI_SYS_Init 실패");
        return 1;
    }
    if (venc_open()) {
        emitf('E', "H.264 인코더를 열지 못했습니다");
        RK_MPI_SYS_Exit();
        return 1;
    }
    logf_("MIPI 센서 %s %ux%u", sns_name, sns_w, sns_h);
    mipi_set_mode(M_IDLE, 0);

    int rc = 0, timeouts = 0, snap_want = 0, snap_drop = 0;
    uint64_t last_pv = 0, pv_gap = 1000000 / (pfps ? pfps : 30);
    while (!stop_flag) {
        struct pollfd p[3] = {{0, POLLIN, 0}, {sp.on ? sp.fd : -1, POLLIN, 0}, {mp.on ? mp.fd : -1, POLLIN, 0}};
        int n = poll(p, 3, 2000);
        if (n < 0 && errno != EINTR)
            break;
        if (p[0].revents) {
            char line[64];
            int k;
            while ((k = read_cmd(line, sizeof line)) == 1) {
                if (!strcmp(line, "preview"))
                    mipi_set_mode(M_PREVIEW, 1);
                else if (!strcmp(line, "photo"))
                    mipi_set_mode(M_PHOTO, 1);
                else if (!strcmp(line, "photo nopreview"))
                    mipi_set_mode(M_PHOTO, 0);
                else if (!strcmp(line, "idle"))
                    mipi_set_mode(M_IDLE, 0);
                else if (!strcmp(line, "snap")) {
                    if (mode != M_PHOTO)
                        emitf('E', "snap 은 photo 모드에서만 됩니다");
                    snap_want = 1;
                    snap_drop = (int)skip; /* AF 가 끝난 뒤부터 센다 */
                    /* 연속 AF 는 미리보기에서 이미 맞춰 두었으니 바로 찍는다 (맞추는 중이면 끝날 때까지 기다림) */
                    if (af_auto && !af_cont && mode == M_PHOTO && !af_busy())
                        af_start(0);
                } else if (!strcmp(line, "idr"))
                    RK_MPI_VENC_RequestIDR(0, RK_TRUE);
                else if (!strcmp(line, "focus on"))
                    focus_on = 1, focus_reset();
                else if (!strcmp(line, "focus off"))
                    focus_on = 0;
                else if (!strcmp(line, "af"))
                    af_start(0);
                else if (!strncmp(line, "ctrls", 5) || !strncmp(line, "ctrl ", 5))
                    mipi_ctrl_cmd(line);
                else if (!strcmp(line, "quit"))
                    stop_flag = 1;
                timeouts = 0;
            }
            if (k < 0)
                stop_flag = 1;
        }
        if (mode == M_IDLE)
            continue;
        if (n == 0) {
            if (++timeouts >= 3) {
                emitf('E', "ISP 프레임이 6초 동안 없습니다");
                rc = 1;
                break;
            }
            continue;
        }
        struct v4l2_plane lpl[VIDEO_MAX_PLANES];
        struct v4l2_buffer b;
        unsigned cnt;
        if (mp.on && (p[2].revents & POLLIN)) {
            int d = mcam_latest(&mp, &b, lpl, &cnt);
            if (d < 0) {
                emitf('E', "mainpath DQBUF 실패: %s", strerror(errno));
                rc = 1;
                break;
            }
            if (d == 0) {
                timeouts = 0;
                if (!sp.on) { /* 미리보기가 없으면 사진 프레임으로 AF/AWB */
                    af_frame(mp.b[b.index].start, mp.w, mp.h, mp.stride);
                    awb_frame(mp.b[b.index].start, mp.w, mp.h, mp.stride);
                }
                if (snap_want && !af_busy()) {
                    if (snap_drop > 0)
                        snap_drop -= (int)cnt;
                    if (snap_drop <= 0) {
                        if (snap_jpeg(mp.b[b.index].start, mp.w, mp.h, mp.stride))
                            emitf('E', "JPEG 인코딩 실패");
                        snap_want = 0;
                    }
                }
                mcam_release(&mp, &b);
            }
        }
        if (sp.on && (p[1].revents & POLLIN)) {
            int d = mcam_latest(&sp, &b, lpl, &cnt);
            if (d < 0) {
                emitf('E', "selfpath DQBUF 실패: %s", strerror(errno));
                rc = 1;
                break;
            }
            if (d == 0) {
                timeouts = 0;
                af_frame(sp.b[b.index].start, sp.w, sp.h, sp.stride);
                if (mode == M_PREVIEW)
                    caf_frame(sp.b[b.index].start, sp.w, sp.h, sp.stride);
                awb_frame(sp.b[b.index].start, sp.w, sp.h, sp.stride);
                /* 설정한 미리보기 fps 를 넘지 않게 (ISP 는 센서 속도로 낸다) */
                uint64_t t = now_us();
                if (t - last_pv >= pv_gap * 9 / 10) {
                    last_pv = t;
                    struct raw r = {sp.b[b.index].start, sp.stride, V4L2_PIX_FMT_NV12};
                    if (encode(fill_from_camera, &r)) {
                        mcam_release(&sp, &b);
                        emitf('E', "인코딩 실패");
                        rc = 1;
                        break;
                    }
                }
                mcam_release(&sp, &b);
            }
        }
    }
    mcam_stop(&mp);
    mcam_stop(&sp);
    aiq_stop();
    jpeg_close();
    venc_close();
    RK_MPI_SYS_Exit();
    return rc;
}

/* ------------------------------------------------------------------ list */

static int run_list(void) {
    if (cam_open(dev))
        return 1;
    printf("%s\n", dev);
    struct v4l2_fmtdesc d;
    for (unsigned i = 0;; i++) {
        memset(&d, 0, sizeof d);
        d.index = i;
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(cam_fd, VIDIOC_ENUM_FMT, &d) < 0)
            break;
        char a[5];
        printf("  %s (%s):", fourcc(d.pixelformat, a), d.description);
        struct v4l2_frmsizeenum s;
        for (unsigned j = 0;; j++) {
            memset(&s, 0, sizeof s);
            s.index = j;
            s.pixel_format = d.pixelformat;
            if (xioctl(cam_fd, VIDIOC_ENUM_FRAMESIZES, &s) < 0 || s.type != V4L2_FRMSIZE_TYPE_DISCRETE)
                break;
            struct v4l2_frmivalenum fi;
            memset(&fi, 0, sizeof fi);
            fi.pixel_format = d.pixelformat;
            fi.width = s.discrete.width;
            fi.height = s.discrete.height;
            unsigned fps = 0;
            if (xioctl(cam_fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0 && fi.type == V4L2_FRMIVAL_TYPE_DISCRETE &&
                fi.discrete.numerator)
                fps = fi.discrete.denominator / fi.discrete.numerator;
            printf(" %ux%u@%u", s.discrete.width, s.discrete.height, fps);
        }
        printf("\n");
    }
    close(cam_fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: camhelper run|list [-d dev] [-W w -H h -f fps -b kbps -g gop] [-X w -Y h -s skip]"
                        " [-A af_min-af_max]\n");
        return 2;
    }
    const char *m = argv[1];
    int c;
    optind = 2;
    while ((c = getopt(argc, argv, "d:W:H:f:b:g:X:Y:s:A:P:M:")) != -1) {
        switch (c) {
        case 'd': snprintf(dev, sizeof dev, "%s", optarg); break;
        case 'A': { /* AF 탐색 범위 "최소-최대" (렌즈 VCM 0~1023) */
            int lo = 0, hi = VCM_MAX;
            if (sscanf(optarg, "%d-%d", &lo, &hi) == 2)
                af_set_range(lo, hi);
            break;
        }
        case 'P': af_far = !strcmp(optarg, "far"); break; /* AF: far = 먼 쪽(두피) 우선, best = 가장 선명한 곳 */
        case 'M': af_cont = strcmp(optarg, "snap") != 0; break; /* AF: continuous(기본) | snap */
        case 'W': pw = (unsigned)atoi(optarg); break;
        case 'H': ph = (unsigned)atoi(optarg); break;
        case 'f': pfps = (unsigned)atoi(optarg); break;
        case 'b': kbps = (unsigned)atoi(optarg); break;
        case 'g': gop = (unsigned)atoi(optarg); break;
        case 'X': xw = (unsigned)atoi(optarg); break;
        case 'Y': xh = (unsigned)atoi(optarg); break;
        case 's': skip = (unsigned)atoi(optarg); break;
        default: return 2;
        }
    }
    if (pw % 16 || ph % 2 || pw > 4096) {
        fprintf(stderr, "미리보기 크기 %ux%u: 가로는 16 배수, 세로는 짝수\n", pw, ph);
        return 2;
    }
    t0 = now_us();
    if (getenv("CAMHELPER_AF_CLIP"))
        af_clip = atoi(getenv("CAMHELPER_AF_CLIP"));
    if (getenv("CAMHELPER_AF_SETTLE"))
        af_settle = atoi(getenv("CAMHELPER_AF_SETTLE"));
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

    dev_auto = !dev[0];
    /* UVC 카메라가 없으면 rkisp 에 붙은 MIPI 센서를 쓴다 (-d mipi 로 강제할 수도 있다) */
    if ((!strcmp(dev, "mipi") || (!dev[0] && find_uvc(dev, sizeof dev))) && !find_mipi_sensor()) {
        snprintf(dev, sizeof dev, "mipi");
        if (!strcmp(m, "run"))
            return run_mipi();
        printf("MIPI %s %ux%u -> rkisp_selfpath(미리보기) / rkisp_mainpath(사진), NV12\n", sns_name, sns_w, sns_h);
        return 0;
    }
    if (!dev[0] || !strcmp(dev, "mipi")) {
        if (strcmp(m, "list"))
            emitf('E', "UVC 카메라를 찾지 못했습니다");
        else
            fprintf(stderr, "UVC 카메라를 찾지 못했습니다\n");
        return 1;
    }
    if (!strcmp(m, "run"))
        return run();
    if (!strcmp(m, "list"))
        return run_list();
    fprintf(stderr, "알 수 없는 모드: %s\n", m);
    return 2;
}
