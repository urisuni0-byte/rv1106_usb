#!/bin/bash
# PC(Git Bash)에서 실행: WSL 로 보드 프로그램을 빌드한다 -> board/out/{scannerd,camhelper}
#   scannerd : Go (pion WebRTC, go-ble, zeroconf) - ARMv7 정적 바이너리
#   camhelper: C (V4L2 + rockit VENC) - Luckfox uClibc 툴체인
# WSL 준비: ~/sdk/go (Go 1.24+), ~/lfx/luckfox-pico (툴체인 + rockit 헤더), ~/lfx/lnk/librockit_full.so
set -euo pipefail
cd "$(dirname "$0")/.."
WIN_DIR=$(pwd -W 2>/dev/null || pwd)
WSL_DIR=$(wsl -- wslpath -a "$WIN_DIR" | tr -d '\r')
VER=$(sed -n 's/^FW_VERSION=//p' board/scanner.conf | tr -d '\r')

MSYS_NO_PATHCONV=1 wsl -- bash -s "$WSL_DIR" "$VER" <<'SH'
set -e
cd "$1/board"
export PATH=$HOME/sdk/go/bin:$PATH GOPATH=$HOME/gopath GOCACHE=$HOME/.cache/go-build
mkdir -p out
bash camhelper/build.sh
(cd scannerd && GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0 \
    go build -trimpath -ldflags "-s -w -X main.version=$2" -o ../out/scannerd .)
file out/scannerd
SH
ls -la board/out
