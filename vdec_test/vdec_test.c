/*
 * RV1106 Rockit VDEC JPEG/MJPEG 디코딩 테스트
 *
 * usage: vdec_test <file.jpg> <width> <height> [jpeg|mjpeg] [loops] [out.yuv|-] [frame_buf_size]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rk_comm_vdec.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"

#define CHN 0

static long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    unsigned char *buf;

    if (!fp) {
        perror(path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    *len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc(*len);
    if (fread(buf, 1, *len, fp) != *len) {
        free(buf);
        buf = NULL;
    }
    fclose(fp);
    return buf;
}

static int decode_one(const unsigned char *data, size_t len, int loop, const char *out_path) {
    MB_BLK mb = RK_NULL;
    VDEC_STREAM_S stream;
    VIDEO_FRAME_INFO_S frame;
    long long t0, t1;
    RK_S32 ret;

    ret = RK_MPI_MMZ_Alloc(&mb, len, RK_MMZ_ALLOC_CACHEABLE);
    if (ret != RK_SUCCESS) {
        printf("RK_MPI_MMZ_Alloc failed: 0x%x\n", ret);
        return -1;
    }
    memcpy(RK_MPI_MB_Handle2VirAddr(mb), data, len);
    RK_MPI_SYS_MmzFlushCache(mb, RK_FALSE);

    memset(&stream, 0, sizeof(stream));
    stream.pMbBlk = mb;
    stream.u32Len = len;
    stream.u64PTS = loop;
    stream.bEndOfStream = RK_FALSE;
    stream.bEndOfFrame = RK_TRUE;
    stream.bBypassMbBlk = RK_FALSE;

    t0 = now_us();
    ret = RK_MPI_VDEC_SendStream(CHN, &stream, 3000);
    if (ret != RK_SUCCESS) {
        printf("[%d] RK_MPI_VDEC_SendStream failed: 0x%x\n", loop, ret);
        RK_MPI_MB_ReleaseMB(mb);
        return -1;
    }

    memset(&frame, 0, sizeof(frame));
    ret = RK_MPI_VDEC_GetFrame(CHN, &frame, 3000);
    t1 = now_us();
    RK_MPI_MB_ReleaseMB(mb);
    if (ret != RK_SUCCESS) {
        printf("[%d] RK_MPI_VDEC_GetFrame failed: 0x%x\n", loop, ret);
        return -1;
    }

    printf("[%d] decoded %ux%u (vir %ux%u) pixfmt=%d compress=%d size=%llu  %.2f ms\n", loop,
           frame.stVFrame.u32Width, frame.stVFrame.u32Height,
           frame.stVFrame.u32VirWidth, frame.stVFrame.u32VirHeight,
           frame.stVFrame.enPixelFormat, frame.stVFrame.enCompressMode,
           (unsigned long long)RK_MPI_MB_GetSize(frame.stVFrame.pMbBlk), (t1 - t0) / 1000.0);

    if (out_path && loop == 0) {
        FILE *fp = fopen(out_path, "wb");
        if (fp) {
            void *va = RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);
            RK_MPI_SYS_MmzFlushCache(frame.stVFrame.pMbBlk, RK_TRUE);
            /* NV12/NV16 등 semi-planar 가정: Y + UV, 원본 크기 그대로 덤프 */
            fwrite(va, 1, RK_MPI_MB_GetSize(frame.stVFrame.pMbBlk), fp);
            fclose(fp);
            printf("    dumped to %s\n", out_path);
        }
    }

    RK_MPI_VDEC_ReleaseFrame(CHN, &frame);
    return 0;
}

int main(int argc, char **argv) {
    VDEC_CHN_ATTR_S attr;
    unsigned char *data;
    size_t len;
    int width, height, loops = 1, ok = 0, i;
    RK_CODEC_ID_E codec = RK_VIDEO_ID_JPEG;
    const char *out_path = NULL;
    RK_U32 frame_buf_size = 0;
    RK_S32 ret;

    if (argc < 4) {
        printf("usage: %s <file.jpg> <width> <height> [jpeg|mjpeg] [loops] [out.yuv|-] [frame_buf_size]\n",
               argv[0]);
        return 1;
    }
    width = atoi(argv[2]);
    height = atoi(argv[3]);
    if (argc > 4 && !strcmp(argv[4], "mjpeg"))
        codec = RK_VIDEO_ID_MJPEG;
    if (argc > 5)
        loops = atoi(argv[5]);
    if (argc > 6 && strcmp(argv[6], "-"))
        out_path = argv[6];
    if (argc > 7)
        frame_buf_size = strtoul(argv[7], NULL, 0);

    data = read_file(argv[1], &len);
    if (!data)
        return 1;
    printf("input %s: %zu bytes, %dx%d, codec=%s\n", argv[1], len, width, height,
           codec == RK_VIDEO_ID_JPEG ? "JPEG" : "MJPEG");

    ret = RK_MPI_SYS_Init();
    printf("RK_MPI_SYS_Init: 0x%x\n", ret);
    if (ret != RK_SUCCESS)
        return 1;

    memset(&attr, 0, sizeof(attr));
    attr.enMode = VIDEO_MODE_FRAME;
    attr.enType = codec;
    attr.u32PicWidth = width;
    attr.u32PicHeight = height;
    attr.u32FrameBufCnt = 2;
    attr.u32StreamBufCnt = 2;
    /* 기본 프레임 버퍼는 w*h*2 라 4:4:4 JPEG(w*h*3 필요)는 실패함 */
    if (frame_buf_size > 0)
        attr.u32FrameBufSize = frame_buf_size;

    ret = RK_MPI_VDEC_CreateChn(CHN, &attr);
    printf("RK_MPI_VDEC_CreateChn: 0x%x\n", ret);
    if (ret != RK_SUCCESS)
        goto exit_sys;

    ret = RK_MPI_VDEC_StartRecvStream(CHN);
    printf("RK_MPI_VDEC_StartRecvStream: 0x%x\n", ret);
    if (ret != RK_SUCCESS)
        goto destroy;

    for (i = 0; i < loops; i++) {
        if (decode_one(data, len, i, out_path) == 0)
            ok++;
    }
    printf("RESULT: %d/%d frames decoded\n", ok, loops);

    RK_MPI_VDEC_StopRecvStream(CHN);
destroy:
    RK_MPI_VDEC_DestroyChn(CHN);
exit_sys:
    RK_MPI_SYS_Exit();
    free(data);
    return ok > 0 ? 0 : 1;
}
