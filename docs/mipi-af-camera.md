# MIPI 자동초점 카메라 검토 (2026-10-13)

USB 카메라(Arducam B0290, IMX298)를 MIPI 카메라와 자체 AF로 바꾸는 검토 기록이다.

## 왜 MIPI인가

현재 USB 카메라 구성의 한계:
- RV1106에는 하드웨어 JPEG 디코더가 없다. 그래서 MJPEG 1080p30을 실시간으로 디코딩할 수 없다.
- YUYV는 USB2 대역 때문에 640x480@20이 한계다. 실시간 영상이 흐린 원인이 이것이다.

MIPI 센서로 바꾸면:
- ISP가 직접 처리하므로 디코딩이 필요 없다.
- 1080p H.264 실시간과 원본 해상도 하드웨어 JPEG 스냅샷을 동시에 낼 수 있어, 촬영 중에도 영상이 끊기지 않는다.

## 요구 조건

- 렌즈에서 두피까지: 약 15mm
- 촬영 영역: 6 x 6mm
- "10배 확대"는 화면 배율이다. 6mm가 화면에서 60mm로 보이면 10배다.
  - 9.7" 패드에서 6mm를 화면 세로에 가득 채우면 약 25배가 된다.

## 광학 계산 (SC4336, 단순 렌즈 근사)

| 항목 | 값 |
|---|---|
| 센서 | 5.12 x 2.88mm, 2.0µm 화소 |
| 배율 | 0.48배 (세로 2.88mm에 6mm를 담음) |
| 실제 찍히는 영역 | 10.7 x 6mm (6mm 정사각형에는 1440 x 1440화소) |
| 해상도 | 약 4.2µm/화소 |
| 렌즈 초점거리 | 약 4.9mm (일반 표기 대각 화각 약 62°) |
| 매크로 위치까지 렌즈를 밀어내야 하는 거리 | 약 2.3mm. VCM으로는 불가하므로 조립 시 기계적으로 고정한다 |
| VCM 0.3mm로 맞출 수 있는 범위 | 두피 쪽 거리 약 ±0.65mm (VCM 0.5mm면 약 ±1mm) |
| 피사계 심도 | f/2.4에서 약 0.12mm, f/8에서 약 0.4mm. AF가 반드시 필요하다 |

센서가 작을수록 배율이 낮아져, AF 범위와 피사계 심도가 넓어진다.

## RV1106 (Luckfox Pico Ultra)에서 확인한 것

**보드의 IQ 파일** (`/etc/iqfiles`):
- 센서: sc3336, sc4336, sc530ai, sc500ai, sc501ai, sc401ai, sc200ai, sc230ai, sc231hai, sc301iot, sc3338, sc031gs, gc1084, gc2053, gc2093, gc4023, gc4653, imx415, jx_k17, mis2032, mis5001, os02h10, os02k10, os04a10
- 모두 고정 초점 모듈용 튜닝이다. AF와 vcmcfg 항목은 빈 기본값이다.

**보드 이미지의 드라이버:**
- 센서 드라이버는 sc3336, mis5001 두 개뿐이다. 다른 센서는 SDK에서 모듈을 빌드해야 한다.
- VCM 드라이버는 로드되어 있지 않다.

**Luckfox 커널 소스:**
- ISP(V32_L) 입력 한계는 4224 x 3136이다.
- Pico Ultra 카메라 단자는 2-lane이다 (`data-lanes = <1 2>`, i2c4).
- IMX415 드라이버(`imx415.c` v0.1.8)는 `data-lanes` 개수로 모드 표를 고른다.
  - 2-lane: 3864x2192@15 (12/10bit), 1284x720@90
  - 4-lane: 3864x2192@30 (HDR 포함), 1944x1097@30
- Rockchip `rv1106-evb-cam.dtsi`에 IMX415가 2-lane, I2C 0x1a, 모듈명 `CMK-OT2022-PX1`로 연결되어 있다. 보드의 IQ 파일과 같은 조합이다.
- 켤 수 있는 VCM 드라이버: DW9714, DW9763, FP5510, CN3927V

**아직 확인하지 못한 것:**
- JPEG 인코더가 3864x2192를 한 번에 인코딩할 수 있는지
- H.264 인코더는 최대 3072x1728이라 1080p 실시간 영상은 문제없다.

## SG2002 (LicheeRV Nano)에서 확인한 것

**드라이버** (`sipeed/LicheeRV-Nano-Build`, `middleware/v2/component/isp/sensor/cv182x`):
- 소니: imx307, imx327, imx335. imx415는 없다.
- 그 밖에: sc4336, sc500ai, sc501ai, sc531ai_2L, sc5336_2L, sc3336, os04a10, os04c10, os08a20, gc4653, ov5647 등

**Sipeed 카메라와 하드웨어:**
- Sipeed 공식 카메라는 GC4653, OS04A10, SC035HGS로 모두 고정 초점이다.
- 카메라 단자는 Sipeed 고유 핀 배치의 22핀 4-lane이다.
- ISP는 AF용 선명도 통계만 준다. 렌즈 모터 제어와 AF 알고리즘은 직접 만들어야 한다.

