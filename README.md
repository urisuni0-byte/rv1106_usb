# TGO-ABC 두피 스캐너 (RV1106)

Luckfox Pico Ultra W(**RV1106G3**, Wi-Fi + BT 내장 AIC8800)와 USB 카메라(Arducam B0290 IMX298)로 만든
두피 스캐너 장치 프로그램과 Android 앱입니다. 연동 방식은
**「TGO-ABC 전용 무선 디바이스 연동 시나리오 20260812」** 문서를 따릅니다.

| 인터페이스 | 방향 | 프로토콜 | 내용 |
|---|---|---|---|
| 초기 등록 / Wi-Fi 설정 | App ↔ 장치 | BLE (GATT) | 장치 검색, Device Info, Wi-Fi 프로비저닝 |
| 자동 재연결 | App → 장치 | mDNS / DNS-SD | `_tgoabc._tcp` 로 같은 LAN 의 등록 장치 검색 |
| 실시간 영상 | 장치 → App | WebRTC Media / H.264 | RV1106 하드웨어 인코더, Baseline |
| 제어 / 상태 | App ↔ 장치 | WebRTC DataChannel `control` | 미리보기 시작/종료, 촬영 준비·시작·완료·실패 |
| 상태 / 시그널링 / 이미지 | App ↔ 장치 | HTTP :8090 | 장치 상태, SDP/ICE 교환, JPEG 다운로드 |

## 폴더

| 경로 | 내용 |
|---|---|
| `board/scannerd/` | 장치 데몬 (Go): BLE GATT, mDNS, HTTP API, WebRTC(pion), 촬영 순서, GPIO/센서 |
| `board/scannerd/web/viewer.html` | 미리보기·촬영 화면. 장치가 `/` 로 주고, 앱에도 같은 파일이 들어간다 |
| `board/camhelper/` | 카메라 도우미 (C, 상주): 미리보기 YUYV → rockit VENC H.264 / 사진 MJPEG + 촬영 중 미리보기 (MJPEG 디코딩 → 축소 → H.264) |
| `board/scanner.conf` | 장치 설정 (해상도, GPIO 번호, 센서, BLE/mDNS) |
| `board/S96scanner`, `board/install.sh` | init 스크립트 / 보드에서 `/opt/scanner` 설치 |
| `board/usbmode.sh`, `board/S01bootlog` | USB host 전환, 부팅 로그 |
| `android/` | 앱 `com.rv1106.usbcam` "TGO-ABC 스캐너" (Gradle 없이 `build.sh`) |
| `tools/build_board.sh` | WSL 로 `scannerd` + `camhelper` 빌드 → `board/out/` |
| `tools/deploy.sh` | PC → 보드 설치 |
| `tools/fakecam/` | 카메라 없이 PC/패드에서 시험할 때 쓰는 가짜 camhelper (`make_media.sh` 로 시험 영상 생성) |
| `vdec_test/`, `backup/` | 예전 JPEG 디코딩 시험 / 부팅 파티션 백업과 Maskrom 복구 도구 |

## 장치 쪽 동작

```
 App ──BLE──> scannerd(ble.go) ── /userdata/rvcam/sta.* 저장 → rvcam-net.sh sta-restart → IP 응답(notify)
 App ──mDNS─> scannerd(mdns.go)  TGOABC-xxxx._tgoabc._tcp.local :8090  TXT id/name/model/fw
 App ──HTTP─> POST /api/webrtc/offer  → answer(SDP, ICE 포함) / POST /api/webrtc/candidate
 App <─WebRTC─ H.264 ← camhelper preview (YUYV 640x480@20 → VENC)   미리보기 요청이 있을 때만 카메라 사용
 버튼 ─> 촬영: 사진 모드(MJPEG 2592x1944, 미리보기는 MJPEG 디코딩으로 약 5fps 유지) → 일반광 → UV광 → 온·습도 → 저장 → 미리보기 복귀
         DataChannel: capture.state started(step white/uv/climate/save) → completed(result) → ready
 App ──HTTP─> GET /api/captures/{id}/white.jpg, uv.jpg, /bundle (multipart: JSON + JPEG 2장)
```

