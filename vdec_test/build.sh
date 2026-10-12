#!/bin/bash
# WSL에서 실행: Luckfox uClibc 툴체인으로 vdec_test, uvc_vdec_test 크로스 컴파일
# LUCKFOX_SDK: luckfox-pico 경로, BOARD_LIB: 보드에서 가져온 librockit_full.so 등이 있는 디렉터리
set -e
LUCKFOX_SDK=${LUCKFOX_SDK:-$HOME/lfx/luckfox-pico}
BOARD_LIB=${BOARD_LIB:-$HOME/lfx/lnk}
CC=$LUCKFOX_SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc
cd "$(dirname "$0")"
for t in vdec_test uvc_vdec_test; do
    $CC -O2 -Wall -I$LUCKFOX_SDK/media/rockit/rockit/mpi/sdk/include $t.c -o $t \
        -L$BOARD_LIB -l:librockit_full.so -Wl,--allow-shlib-undefined -lpthread
done
file vdec_test uvc_vdec_test