## 센서 후보

| 센서 | 해상도 | RV1106 | SG2002 | 비고 |
|---|---|---|---|---|
| **SC4336** | 2560x1440, 1/3.06" | 드라이버(SDK)와 IQ 있음 | 드라이버 있음 | **추천.** 작은 센서라 매크로에 유리하고, 두 플랫폼에서 공용으로 쓸 수 있다 |
| IMX415 | 3864x2192, 1/2.8" | 드라이버와 IQ 있음 | 없음 | 기성 AF 모듈이 있다 (Waveshare AF, Khadas: DW9714) |
| IMX335 | 2592x1944 (4:3), 1/2.8" | IQ 없음 | 드라이버 있음 | 6mm 정사각형에 약 1944화소가 들어간다 |
| OV5647 | 2592x1944, 1/4" | - | 드라이버 있음 | 파이 카메라용 AF 모듈이 흔하다. 화질은 떨어진다 |
| OS04A10 | 2688x1520, 1/1.79" | IQ 있음 | 드라이버 있음 | 센서가 커서 근접 AF 범위가 좁다 |

## 모듈 업체 요청 사양 (안)

- 센서: SC4336 (대안 GC4653), MIPI 2-lane
- 렌즈: 초점거리(EFL) 약 4.9mm, f/2.4~2.8
- 작업 거리: 렌즈 앞 15mm에 초점, 세로 6mm가 화면에 가득 차는 매크로 조립 (배율 약 0.48배)
- VCM: 이동 거리 0.4mm 이상, 드라이버 칩과 I2C 주소 명시 (예: DW9714)
- 참고: 피부·두피 확대경(dermatoscope, trichoscope) 모듈과 같은 구조라고 설명하면 이해가 빠르다

## AF 방식

- rkaiq 내장 AF는 쓰지 않는다. AF 튜닝과 VCM 드라이버가 필요하기 때문이다.
- 대신 사용자 공간에서 콘트라스트 AF를 직접 만든다.
  1. ISP의 작은 출력에서 가운데 Y 영역의 선명도를 계산한다.
  2. `/dev/i2c`로 VCM을 직접 구동한다.
  3. 거친 탐색 후 세밀하게 탐색한다.
  4. 평소에는 선명도가 떨어질 때만 다시 초점을 찾는다.
- RV1106과 SG2002에서 같은 코드를 쓴다.
- 하우징이 거리를 대략 고정하고, AF는 두피 굴곡과 머리카락 높이만 보정한다.
- 조명: 렌즈 둘레에 링 LED를 둔다. 반사가 심하면 편광 필터를 검토한다.

## 다음 단계

1. 모듈 업체 문의서(영문·중문) 작성
2. 선택한 센서와 VCM용 RV1106 dts, 커널 설정 준비
3. SG2002가 도착하면 후보 센서용 ISP 튜닝 파일이 있는지 확인

## IMX219 AF 진행 (2026-10-16)

현재 보드(Luckfox Pico Ultra W, MIPI0 2-lane)에 맞춰 IMX219 AF 모듈로 진행하기로 했다.
- IMX219는 원래 2-lane이라 단자와 그대로 맞고, 기존 B0292(UVC)와 센서가 같다.
- SDK의 `imx219.c`는 메인라인 버전(rkmodule 없음)이라, sc3336 구조를 따른 Rockchip용 드라이버를 새로 만들었다: `board/drivers/imx219/`.
  - 모드: 3280x2464@15, 1640x1232@30(2x2 binning, 전체 화각), 1920x1080@30(가운데 자름). RAW10 RGGB, 912Mbps/lane.
  - 게인 컨트롤은 선형 x256(256=1배). 10.67배까지 아날로그, 그 위는 디지털.
  - `bash board/drivers/imx219/build.sh`(WSL) → `board/out/imx219.ko`, `board/out/imx219.dtbo`
- DT는 boot 파티션을 다시 쓰지 않고 configfs 런타임 오버레이로 추가한다(i2c4 0x10 + csi2-dphy0 endpoint@2).
  - 보드 FIT의 fdt 자리에 여유가 약 200바이트뿐이라, Luckfox의 정적 fdt_overlay는 쓰지 않는다.
- VCM은 DT 노드 없이 사용자 공간에서 `/dev/i2c-4`의 0x0c로 직접 구동한다(위 AF 방식).
- 남은 일: 모듈 구매·핀 배치 확인, 오버레이를 센서 모듈 로드보다 먼저 적용하도록 부팅 순서 반영, rkaiq IQ 파일(imx219_RPI-CAM-AF_DEFAULT.json) 작성.

### 진행 상태와 이어서 할 일 (2026-10-16 정리)