- Wi-Fi 는 보드에 이미 있는 `rvcam-net.sh` 를 그대로 쓴다. AP(`RvCam-xxxx`, 192.168.4.1)는 항상 떠 있고,
  STA 는 `/userdata/rvcam/sta.ssid|sta.psk` 로 붙는다.
- BLE 는 HCI user channel 로 `hci0` 를 직접 연다 (bluetoothd 는 끈다).
- 조명(일반광/UV LED), 버튼, 온·습도 센서는 `scanner.conf` 에서 GPIO/I2C 를 지정한다. 지정하지 않으면
  시뮬레이션이다. 버튼은 `POST /api/button` 이나 DataChannel `button.press` 로 대신 누를 수 있다
  (뷰어의 [촬영] 버튼이 이것).

### GATT (서비스 `e5a10001-7b4c-4c2d-9f3e-5447f041bc00`)

| UUID | 속성 | 값 |
|---|---|---|
| `e5a10002-…` | read | Device Info `{"name","id","model","fw","mac","port"}` |
| `e5a10003-…` | write | Wi-Fi Provisioning `{"ssid","password"}` |
| `e5a10004-…` | read, notify | 상태 `{"state":"idle|connecting|connected|failed","ssid","ip","port","name","id","error"}` |

### HTTP API (:8090)

| | |
|---|---|
| `GET /api/device`, `GET /api/status` | 장치 정보 / 상태 (Wi-Fi, 미리보기, 촬영, BLE) |
| `POST /api/webrtc/offer` `{"sdp"}` | → `{"session_id","sdp"}` (answer) |
| `POST /api/webrtc/candidate` `{"session_id","candidate"}` | 앱 ICE 후보 |
| `GET /api/webrtc/candidates?session_id=` / `DELETE /api/webrtc/session?session_id=` | 장치 ICE 후보 / 세션 종료 |
| `POST /api/button`, `POST /api/capture` | 버튼 시뮬레이션 / 앱 촬영 |
| `GET /api/captures`, `/api/captures/{id|latest}`, `/{id}/white.jpg`, `/{id}/uv.jpg`, `/{id}/bundle` | 촬영 결과 |
| `GET /api/camera/controls` | 카메라 컨트롤 목록과 현재 값 (V4L2: 초점, 노출, 화이트밸런스, 밝기 …) |
| `POST /api/camera/controls` `{"id","value"}`, `POST /api/camera/controls/reset` | 값 바꾸기(장치에 저장) / 모두 기본값 |
| `GET /api/climate`, `POST /api/light` `{"mode":"off|white|uv"}` | 점검용 |

### DataChannel `control` (JSON)

앱 → 장치: `preview.start`, `preview.stop`, `button.press`, `capture.start`, `status.get`,
`climate.read`, `light.set {"white"|"uv":true}`,
`camera.controls.get`, `camera.control.set {"id","value"}`, `camera.controls.reset`,
`focus.start`, `focus.stop`
(모두 `req` 번호를 붙이면 응답에 `re` 로 돌아온다)

장치 → 앱: `hello`, `preview.state {state: started|stopped|failed, info}`,
`capture.state {state: ready|started|completed|failed, step, capture_id, result}`,
`camera.controls {ok, error, controls:[{id,name,type:int|bool|menu,min,max,step,default,value,inactive,menu}]}`,
`focus.state {on}`, `focus {score, brightness, frames, source: preview|photo, roi}` (focus.start 한 세션에만, 약 5번/초)

### 카메라 설정

뷰어의 [카메라] 버튼이 설정 패널을 연다. 장치가 카메라의 V4L2 컨트롤 목록을 그대로 주고, 화면은 아는 컨트롤을
초점 / 노출 / 화이트밸런스 / 화질로 묶어 한글 이름으로 보여 준다 (모르는 것은 '기타'). 자동 모드가 켜져 있으면
해당 수동 값(초점 위치, 노출 시간, 색온도)은 잠긴다. 바꾼 값은 `/userdata/scanner/camera_controls.json` 에
바꾼 순서대로 저장되고, camhelper 가 새로 뜨거나 카메라가 USB 에서 다시 잡힐 때 다시 적용된다.
camhelper 는 카메라가 끊겼다 다시 잡혀 노드 번호가 바뀌어도(video0 -> video1) 다음 모드 전환 때 다시 찾아 연다.

### 초점 맞춤 보조 (M12 수동 초점 렌즈용)

