#!/bin/bash
# WSL에서 실행: Luckfox 커널 트리로 imx219.ko 외부 모듈 빌드 -> board/out/imx219.ko
# 보드 커널은 MODVERSIONS 가 꺼져 있어 vermagic(5.10.160 mod_unload ARMv7 thumb2 p2v8)만 맞으면 로드된다.
# KBUILD_OUT: 커널 빌드 디렉터리(O=). 처음 한 번 defconfig + modules_prepare 를 한다.
set -e
LUCKFOX_SDK=${LUCKFOX_SDK:-$HOME/lfx/luckfox-pico}
KSRC=$LUCKFOX_SDK/sysdrv/source/kernel
KBUILD_OUT=${KBUILD_OUT:-$HOME/lfx/kbuild}
CROSS=$LUCKFOX_SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-
KMAKE="make -C $KSRC O=$KBUILD_OUT ARCH=arm CROSS_COMPILE=$CROSS"
cd "$(dirname "$0")"

if [ ! -f "$KBUILD_OUT/include/generated/utsrelease.h" ]; then
    mkdir -p "$KBUILD_OUT"
    $KMAKE luckfox_rv1106_linux_defconfig
    $KMAKE -j"$(nproc)" modules_prepare
fi

# 소스 디렉터리를 더럽히지 않도록 빌드용 복사본에서 컴파일
BUILD=$KBUILD_OUT/ext-imx219
mkdir -p "$BUILD"
cp imx219.c Makefile "$BUILD/"
KBUILD_MODPOST_WARN=1 $KMAKE M="$BUILD" modules
mkdir -p ../../out
cp "$BUILD/imx219.ko" ../../out/
modinfo ../../out/imx219.ko | grep -E "vermagic|depends"

# 런타임 DT 오버레이. /plugin/ 이라 __local_fixups__ 는 자동 생성된다.
# -@ 를 쓰면 __symbols__ 가 들어가는데, 보드 live tree 에 __symbols__ 가 없어 커널이 거부한다(-22).
# 기본 DTB 의 숫자 phandle(cru, gpio3)을 오버레이 내부 노드로 오해하는 경고는 끈다
dtc -W no-clocks_property -W no-gpios_property -I dts -O dtb -o ../../out/imx219.dtbo imx219-overlay.dts
ls -la ../../out/imx219.dtbo