**보드 시험 상태**
- `imx219.ko`: 보드에서 insmod/rmmod 성공(vermagic 일치). 센서가 없어 프로브까지는 확인 못 함.
- 오버레이: `-@`로 빌드한 첫 버전은 커널이 거부(-22, live tree에 `__symbols__` 없음). `-@` 없이 다시 빌드한 버전(`board/out/imx219.dtbo`)은 아직 보드에서 시험하지 않음.
- 시험 도중 보드가 네트워크에서 사라짐(load average 10, adb push EOF 후 ping 무응답). 보드에 영구적으로 쓴 것은 없음. boot 파티션 백업은 `backup/ultra-boot/boot_p4.img`(md5 ef5eccd8e5269c75dd690b5ec4999dde).
- 재부팅 후 할 순서:
  1. `:8090/api/device` 스캔으로 IP 찾기(전에는 172.30.1.3).
  2. dtbo와 ko 전송. adb push가 불안정하면 PC에서 HTTP로 받는다.
  3. `mkdir /sys/kernel/config/device-tree/overlays/imx219`, `cat imx219.dtbo > .../dtbo`, `echo 1 > .../status`
  4. `/proc/device-tree`에 노드가 생겼는지 확인. 그다음 insmod 하면 "Unexpected sensor id"가 나오는 것이 정상(카메라 없음).
  5. 정리: rmmod, `echo 0 > status`, rmdir

**카메라 선택 검토 결과**
- OV5640 AF: 메인라인 `ov5640.c`는 있으나 LINK_FREQ 컨트롤과 `get_mbus_config`가 없어 Rockchip csi2-dphy/mipi-csi2에서 스트리밍이 안 됨. 패치가 필요하고 AF 펌웨어(0x8000) 로드도 필요. 받은 모듈 HSK-5M1966AF(`C:\Project\SNT\ov5640.avif`)는 **DVP 전용**(Y2–Y9, PCLK, HREF, VSYNC)이라 MIPI 단자에 연결 불가.
- 추천 순위:
  1. MIS5001 5MP (A): 바로 꽂힘, IQ 있음, 수동 초점.
  2. SC3336.
  3. AF가 꼭 필요하면 IMX219 AF(Arducam B0182).
- 촬영 거리가 스페이서로 고정이면 AF 없이 초점을 한 번 맞춰 고정하는 쪽이 낫다.
  - 4mm 렌즈를 약 1.5mm 풀면 15mm에 초점이 맞는다(계산값).
  - 그때 화면 폭이 약 14mm라 가운데 6mm를 잘라내면 약 1100px, 픽셀당 5µm.
- 일반 AF 모듈(B0182 포함)은 최단 초점거리가 약 10cm라, 어느 쪽이든 15mm 접사에는 접사 렌즈가 필요.
- 아직 사용자가 MIS5001과 IMX219 AF 중 어느 쪽으로 할지 정하지 않음.

**보드 20핀 카메라 단자 (사용자가 준 그림, Ultra W 실물과 같은지 미확인)**
1 GND, 2 XCLK, 3 GND, 4 MDP0, 5 MDN0, 6 GND, 7 MCP, 8 MCN, 9 GND, 10 MDP1, 11 MDN1, 12 GND, 13 SIO-C, 14 SIO-D, 15 GND, 16 RESET, 17 GND, 18 GND, 19 +3.3V, 20 +3.3V

**IMX219 AF 선택 시 변환 보드 (RPi 15핀 → 20핀)**

| RPi 15핀 | 20핀 |
|---|---|
| 1/4/7/10 GND | GND들 |
| 3 D0_P, 2 D0_N | 4 MDP0, 5 MDN0 |
| 6 D1_P, 5 D1_N | 10 MDP1, 11 MDN1 |
| 9 CK_P, 8 CK_N | 7 MCP, 8 MCN |
| 13 SCL, 14 SDA | 13 SIO-C, 14 SIO-D |
| 11 CAM_IO0 | 16 RESET (gpio3 PC5, High=켜짐) |
| 12 CAM_IO1 | 연결 안 함 |
| 15 3V3 | 19/20 +3.3V |
| — | 2 XCLK 연결 안 함 (모듈에 24MHz 발진기 있음) |

- PCB 제작 전에 멀티미터로 20핀 배치를 확인한다(GND끼리 연결, 19/20에 3.3V).
- MIPI는 100Ω 차동으로 짧게, 길이를 맞춘다.

**IMX219 남은 작업**
- IQ: mis5001 또는 sc4336 JSON을 바탕으로 임시 버전 작성. 고칠 값: 게인 1–10.67배 아날로그, 블랙레벨 64(10bit). 그 뒤 RKISP2.x Tuner로 AWB/CCM/LSC 튜닝.
- AF: DW9714(0x0C)를 `/dev/i2c-4`로 직접 구동. 위치 0–1023, 2바이트 `[(pos>>4)&0x3F, (pos<<4)&0xF0|slew]`. 넓게 훑은 뒤 좁게 다시 훑는 콘트라스트 AF를 camhelper에 넣는다.
- 부팅 통합: 오버레이 적용과 `insmod imx219.ko`를 `insmod_ko.sh`에서 video_rkcif/rkisp보다 먼저(보드 파일 수정 전에 사용자 확인).
