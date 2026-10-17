#!/bin/bash
# WSL에서 실행: Luckfox uClibc 툴체인으로 camhelper 크로스 컴파일
# LUCKFOX_SDK: luckfox-pico 경로, BOARD_LIB: 보드에서 가져온 librockit_full.so, librkaiq.so(MIPI 모드) 가 있는 디렉터리
set -e
LUCKFOX_SDK=${LUCKFOX_SDK:-$HOME/lfx/luckfox-pico}
BOARD_LIB=${BOARD_LIB:-$HOME/lfx/lnk}
CC=$LUCKFOX_SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc
cd "$(dirname "$0")"
mkdir -p ../out
$CC -O2 -Wall -Wextra -Wno-unused-parameter \
    -I$LUCKFOX_SDK/media/rockit/rockit/mpi/sdk/include camhelper.c -o ../out/camhelper \
    -L$BOARD_LIB -l:librockit_full.so -l:librkaiq.so -Wl,--allow-shlib-undefined -lpthread -lm
file ../out/camhelper
