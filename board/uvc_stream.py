#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uvc_stream.py - UVC(USB) MJPEG camera -> HTTP streaming server

Luckfox Pico (RV1106) + Arducam B0290 IMX298 16MP Autofocus USB Camera 용.
rv1126_usb/board/uvc_stream.py 를 RV1106(armhf, 램 ~170MB)에 맞춰 옮긴 것.
표준 라이브러리만 사용한다 (ctypes 로 V4L2 를 직접 호출).

  GET /               뷰어 페이지
  GET /stream.mjpg    multipart/x-mixed-replace MJPEG 스트림
  GET /snapshot.jpg   현재 프레임 1장
  GET /status.json    상태 (fps, 해상도, 클라이언트 수)
  GET /audio.wav      마이크 오디오 (arecord 가 있을 때만)
  GET /focus.json     초점 상태 (auto, value, min, max)
  POST /focus         {"auto": true} 또는 {"value": 400}
  POST /photo         최대 해상도(16MP) 사진 1장 -> {"url": "/photos/<name>.jpg"}
  POST /webrtc        WebRTC offer SDP -> answer SDP (H.264 영상 + 스냅샷 데이터 채널)
"""

import ctypes
import errno
import fcntl
import json
import mmap
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------- V4L2 ioctl

_IOC_NRBITS, _IOC_TYPEBITS, _IOC_SIZEBITS = 8, 8, 14
_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = _IOC_NRSHIFT + _IOC_NRBITS
_IOC_SIZESHIFT = _IOC_TYPESHIFT + _IOC_TYPEBITS
_IOC_DIRSHIFT = _IOC_SIZESHIFT + _IOC_SIZEBITS
_IOC_WRITE, _IOC_READ = 1, 2


def _IOC(d, t, nr, size):
    return ((d << _IOC_DIRSHIFT) | (ord(t) << _IOC_TYPESHIFT) |
            (nr << _IOC_NRSHIFT) | (size << _IOC_SIZESHIFT))


def _IOR(t, nr, st):
    return _IOC(_IOC_READ, t, nr, ctypes.sizeof(st))


def _IOW(t, nr, st):
    return _IOC(_IOC_WRITE, t, nr, ctypes.sizeof(st))


def _IOWR(t, nr, st):
    return _IOC(_IOC_READ | _IOC_WRITE, t, nr, ctypes.sizeof(st))


def fourcc(s):
    return ord(s[0]) | (ord(s[1]) << 8) | (ord(s[2]) << 16) | (ord(s[3]) << 24)


def fourcc_str(v):
    return "".join(chr((v >> (8 * i)) & 0xFF) for i in range(4))


V4L2_PIX_FMT_MJPEG = fourcc("MJPG")
V4L2_PIX_FMT_JPEG = fourcc("JPEG")

V4L2_BUF_TYPE_VIDEO_CAPTURE = 1
V4L2_MEMORY_MMAP = 1
V4L2_FIELD_ANY = 0
V4L2_CAP_VIDEO_CAPTURE = 0x00000001
V4L2_CAP_STREAMING = 0x04000000
V4L2_CAP_TIMEPERFRAME = 0x1000
V4L2_FRMSIZE_TYPE_DISCRETE = 1


class timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_usec", ctypes.c_long)]


class v4l2_capability(ctypes.Structure):
    _fields_ = [("driver", ctypes.c_char * 16),
                ("card", ctypes.c_char * 32),
                ("bus_info", ctypes.c_char * 32),
                ("version", ctypes.c_uint32),
                ("capabilities", ctypes.c_uint32),
                ("device_caps", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 3)]


class v4l2_fmtdesc(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32),
                ("type", ctypes.c_uint32),
                ("flags", ctypes.c_uint32),
                ("description", ctypes.c_char * 32),
                ("pixelformat", ctypes.c_uint32),
                ("mbus_code", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 3)]


class v4l2_pix_format(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32),
                ("height", ctypes.c_uint32),
                ("pixelformat", ctypes.c_uint32),
                ("field", ctypes.c_uint32),
                ("bytesperline", ctypes.c_uint32),
                ("sizeimage", ctypes.c_uint32),
                ("colorspace", ctypes.c_uint32),
                ("priv", ctypes.c_uint32),
                ("flags", ctypes.c_uint32),
                ("ycbcr_enc", ctypes.c_uint32),
                ("quantization", ctypes.c_uint32),
                ("xfer_func", ctypes.c_uint32)]


class _fmt_union(ctypes.Union):
    # 커널 union 에는 v4l2_window(포인터 멤버 포함)가 들어 있어서 union 정렬이
    # 포인터 크기로 올라간다. _align 더미가 그 정렬을 재현해 ioctl 인자 크기를
    # 32/64-bit 양쪽에서 커널과 일치시킨다 (armhf 204, x86_64 208).
    _fields_ = [("pix", v4l2_pix_format),
                ("_align", ctypes.c_void_p),
                ("raw_data", ctypes.c_uint8 * 200)]


class v4l2_format(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("fmt", _fmt_union)]


class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [("count", ctypes.c_uint32),
                ("type", ctypes.c_uint32),
                ("memory", ctypes.c_uint32),
                ("capabilities", ctypes.c_uint32),
                ("flags", ctypes.c_uint8),
                ("reserved", ctypes.c_uint8 * 3)]


class v4l2_timecode(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("frames", ctypes.c_uint8), ("seconds", ctypes.c_uint8),
                ("minutes", ctypes.c_uint8), ("hours", ctypes.c_uint8),
                ("userbits", ctypes.c_uint8 * 4)]


class v4l2_buffer(ctypes.Structure):
    # m 은 union { __u32 offset; unsigned long userptr; void *planes; __s32 fd; }
    # MMAP 모드에서는 offset 만 쓰므로 포인터 크기 정수 하나로 둔다.
    _fields_ = [("index", ctypes.c_uint32),
                ("type", ctypes.c_uint32),
                ("bytesused", ctypes.c_uint32),
                ("flags", ctypes.c_uint32),
                ("field", ctypes.c_uint32),
                ("timestamp", timeval),
                ("timecode", v4l2_timecode),
                ("sequence", ctypes.c_uint32),
                ("memory", ctypes.c_uint32),
                ("m", ctypes.c_ulong),
                ("length", ctypes.c_uint32),
                ("reserved2", ctypes.c_uint32),
                ("request_fd", ctypes.c_int32)]


class v4l2_fract(ctypes.Structure):
    _fields_ = [("numerator", ctypes.c_uint32),
                ("denominator", ctypes.c_uint32)]


class v4l2_captureparm(ctypes.Structure):
    _fields_ = [("capability", ctypes.c_uint32),
                ("capturemode", ctypes.c_uint32),
                ("timeperframe", v4l2_fract),
                ("extendedmode", ctypes.c_uint32),
                ("readbuffers", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 4)]


class _parm_union(ctypes.Union):
    _fields_ = [("capture", v4l2_captureparm), ("raw_data", ctypes.c_uint8 * 200)]


class v4l2_streamparm(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("parm", _parm_union)]


class _frmsize_discrete(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32)]


class _frmsize_stepwise(ctypes.Structure):
    _fields_ = [("min_width", ctypes.c_uint32), ("max_width", ctypes.c_uint32),
                ("step_width", ctypes.c_uint32), ("min_height", ctypes.c_uint32),
                ("max_height", ctypes.c_uint32), ("step_height", ctypes.c_uint32)]


class _frmsize_union(ctypes.Union):
    _fields_ = [("discrete", _frmsize_discrete), ("stepwise", _frmsize_stepwise)]


class v4l2_frmsizeenum(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("pixel_format", ctypes.c_uint32),
                ("type", ctypes.c_uint32), ("u", _frmsize_union),
                ("reserved", ctypes.c_uint32 * 2)]


class v4l2_control(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint32), ("value", ctypes.c_int32)]


class v4l2_queryctrl(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint32), ("type", ctypes.c_uint32),
                ("name", ctypes.c_char * 32),
                ("minimum", ctypes.c_int32), ("maximum", ctypes.c_int32),
                ("step", ctypes.c_int32), ("default_value", ctypes.c_int32),
                ("flags", ctypes.c_uint32), ("reserved", ctypes.c_uint32 * 2)]


V4L2_CID_FOCUS_ABSOLUTE = 0x009a090a
V4L2_CID_FOCUS_AUTO = 0x009a090c
V4L2_CTRL_FLAG_DISABLED = 0x0001

VIDIOC_QUERYCAP = _IOR("V", 0, v4l2_capability)
VIDIOC_ENUM_FMT = _IOWR("V", 2, v4l2_fmtdesc)
VIDIOC_G_FMT = _IOWR("V", 4, v4l2_format)
VIDIOC_S_FMT = _IOWR("V", 5, v4l2_format)
VIDIOC_REQBUFS = _IOWR("V", 8, v4l2_requestbuffers)
VIDIOC_QUERYBUF = _IOWR("V", 9, v4l2_buffer)
VIDIOC_QBUF = _IOWR("V", 15, v4l2_buffer)
VIDIOC_DQBUF = _IOWR("V", 17, v4l2_buffer)
VIDIOC_STREAMON = _IOW("V", 18, ctypes.c_int)
VIDIOC_STREAMOFF = _IOW("V", 19, ctypes.c_int)
VIDIOC_G_PARM = _IOWR("V", 21, v4l2_streamparm)
VIDIOC_S_PARM = _IOWR("V", 22, v4l2_streamparm)
VIDIOC_ENUM_FRAMESIZES = _IOWR("V", 74, v4l2_frmsizeenum)
VIDIOC_G_CTRL = _IOWR("V", 27, v4l2_control)
VIDIOC_S_CTRL = _IOWR("V", 28, v4l2_control)
VIDIOC_QUERYCTRL = _IOWR("V", 36, v4l2_queryctrl)


def xioctl(fd, req, arg):
    """EINTR 재시도 포함 ioctl."""
    while True:
        try:
            return fcntl.ioctl(fd, req, arg)
        except OSError as e:
            if e.errno == errno.EINTR:
                continue
            raise


# ----------------------------------------------------------- MJPEG DHT 보정
# UVC MJPEG 는 규격상 Huffman 테이블(DHT)을 프레임에서 생략할 수 있다.
# 브라우저는 DHT 없는 JPEG 를 디코드하지 못하므로 표준 테이블을 끼워 넣는다.
_STD_DHT = bytes([
    0xFF, 0xC4, 0x01, 0xA2,
    0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04,
    0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05,
    0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1,
    0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19,
    0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54,
    0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA,
    0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
    0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
    0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
    0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
    0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04,
    0x04, 0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05,
    0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52,
    0xF0, 0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1,
    0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53,
    0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
    0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2,
    0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5,
    0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
    0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
])


def fix_mjpeg(buf):
    """DHT 가 없으면 SOS 앞에 표준 DHT 를 삽입한 bytes 를 돌려준다."""
    if len(buf) < 4 or buf[0] != 0xFF or buf[1] != 0xD8:
        return buf
    i, n = 2, len(buf)
    while i + 3 < n:
        if buf[i] != 0xFF:
            i += 1
            continue
        marker = buf[i + 1]
        if marker == 0xC4:          # DHT 있음 -> 그대로 사용
            return buf
        if marker == 0xDA:          # SOS 도달 -> 그 앞에 삽입
            return buf[:i] + _STD_DHT + buf[i:]
        if marker == 0xD8 or marker == 0x01 or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        seglen = (buf[i + 2] << 8) | buf[i + 3]
        if seglen < 2:
            return buf
        i += 2 + seglen
    return buf


# ---------------------------------------------------------------- Camera

class Camera:
    def __init__(self, device, width, height, fps, nbufs=4):
        self.device = device
        self.req_w, self.req_h, self.req_fps = width, height, fps
        self.nbufs = nbufs
        self.fd = None
        self.bufs = []
        self.width = self.height = 0
        self.pixelformat = 0
        self.fps = float(fps)
        self.streaming = False
        self.card = ""

    def open(self):
        self.fd = os.open(self.device, os.O_RDWR | os.O_NONBLOCK)
        cap = v4l2_capability()
        xioctl(self.fd, VIDIOC_QUERYCAP, cap)
        self.card = cap.card.decode("utf-8", "replace").strip()
        caps = cap.device_caps or cap.capabilities
        if not caps & V4L2_CAP_VIDEO_CAPTURE:
            raise RuntimeError("%s: video capture 노드가 아님" % self.device)
        if not caps & V4L2_CAP_STREAMING:
            raise RuntimeError("%s: streaming I/O 미지원" % self.device)
        return self

    def list_formats(self):
        out, i = [], 0
        while True:
            f = v4l2_fmtdesc(index=i, type=V4L2_BUF_TYPE_VIDEO_CAPTURE)
            try:
                xioctl(self.fd, VIDIOC_ENUM_FMT, f)
            except OSError:
                break
            out.append((f.pixelformat, f.description.decode("utf-8", "replace")))
            i += 1
        return out

    def list_sizes(self, pixfmt):
        out, i = [], 0
        while True:
            fs = v4l2_frmsizeenum(index=i, pixel_format=pixfmt)
            try:
                xioctl(self.fd, VIDIOC_ENUM_FRAMESIZES, fs)
            except OSError:
                break
            if fs.type == V4L2_FRMSIZE_TYPE_DISCRETE:
                out.append((fs.u.discrete.width, fs.u.discrete.height))
            else:
                out.append((fs.u.stepwise.max_width, fs.u.stepwise.max_height))
                break
            i += 1
        return out

    def configure(self):
        avail = [p for p, _ in self.list_formats()]
        pick = next((p for p in (V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_JPEG)
                     if p in avail), None)
        if pick is None:
            raise RuntimeError(
                "카메라가 MJPEG 를 내보내지 않습니다. 지원 포맷: %s\n"
                "  (이 스크립트는 카메라의 하드웨어 JPEG 출력을 그대로 전달합니다.)"
                % ", ".join(fourcc_str(p) for p in avail))

        fmt = v4l2_format(type=V4L2_BUF_TYPE_VIDEO_CAPTURE)
        fmt.fmt.pix.width = self.req_w
        fmt.fmt.pix.height = self.req_h
        fmt.fmt.pix.pixelformat = pick
        fmt.fmt.pix.field = V4L2_FIELD_ANY
        xioctl(self.fd, VIDIOC_S_FMT, fmt)   # 드라이버가 가장 가까운 값으로 보정

        self.width = fmt.fmt.pix.width
        self.height = fmt.fmt.pix.height
        self.pixelformat = fmt.fmt.pix.pixelformat

        parm = v4l2_streamparm(type=V4L2_BUF_TYPE_VIDEO_CAPTURE)
        try:
            xioctl(self.fd, VIDIOC_G_PARM, parm)
            if parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME:
                parm.parm.capture.timeperframe.numerator = 1
                parm.parm.capture.timeperframe.denominator = self.req_fps
                xioctl(self.fd, VIDIOC_S_PARM, parm)
                xioctl(self.fd, VIDIOC_G_PARM, parm)
            tpf = parm.parm.capture.timeperframe
            if tpf.numerator:
                self.fps = tpf.denominator / float(tpf.numerator)
        except OSError:
            pass
        return self

    def _map_buffers(self):
        rb = v4l2_requestbuffers(count=self.nbufs,
                                 type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                 memory=V4L2_MEMORY_MMAP)
        xioctl(self.fd, VIDIOC_REQBUFS, rb)
        if rb.count < 2:
            raise RuntimeError("버퍼 메모리 부족 (count=%d)" % rb.count)
        self.nbufs = rb.count
        for i in range(rb.count):
            b = v4l2_buffer(index=i, type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
                            memory=V4L2_MEMORY_MMAP)
            xioctl(self.fd, VIDIOC_QUERYBUF, b)
            self.bufs.append(mmap.mmap(self.fd, b.length, mmap.MAP_SHARED,
                                       mmap.PROT_READ | mmap.PROT_WRITE,
                                       offset=b.m))

    def start(self):
        self._map_buffers()
        for i in range(self.nbufs):
            b = v4l2_buffer(index=i, type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
                            memory=V4L2_MEMORY_MMAP)
            xioctl(self.fd, VIDIOC_QBUF, b)
        xioctl(self.fd, VIDIOC_STREAMON,
               ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE))
        self.streaming = True
        return self

    def read_frame(self, timeout=2.0):
        """JPEG bytes 하나를 돌려준다. 타임아웃이면 None."""
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return None
        b = v4l2_buffer(index=0, type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
                        memory=V4L2_MEMORY_MMAP)
        try:
            xioctl(self.fd, VIDIOC_DQBUF, b)
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EIO):
                return None
            raise
        mm = self.bufs[b.index]
        try:
            data = bytes(mm[:b.bytesused]) if b.bytesused else b""
        finally:
            xioctl(self.fd, VIDIOC_QBUF, b)
        return data or None

    def _halt(self):
        """스트리밍을 멈추고 버퍼를 반납한다 (fd 는 유지)."""
        if self.streaming:
            try:
                xioctl(self.fd, VIDIOC_STREAMOFF,
                       ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE))
            except OSError:
                pass
            self.streaming = False
        for mm in self.bufs:
            try:
                mm.close()
            except Exception:
                pass
        if self.bufs:
            try:      # count=0: 드라이버 버퍼 해제 (이걸 해야 S_FMT 가 EBUSY 없이 된다)
                xioctl(self.fd, VIDIOC_REQBUFS, v4l2_requestbuffers(
                    count=0, type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    memory=V4L2_MEMORY_MMAP))
            except OSError:
                pass
        self.bufs = []

    def restart(self, width, height, fps, nbufs=None):
        """같은 fd 로 해상도를 바꿔 다시 스트리밍한다.
        fd 를 닫지 않으므로 초점 같은 컨트롤 설정도 그대로 유지된다."""
        self._halt()
        self.req_w, self.req_h, self.req_fps = width, height, int(round(fps))
        if nbufs:
            self.nbufs = nbufs
        return self.configure().start()

    def max_size(self):
        """MJPEG 로 낼 수 있는 가장 큰 해상도."""
        sizes = self.list_sizes(self.pixelformat or V4L2_PIX_FMT_MJPEG)
        return max(sizes, key=lambda s: s[0] * s[1]) if sizes else None

    def stop(self):
        if self.fd is None:
            return
        self._halt()
        os.close(self.fd)
        self.fd = None

    # --- 카메라 컨트롤 (V4L2_CID_*) ---
    def query_ctrl(self, cid):
        q = v4l2_queryctrl(id=cid)
        try:
            xioctl(self.fd, VIDIOC_QUERYCTRL, q)
        except OSError:
            return None
        if q.flags & V4L2_CTRL_FLAG_DISABLED:
            return None
        return dict(min=q.minimum, max=q.maximum, step=q.step or 1,
                    default=q.default_value)

    def get_ctrl(self, cid):
        c = v4l2_control(id=cid)
        xioctl(self.fd, VIDIOC_G_CTRL, c)
        return c.value

    def set_ctrl(self, cid, value):
        xioctl(self.fd, VIDIOC_S_CTRL, v4l2_control(id=cid, value=int(value)))


# ------------------------------------------------------- Frame broadcaster

class FrameHub:
    """최신 프레임 1장만 들고 있다가 깨어난 클라이언트에게 준다.
    느린 클라이언트 때문에 캡처가 막히지 않도록 큐를 쌓지 않는다."""

    def __init__(self):
        self.cond = threading.Condition()
        self.frame = None
        self.seq = 0
        self.fps = 0.0
        self.clients = 0
        self.started = time.monotonic()
        self._tick = []

    def publish(self, jpg):
        now = time.monotonic()
        with self.cond:
            self.frame = jpg
            self.seq += 1
            self._tick.append(now)
            if len(self._tick) > 30:
                self._tick.pop(0)
            if len(self._tick) > 1:
                span = self._tick[-1] - self._tick[0]
                self.fps = (len(self._tick) - 1) / span if span > 0 else 0.0
            self.cond.notify_all()

    def get(self, last_seq, timeout=5.0):
        with self.cond:
            if self.seq == last_seq:
                self.cond.wait(timeout)
            if self.seq == last_seq or self.frame is None:
                return None, last_seq
            return self.frame, self.seq


HUB = FrameHub()
STATE = {"device": "", "card": "", "width": 0, "height": 0,
         "pixfmt": "", "target_fps": 0, "audio": False,
         "audio_dev": None, "audio_rate": 48000, "port": 0}


class PhotoDesk:
    """스트리밍 해상도와 별개로 최대 해상도 사진 1장을 찍는다.
    캡처 스레드가 요청을 받아 해상도 전환 -> 촬영 -> 원래 해상도 복귀를 한다
    (그동안 1~2초 스트림이 멈춘다)."""

    SKIP = 3            # 모드 전환 직후 프레임은 노출이 안 맞아서 버린다
    KEEP = 4            # 메모리에 남겨둘 최근 사진 수

    def __init__(self):
        self.size = None                 # (w, h) - main() 에서 채움
        self.lock = threading.Lock()     # 한 번에 한 장
        self.want = threading.Event()
        self.done = threading.Event()
        self.result = None
        self.photos = []                 # [(name, jpg)]

    def take(self, timeout=20.0):
        with self.lock:
            self.result = None
            self.done.clear()
            self.want.set()
            if not self.done.wait(timeout):
                self.want.clear()
                raise RuntimeError("촬영 시간 초과")
            if isinstance(self.result, Exception):
                raise self.result
            jpg = self.result
            name = time.strftime("%Y%m%d_%H%M%S")
            self.photos = [p for p in self.photos if p[0] != name][-(self.KEEP - 1):]
            self.photos.append((name, jpg))
            return name, jpg

    def get(self, name):
        for n, jpg in self.photos:
            if n == name:
                return jpg
        return None

    def shoot(self, cam, fix_dht):
        """캡처 스레드에서 호출."""
        self.want.clear()
        base = (cam.width, cam.height, cam.fps, cam.nbufs)
        try:
            w, h = self.size
            t0 = time.monotonic()
            cam.restart(w, h, 10, nbufs=2)
            jpg = None
            for _ in range(self.SKIP + 1):
                f = cam.read_frame(timeout=3.0)
                if f:
                    jpg = f
            if not jpg:
                raise RuntimeError("고해상도 프레임을 받지 못했습니다")
            eoi = jpg.rfind(b"\xff\xd9")       # UVC 가 EOI 뒤에 붙이는 0 패딩 제거
            if eoi > 0 and len(jpg) - eoi <= 64:
                jpg = jpg[:eoi + 2]
            self.result = fix_mjpeg(jpg) if fix_dht else jpg
            sys.stderr.write("[photo] %dx%d %d KB (%.1fs)\n" % (
                cam.width, cam.height, len(jpg) // 1024, time.monotonic() - t0))
        except Exception as e:
            self.result = e if isinstance(e, Exception) else RuntimeError(str(e))
        finally:
            self.done.set()
            # 실패하면 예외가 capture_loop 를 끝내고 systemd 가 서비스를 다시 띄운다
            cam.restart(base[0], base[1], base[2], nbufs=base[3])


PHOTO = PhotoDesk()


def capture_loop(cam, stop_evt, fix_dht):
    fails = 0
    while not stop_evt.is_set():
        if PHOTO.want.is_set():
            try:
                PHOTO.shoot(cam, fix_dht)
            except OSError as e:
                sys.stderr.write("[capture] 해상도 복귀 실패: %s\n" % e)
                break
            continue
        try:
            jpg = cam.read_frame()
        except OSError as e:
            sys.stderr.write("[capture] ioctl 오류: %s\n" % e)
            break
        if jpg is None:
            fails += 1
            if fails % 10 == 0:
                sys.stderr.write("[capture] 프레임 타임아웃 %d회\n" % fails)
            continue
        fails = 0
        HUB.publish(fix_mjpeg(jpg) if fix_dht else jpg)
    sys.stderr.write("[capture] 종료\n")
    if not stop_evt.is_set():
        # 캡처가 죽었는데 HTTP 만 살아 있으면 멈춘 화면만 계속 나간다.
        # 프로세스를 끝내서 systemd(Restart=always) 가 카메라부터 다시 열게 한다.
        sys.stderr.flush()
        os._exit(1)


# ---------------------------------------------------------------- Audio

def find_audio_device():
    """UVC 카메라 내장 마이크의 ALSA 장치 이름을 찾는다."""
    if not shutil.which("arecord"):
        return None
    try:
        out = subprocess.run(["arecord", "-l"], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, timeout=5).stdout
        out = out.decode("utf-8", "replace")
    except Exception:
        return None
    fallback = None
    for line in out.splitlines():
        if not line.startswith("card "):
            continue
        try:
            cardno = int(line.split()[1].rstrip(":"))
        except (IndexError, ValueError):
            continue
        low = line.lower()
        if any(k in low for k in ("usb", "camera", "webcam", "uvc")):
            return "plughw:%d,0" % cardno
        if fallback is None:
            fallback = "plughw:%d,0" % cardno
    return fallback


def wav_header(rate=48000, ch=1, bits=16):
    """길이를 최대치로 적어둔 스트리밍용 WAV 헤더."""
    byte_rate = rate * ch * bits // 8
    return (b"RIFF" + struct.pack("<I", 0xFFFFFFFF - 8) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, ch, rate, byte_rate,
                        ch * bits // 8, bits) +
            b"data" + struct.pack("<I", 0xFFFFFFFF - 36))


# ---------------------------------------------------------------- Focus

class Focus:
    """자동초점 on/off 와 수동 초점 위치. 마지막 설정은 파일에 남겨
    서비스 재시작·재부팅 후에도 다시 적용한다."""

    def __init__(self, cam, path):
        self.cam, self.path = cam, path
        self.lock = threading.Lock()
        self.rng = cam.query_ctrl(V4L2_CID_FOCUS_ABSOLUTE)
        self.has_auto = cam.query_ctrl(V4L2_CID_FOCUS_AUTO) is not None

    @property
    def available(self):
        return self.rng is not None

    def state(self):
        if not self.available:
            return {"available": False}
        with self.lock:
            auto = bool(self.cam.get_ctrl(V4L2_CID_FOCUS_AUTO)) \
                if self.has_auto else False
            value = self.cam.get_ctrl(V4L2_CID_FOCUS_ABSOLUTE)
        st = dict(self.rng, available=True, auto=auto, value=value,
                  has_auto=self.has_auto)
        return st

    def apply(self, auto=None, value=None, save=True):
        if not self.available:
            raise ValueError("이 카메라는 초점 조절을 지원하지 않습니다")
        with self.lock:
            if value is not None:
                v = int(value)
                if not self.rng["min"] <= v <= self.rng["max"]:
                    raise ValueError("초점 값 범위: %d~%d"
                                     % (self.rng["min"], self.rng["max"]))
                auto = False        # 수동 값을 주면 자동초점은 끈다
            if auto is not None and self.has_auto:
                self.cam.set_ctrl(V4L2_CID_FOCUS_AUTO, 1 if auto else 0)
            if value is not None:
                self.cam.set_ctrl(V4L2_CID_FOCUS_ABSOLUTE, v)
        st = self.state()
        if save:
            self._save(st)
        return st

    def _save(self, st):
        try:
            tmp = self.path + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"auto": st["auto"], "value": st["value"]}, f)
            os.replace(tmp, self.path)
        except OSError as e:
            sys.stderr.write("[focus] 설정 저장 실패: %s\n" % e)

    def restore(self):
        if not self.available:
            return
        try:
            with open(self.path) as f:
                saved = json.load(f)
        except (OSError, ValueError):
            return
        try:
            if saved.get("auto", True):
                self.apply(auto=True, save=False)
            else:
                self.apply(value=saved["value"], save=False)
            sys.stderr.write("[focus] 저장된 설정 적용: %s\n" % saved)
        except (OSError, ValueError, KeyError) as e:
            sys.stderr.write("[focus] 저장된 설정 적용 실패: %s\n" % e)


FOCUS = None
RTC = None          # rtc.WebRTC (GStreamer 가 있을 때만)


def start_webrtc(bitrate, fps):
    """WebRTC 를 켠다. 필요한 패키지가 없으면 이유를 남기고 MJPEG 만 쓴다."""
    global RTC
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import rtc
    except Exception as e:          # ImportError, gi.require_version 의 ValueError
        STATE["webrtc_off"] = "GStreamer/PyGObject 없음 (%s)" % e
        return
    missing = rtc.required_elements()
    if missing:
        STATE["webrtc_off"] = "GStreamer 요소 없음: %s" % ", ".join(missing)
        return
    RTC = rtc.WebRTC(HUB, PHOTO, fps=fps, bitrate=bitrate)


# ---------------------------------------------------------------- HTTP

BOUNDARY = "rv1106frame"
PAGE = ""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "rv1106-uvc/1.0"

    def log_message(self, fmt, *args):
        if os.environ.get("UVC_STREAM_VERBOSE"):
            sys.stderr.write("[http] %s %s\n"
                             % (self.client_address[0], fmt % args))

    def _send(self, code, ctype, body, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            # 클라이언트가 먼저 끊음 (예: 앱 WebView 가 다운로드를 넘기며 연결을 닫음)
            self.close_connection = True

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            self._send(200, "text/html; charset=utf-8", PAGE.encode("utf-8"))
        elif path == "/stream.mjpg":
            self.stream_mjpeg()
        elif path == "/snapshot.jpg":
            self.snapshot()
        elif path == "/status.json":
            st = {k: v for k, v in STATE.items() if k != "audio_dev"}
            if FOCUS is not None and FOCUS.available:
                try:
                    f = FOCUS.state()
                    st["focus"] = {"auto": f["auto"], "value": f["value"]}
                except OSError:
                    pass
            st.update(fps=round(HUB.fps, 2), frames=HUB.seq,
                      clients=HUB.clients,
                      webrtc=RTC is not None,
                      webrtc_peers=RTC.count if RTC else 0,
                      uptime=round(time.monotonic() - HUB.started, 1))
            self._send(200, "application/json", json.dumps(st).encode())
        elif path == "/focus.json":
            self.focus(None)
        elif path.startswith("/photos/") and path.endswith(".jpg"):
            name = path[len("/photos/"):-4]
            jpg = PHOTO.get(name)
            if jpg is None:
                self._send(404, "text/plain; charset=utf-8", b"photo expired\n")
            else:
                self._send(200, "image/jpeg", jpg, {"Content-Disposition":
                           'attachment; filename="UsbCam_%s.jpg"' % name})
        elif path == "/audio.wav":
            self.stream_audio()
        elif path == "/favicon.ico":
            self._send(204, "image/x-icon", b"")
        else:
            self._send(404, "text/plain; charset=utf-8", b"404\n")

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if path == "/photo":
            self.photo()
            return
        if path == "/webrtc":
            self.webrtc()
            return
        if path != "/focus":
            self._send(404, "text/plain; charset=utf-8", b"404\n")
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
            req = json.loads(self.rfile.read(min(n, 4096)) or b"{}")
            if not isinstance(req, dict):
                raise ValueError
        except ValueError:
            self._json(400, {"error": "JSON 본문이 필요합니다"})
            return
        self.focus(req)

    def webrtc(self):
        """POST /webrtc: 본문 offer SDP -> answer SDP (ICE 후보 포함, LAN 전용)."""
        if RTC is None:
            self._send(503, "text/plain; charset=utf-8",
                       ("WebRTC 비활성: %s\n" % STATE.get("webrtc_off", "")).encode())
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
            if not 0 < n <= 64 * 1024:
                raise ValueError("offer SDP 가 필요합니다")
            offer = self.rfile.read(n).decode("utf-8")
            if not offer.startswith("v=0"):
                raise ValueError("offer SDP 가 아닙니다")
            answer = RTC.answer(offer)
        except ValueError as e:
            self._send(400, "text/plain; charset=utf-8", ("%s\n" % e).encode())
            return
        except Exception as e:
            sys.stderr.write("[webrtc] 협상 실패: %s\n" % e)
            self._send(500, "text/plain; charset=utf-8", ("%s\n" % e).encode())
            return
        self._send(200, "application/sdp", answer.encode("utf-8"))

    def photo(self):
        """POST /photo: 최대 해상도로 1장 찍고 다운로드 주소를 돌려준다.
        사진은 메모리에 최근 몇 장만 남는다 (GET /photos/<name>.jpg)."""
        if not PHOTO.size:
            self._json(503, {"error": "고해상도 촬영을 지원하지 않습니다"})
            return
        try:
            name, jpg = PHOTO.take()
        except Exception as e:
            self._json(500, {"error": str(e)})
            return
        self._json(200, {"url": "/photos/%s.jpg" % name,
                         "filename": "UsbCam_%s.jpg" % name,
                         "width": PHOTO.size[0], "height": PHOTO.size[1],
                         "bytes": len(jpg)})

    def _json(self, code, obj):
        self._send(code, "application/json", json.dumps(obj).encode())

    def focus(self, req):
        """GET: 현재 상태.  POST {"auto": bool} 또는 {"value": int}."""
        if FOCUS is None:
            self._json(503, {"available": False})
            return
        try:
            if req is None:
                st = FOCUS.state()
            else:
                st = FOCUS.apply(auto=req.get("auto"), value=req.get("value"))
        except ValueError as e:
            self._json(400, {"error": str(e)})
            return
        except OSError as e:
            self._json(500, {"error": "카메라 제어 실패: %s" % e})
            return
        self._json(200, st)

    def snapshot(self):
        frame, _ = HUB.get(-1, timeout=5.0)
        if frame is None:
            self._send(503, "text/plain; charset=utf-8", b"no frame yet\n")
            return
        self._send(200, "image/jpeg", frame,
                   {"Content-Disposition": 'inline; filename="snapshot.jpg"'})

    def stream_mjpeg(self):
        self.send_response(200)
        self.send_header("Content-Type",
                         "multipart/x-mixed-replace; boundary=%s" % BOUNDARY)
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Connection", "close")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        HUB.clients += 1
        last = -1
        try:
            while True:
                frame, last = HUB.get(last, timeout=10.0)
                if frame is None:
                    continue
                self.wfile.write(
                    ("--%s\r\nContent-Type: image/jpeg\r\n"
                     "Content-Length: %d\r\n\r\n"
                     % (BOUNDARY, len(frame))).encode("ascii"))
                self.wfile.write(frame)
                self.wfile.write(b"\r\n")
        except (BrokenPipeError, ConnectionResetError, socket.timeout, OSError):
            pass
        finally:
            HUB.clients = max(0, HUB.clients - 1)

    def stream_audio(self):
        dev = STATE.get("audio_dev")
        if not dev:
            self._send(503, "text/plain; charset=utf-8",
                       "마이크를 찾지 못했습니다 (alsa-utils 의 arecord 필요)\n"
                       .encode("utf-8"))
            return
        rate = STATE.get("audio_rate", 48000)
        try:
            proc = subprocess.Popen(
                ["arecord", "-D", dev, "-f", "S16_LE", "-c", "1",
                 "-r", str(rate), "-t", "raw", "-q"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        except OSError as e:
            self._send(500, "text/plain; charset=utf-8", str(e).encode("utf-8"))
            return
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(wav_header(rate, 1, 16))
            while True:
                chunk = proc.stdout.read(4096)
                if not chunk:
                    break
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            proc.kill()
            proc.wait()


def load_page():
    global PAGE
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        with open(os.path.join(here, "www", "index.html"),
                  "r", encoding="utf-8") as f:
            PAGE = f.read()
    except OSError:
        PAGE = ("<!doctype html><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>RV1106 USB Camera</title>"
                "<body style='margin:0;background:#111'>"
                "<img src='/stream.mjpg' style='width:100%;height:auto'>")


# ---------------------------------------------------------------- main

def local_ips():
    ips = []
    try:
        out = subprocess.run(["ip", "-4", "-o", "addr"],
                             stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, timeout=3).stdout
        for line in out.decode("utf-8", "replace").splitlines():
            parts = line.split()
            if len(parts) > 3 and parts[2] == "inet":
                ip = parts[3].split("/")[0]
                if not ip.startswith("127."):
                    ips.append(ip)
    except Exception:
        pass
    if not ips:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ips.append(s.getsockname()[0])
            s.close()
        except Exception:
            pass
    return ips


def _is_uvc(dev):
    # /sys/class/video4linux/videoN/device/driver -> .../uvcvideo
    name = os.path.basename(dev)
    try:
        drv = os.readlink("/sys/class/video4linux/%s/device/driver" % name)
    except OSError:
        return False
    return os.path.basename(drv) == "uvcvideo"


def video_nodes():
    # 심볼릭 링크(/dev/video-camera0 등)는 중복이라 제외하고,
    # RV1106 처럼 ISP/CIF 노드가 많은 보드에서 UVC 카메라를 먼저 시도한다.
    try:
        devs = [("/dev/" + d) for d in os.listdir("/dev")
                if d.startswith("video") and not os.path.islink("/dev/" + d)]
    except OSError:
        return []
    num = lambda d: int("".join(c for c in d if c.isdigit()) or 0)
    return sorted(devs, key=lambda d: (not _is_uvc(d), num(d)))


def pick_device(explicit):
    if explicit:
        return explicit
    for dev in video_nodes():
        cam = Camera(dev, 640, 480, 15)
        try:
            cam.open()
            fmts = [p for p, _ in cam.list_formats()]
        except Exception:
            continue
        finally:
            cam.stop()
        if V4L2_PIX_FMT_MJPEG in fmts or V4L2_PIX_FMT_JPEG in fmts:
            return dev
    return None


def cmd_list(device):
    devs = [device] if device else video_nodes()
    if not devs:
        print("video 노드가 없습니다. 카메라 연결과 uvcvideo 드라이버를 확인하세요.")
        return 1
    for dev in devs:
        print("=" * 64)
        print(dev)
        cam = Camera(dev, 640, 480, 15)
        try:
            cam.open()
        except Exception as e:
            print("  열 수 없음: %s" % e)
            continue
        try:
            print("  name : %s" % cam.card)
            for pf, desc in cam.list_formats():
                sizes = cam.list_sizes(pf)
                print("  %-5s %-26s %s"
                      % (fourcc_str(pf), desc,
                         " ".join("%dx%d" % s for s in sizes[:14])
                         or "(해상도 열거 불가)"))
        finally:
            cam.stop()
    return 0


def main(argv=None):
    import argparse
    p = argparse.ArgumentParser(
        description="UVC MJPEG 카메라를 HTTP(MJPEG)로 스트리밍한다.")
    p.add_argument("-d", "--device", help="예: /dev/video0 (생략 시 자동 탐색)")
    p.add_argument("-W", "--width", type=int, default=1280)
    p.add_argument("-H", "--height", type=int, default=720)
    p.add_argument("-f", "--fps", type=int, default=30)
    p.add_argument("-p", "--port", type=int, default=8090)   # 8080 은 RvCam 프로비저닝 API
    p.add_argument("-b", "--bind", default="0.0.0.0")
    p.add_argument("--buffers", type=int, default=4)
    p.add_argument("--no-audio", action="store_true",
                   help="/audio.wav 엔드포인트 비활성")
    p.add_argument("--audio-rate", type=int, default=48000)
    p.add_argument("--no-dht-fix", action="store_true",
                   help="MJPEG Huffman 테이블 자동 삽입 끄기")
    p.add_argument("--no-webrtc", action="store_true",
                   help="WebRTC(H.264) 송출 끄기 (MJPEG 만)")
    p.add_argument("--webrtc-bps", type=int, default=6000000,
                   help="WebRTC H.264 목표 비트레이트 (기본 6 Mbps)")
    p.add_argument("--photo-max", default="2592x1944",
                   help="사진 최대 해상도 WxH (RV1106 램이 작아 16MP 는 기본 제한, 0 이면 제한 없음)")
    p.add_argument("--list", action="store_true",
                   help="카메라 지원 포맷만 출력하고 종료")
    args = p.parse_args(argv)

    if args.list:
        return cmd_list(args.device)

    dev = pick_device(args.device)
    if dev is None:
        sys.stderr.write(
            "MJPEG 를 지원하는 카메라를 찾지 못했습니다.\n"
            "  1) lsusb              - 카메라가 보이는지\n"
            "  2) ls /dev/video*     - 노드가 생겼는지\n"
            "  3) %s --list  - 지원 포맷 확인\n" % sys.argv[0])
        return 2

    cam = Camera(dev, args.width, args.height, args.fps,
                 nbufs=args.buffers).open().configure().start()

    STATE.update(device=dev, card=cam.card, width=cam.width, height=cam.height,
                 pixfmt=fourcc_str(cam.pixelformat),
                 target_fps=round(cam.fps, 2), port=args.port)
    if not args.no_audio:
        adev = find_audio_device()
        STATE.update(audio_dev=adev, audio=bool(adev),
                     audio_rate=args.audio_rate)

    PHOTO.size = cam.max_size()
    if PHOTO.size and args.photo_max not in ("", "0"):
        mw, mh = (int(v) for v in args.photo_max.lower().split("x"))
        fits = [s for s in cam.list_sizes(cam.pixelformat)
                if s[0] * s[1] <= mw * mh]
        PHOTO.size = max(fits, key=lambda s: s[0] * s[1]) if fits else None
    if PHOTO.size and PHOTO.size[0] * PHOTO.size[1] <= cam.width * cam.height:
        PHOTO.size = None                       # 이미 최대 해상도로 스트리밍 중
    STATE.update(photo="%dx%d" % PHOTO.size if PHOTO.size else None)

    global FOCUS
    FOCUS = Focus(cam, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "focus.json"))
    FOCUS.restore()

    if args.no_webrtc:
        STATE["webrtc_off"] = "--no-webrtc"
    else:
        start_webrtc(args.webrtc_bps, cam.fps)

    load_page()

    stop_evt = threading.Event()
    threading.Thread(target=capture_loop,
                     args=(cam, stop_evt, not args.no_dht_fix),
                     daemon=True).start()

    httpd = ThreadingHTTPServer((args.bind, args.port), Handler)
    httpd.daemon_threads = True

    print("-" * 58)
    print(" camera : %s (%s)" % (dev, cam.card))
    print(" format : %s %dx%d @ %s fps"
          % (fourcc_str(cam.pixelformat), cam.width, cam.height,
             STATE["target_fps"]))
    print(" audio  : %s" % (STATE.get("audio_dev") or "없음"))
    print(" focus  : %s" % ("%(min)d~%(max)d" % FOCUS.rng if FOCUS.available
                            else "조절 불가"))
    print(" webrtc : %s" % ("H.264 %.1f Mbps" % (args.webrtc_bps / 1e6) if RTC
                            else "꺼짐 - %s" % STATE.get("webrtc_off")))
    print("-" * 58)
    for ip in local_ips():
        print("  패드에서 접속 ->  http://%s:%d/" % (ip, args.port))
    print("-" * 58)
    sys.stdout.flush()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n종료합니다.")
    finally:
        stop_evt.set()
        httpd.shutdown()
        cam.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