뷰어 위쪽 [초점] 버튼. camhelper 가 H.264 로 압축하기 전의 미리보기 Y 평면에서 가운데 1/2 x 1/2 영역
(화면의 점선 상자)의 라플라시안 분산을 재서 200ms 평균을 보낸다. 값은 장면·조명에 따라 달라지므로 화면은
지금까지의 최고값 대비 비율(막대, 최근 15초 그래프)로 보여 주고, 최고값의 95% 이상이면 초록이 된다.
렌즈를 천천히 한쪽으로 돌려 최고점을 지나친 뒤 되돌아와 초록에서 멈추면 된다. 대상이나 조명을 바꾸면
[최고값 지우기]. 너무 어둡거나(밝기 < 35) 포화되면(> 225) 경고한다. 켜 둔 앱이 없으면 측정하지 않는다.

## 빌드 / 설치

```bash
bash tools/build_board.sh       # WSL: ~/sdk/go, ~/lfx/luckfox-pico(툴체인+rockit 헤더), ~/lfx/lnk/librockit_full.so
bash tools/deploy.sh pad        # 패드 경유 (보드 host 모드). USB adb 면 인자 없이
bash android/build.sh && adb -s <패드> install -r -g android/out/RV1106USB.apk
```

보드 로그: `/tmp/scanner.log`, 설정: `/opt/scanner/scanner.conf` (`/etc/init.d/S96scanner restart`),
촬영: `/userdata/scanner/<id>/`. 카메라 포맷 확인: `LD_LIBRARY_PATH=/oem/usr/lib /opt/scanner/camhelper list`.

카메라 없이 시험: `tools/fakecam` 을 빌드하고 `make_media.sh` 로 영상을 만든 뒤 `scanner.conf` 에
`CAMHELPER=<fakecam 경로>`, `BLE=0`, 환경변수 `FAKECAM_DIR=<영상 디렉터리>`.

## 확인한 것 (2026-10-13)

- PC(WSL) + 가짜 카메라: Chrome 에서 WebRTC H.264 재생, 버튼 촬영 단계 표시, 결과(일반광/UV/온습도),
  번들 다운로드, 촬영 후 미리보기 복귀.
- 패드(Android 9) Chrome: 같은 흐름 동작.
- 실제 보드: scannerd 기동, BLE 광고 → 앱에서 검색·연결·Device Info 읽기.
  `camhelper list` 로 카메라 포맷 확인 (YUYV 640x480@20).
- 실제 보드 (공유기 STA 172.30.1.x): 앱 BLE 프로비저닝, 하드웨어 H.264 미리보기 앱 재생, 실카메라 촬영 (2592x1944).
- 촬영 중 미리보기 (PC Chrome 측정): 평소 15fps → 촬영 중 약 5fps 유지, 촬영 3.5초.
  카메라 모드 전환 순간에만 1.6초 / 0.9초 멈춘다 (UVC 카메라는 스트림이 하나라 해상도를 바꿀 때 재시작이 필요).
- 첫 VENC 시험 직후 보드 AP 가 사라진 적이 있다 (BLE 는 계속 동작) — 전원 문제인지 확인 필요.

## USB host 모드 (카메라 사용에 필수)

DT 에 `dr_mode = "peripheral"` 이 고정돼 있어 부팅 파티션 DTB 를 바꾸고 재부팅해야 한다.

```bash
adb shell bash /opt/scanner/usbmode.sh host && adb shell reboot
```

- host 모드에서는 USB adb 가 없다. 보드 AP(192.168.4.1)로 `adb connect` 하거나 `deploy.sh pad` 를 쓴다.
- 카메라 5V 는 외부에서 넣어야 한다 (보드 5V 가 VBUS 로 나가지 않음). 5V 2A 이상, GND 공통.
- 되돌리기: `usbmode.sh peripheral && reboot`. 원본 그대로: `dd if=/userdata/boot_backup.img of=/dev/mmcblk0p4`.

### Maskrom 복구

BOOT 버튼을 누른 채 PC USB 연결 → `Rockusb Device (2207:110C)`:

```bash
cd backup/tools
./upgrade_tool.exe db rv1106_download_v1.15.108.bin
./upgrade_tool.exe wl 0x640 ../boot_mmcblk0p4.img
./upgrade_tool.exe rd
```
