# RV1106 USB 카메라 → 패드 Wi-Fi 영상 보기

Luckfox Pico **RV1106G3** 보드(Buildroot, uClibc, 램 ~170MB)에 연결한
**Arducam B0290 (IMX298 16MP AF, UVC USB2.0)** 영상을 패드 앱 **RV1106 USB** 로 보는 구성입니다.
`rv1126_usb`(RV1126 + 같은 카메라 + AuraCam 앱)를 RV1106 으로 옮긴 것입니다.

```
[B0290 USB 카메라] --USB(host)--> [RV1106 보드] --Wi-Fi AP RvCam-xxxx--> [패드 앱 RV1106 USB]
                                  uvc_stream.py         192.168.4.1:8090
```

카메라가 MJPEG 로 압축해서 보내므로 보드는 **디코딩/인코딩 없이 그대로** HTTP(multipart MJPEG)로 넘깁니다.
RV1106 에는 하드웨어 JPEG 디코더가 없고 소프트웨어 디코딩은 1080p 에서 50~100ms/프레임이라,
디코딩하지 않는 이 방식이 이 보드에 맞습니다.

## 폴더

| 경로 | 내용 |
|---|---|
| `board/uvc_stream.py` | V4L2(ctypes) MJPEG 캡처 → HTTP 서버 (표준 라이브러리만, 보드 Python 3.11) |
| `board/start.sh`, `board/stream.conf` | 실행 스크립트 / 해상도·포트 설정 (기본 1280x720@30, 포트 8090) |
| `board/S95uvcstream` | init 스크립트. 카메라가 없거나 죽으면 5초마다 다시 시도 |
| `board/install.sh` | 보드에서 `/opt/uvc-stream` 에 설치 + 자동 실행 등록 |
| `board/usbmode.sh` | USB `dr_mode` 를 host/peripheral 로 바꿈 (부팅 파티션 DTB 수정, 재부팅 필요) |
| `tools/deploy.sh` | PC → 보드 설치 (USB adb 또는 Wi-Fi adb) |
| `android/` | 패드 앱 `com.rv1106.usbcam` (Gradle 없이 `build.sh` 로 빌드) |
| `vdec_test/` | Rockit VDEC JPEG 디코딩 테스트 (소프트웨어 디코딩 확인용) |
| `backup/` | 원본 부팅 파티션 백업 + Maskrom 복구 도구 |

## 포트

| 포트 | 서비스 |
|---|---|
| 8090 | **uvc_stream** (`/`, `/stream.mjpg`, `/snapshot.jpg`, `/status.json`, `/photo`, `/focus`) |
| 8080 | RvCam 프로비저닝 API (`rvcam-provd.py`, 기존) |
| 5555 / 22 | adbd TCP / sshd |

## 설치

```bash
# 1) 보드 소프트웨어 설치/업데이트
bash tools/deploy.sh            # 보드가 peripheral 모드 (USB adb)
bash tools/deploy.sh pad        # 보드가 host 모드: 패드(USB adb + RvCam Wi-Fi)를 중계로 사용
bash tools/deploy.sh <보드IP>   # PC 와 보드가 같은 공유기
CONF=1 bash tools/deploy.sh pad # board/stream.conf 로 보드 설정까지 덮어쓰기 (기본은 보드 설정 유지)

# 2) 패드 앱
bash android/build.sh
adb -s <패드 시리얼> install -r -g android/out/RV1106USB.apk
```

## USB host 모드 (카메라 사용에 필수)

이 보드는 USB-C OTG 포트 하나뿐이고 DT 에 `dr_mode = "peripheral"` 이 고정돼 있어서
런타임 전환(PHY otg_mode, dwc3 debugfs)은 **무시됩니다**. 부팅 파티션 DTB 를 바꾸고 재부팅해야 합니다.

```bash
adb shell bash /opt/uvc-stream/usbmode.sh host && adb shell reboot
```

- host 모드에서는 **USB adb / USB 네트워크(usb0) 가 없습니다**. 보드 Wi-Fi(AP 192.168.4.1 또는 공유기 STA)로
  `adb connect <보드IP>:5555` / ssh 로 접속하세요.
- 보드는 USB 포트로 카메라에 5V 를 줘야 하므로 **보드 전원은 따로** 넣어야 합니다 (카메라 최대 200mA).
- 되돌리기: `bash /opt/uvc-stream/usbmode.sh peripheral && reboot`.
  host↔peripheral 왕복 후 DTB 가 원본과 바이트 단위로 달라지므로, 완전히 원래대로 하려면
  `dd if=/userdata/boot_backup.img of=/dev/mmcblk0p4` (md5 `c807ab96a0c82b963e8a1c597e51f48b`).

### 접속이 전부 끊겼을 때 (Maskrom 복구)

BOOT 버튼을 누른 채 PC USB 연결 → `Rockusb Device (2207:110C)`:

```bash
cd backup/tools
./upgrade_tool.exe db rv1106_download_v1.15.108.bin   # 로더를 RAM 에만 올림
./upgrade_tool.exe wl 0x640 ../boot_mmcblk0p4.img      # boot 파티션(LBA 0x640) 원본 복원
./upgrade_tool.exe rd
```

## 보드 변경 사항

- `/oem/usr/bin/RkLunch.sh`: MIPI 센서(`m00_*` subdev)가 없으면 rkipc 를 띄우지 않음
  (센서 없이 실행하면 시작 직후 segfault). 원본은 `RkLunch.sh.orig`.
  MIPI 카메라를 다시 꽂으면 원래대로 rkipc 가 뜹니다.
- `/etc/init.d/S95uvcstream` 추가.
- `/etc/init.d/S99zuvctest`(VDEC 테스트용 자동 복구)는 host 모드 상시 운용과 충돌해서 제거함
- `/etc/init.d/S01bootlog` 추가: `/userdata/bootlog/boot_NNNN.log` 에 2초마다 uptime·메모리·SoC 온도·새 커널 로그 기록
  (최근 5회 부팅). 보드가 갑자기 꺼지면 이전 부팅 파일의 마지막 줄을 본다.

## 전원 주의

- 보드 5V 가 USB-C VBUS 로 나가지 않는다 (DT 에 vbus-supply 없음). 카메라 5V 는 외부에서 넣어야 한다.
- host 모드에서 `rv1106_npor_powergood_isr voltage jitter detected` 직후 카메라가 USB 에서 끊겼다가 다시 붙는 것이 관측됨.
  보드가 갑자기 꺼지는 것도 같은 원인으로 보인다 → 5V 2A 이상 전원, 짧고 굵은 배선, 보드와 카메라 GND 공통.

