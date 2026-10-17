#!/bin/bash
# PC(Git Bash)에서 보드로 scannerd/camhelper 를 올리고 설치(업데이트)한다.
#
#   bash tools/deploy.sh                 USB adb (보드가 peripheral 모드일 때)
#   bash tools/deploy.sh pad             패드 경유 (보드가 host 모드라 USB adb 가 없을 때)
#                                        패드가 USB adb 로 PC 에, Wi-Fi 로 보드 AP(RvCam-xxxx)에 붙어 있어야 한다
#   bash tools/deploy.sh 172.30.1.50     Wi-Fi adb 직접 (PC 와 보드가 같은 공유기에 있을 때)
#
#   CONF=1 bash tools/deploy.sh pad      board/scanner.conf 도 덮어쓴다 (기본은 보드 설정 유지)
#
# 빌드는 먼저: bash tools/build_board.sh
set -euo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."

ADB=${ADB:-adb}
BOARD_IP=192.168.4.1
RELAY_PORT=15555          # 패드 안에서 듣는 포트
LOCAL_PORT=25555          # PC 쪽 포트 (adb forward)
RELAY_PID=""

cleanup() {
    if [ -n "$RELAY_PID" ]; then
        "$ADB" disconnect "127.0.0.1:$LOCAL_PORT" >/dev/null 2>&1 || true
        "$ADB" -s "$PAD" forward --remove "tcp:$LOCAL_PORT" >/dev/null 2>&1 || true
        # 패턴 끝 글자를 [] 로 감싸야 pkill 이 자기 셸까지 잡지 않는다
        "$ADB" -s "$PAD" shell "pkill -f 'nc -L -p ${RELAY_PORT%?}[${RELAY_PORT: -1}]'" >/dev/null 2>&1 || true
        kill "$RELAY_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# 첫 번째 adb 장치 중 조건에 맞는 것
find_serial() {     # $1 = 'board' | 'pad'
    for s in $("$ADB" devices | awk 'NR>1 && $2=="device"{print $1}'); do
        if "$ADB" -s "$s" shell 'cat /proc/device-tree/compatible 2>/dev/null' | tr '\0' ' ' | grep -q rv1106; then
            [ "$1" = board ] && { echo "$s"; return; }
        else
            [ "$1" = pad ] && { echo "$s"; return; }
        fi
    done
}

case "${1:-}" in
"")
    T=$(find_serial board)
    [ -n "$T" ] || { echo "USB adb 에서 RV1106 보드를 찾지 못했습니다 (host 모드라면: bash tools/deploy.sh pad)"; exit 1; }
    ;;
pad)
    PAD=${PAD:-$(find_serial pad)}
    [ -n "$PAD" ] || { echo "USB adb 에 패드가 없습니다"; exit 1; }
    "$ADB" -s "$PAD" shell "ping -c1 -W2 $BOARD_IP >/dev/null" ||
        { echo "패드가 보드($BOARD_IP)에 닿지 않습니다 - 패드를 RvCam-xxxx Wi-Fi 에 연결하세요"; exit 1; }
    # 패드에서 toybox nc 로 TCP 중계 (adb 셸이 살아 있는 동안만 유지된다)
    "$ADB" -s "$PAD" shell "cd /data/local/tmp; while true; do rm -f f; mkfifo f; \
        toybox nc -l -p $RELAY_PORT < f | toybox nc $BOARD_IP 5555 > f; done" >/dev/null 2>&1 &
    RELAY_PID=$!
    sleep 2
    "$ADB" -s "$PAD" forward "tcp:$LOCAL_PORT" "tcp:$RELAY_PORT" >/dev/null
    T="127.0.0.1:$LOCAL_PORT"
    "$ADB" connect "$T" >/dev/null
    echo "패드($PAD) 경유"
    ;;
*)
    T="$1:5555"
    "$ADB" connect "$T" >/dev/null
    ;;
esac
# 중계를 거치면 adb 인증이 끝날 때까지 몇 초 동안 offline 으로 보인다
for i in $(seq 1 15); do
    [ "$("$ADB" -s "$T" get-state 2>/dev/null)" = device ] && break
    sleep 1
done
echo "대상: $T"

[ -f board/out/scannerd ] && [ -f board/out/camhelper ] || { echo "먼저 빌드하세요: bash tools/build_board.sh"; exit 1; }
"$ADB" -s "$T" shell 'rm -rf /tmp/scanner-src && mkdir -p /tmp/scanner-src'
for f in board/out/scannerd board/out/camhelper board/usbmode.sh board/scanner.conf \
         board/S96scanner board/S01bootlog board/install.sh; do
    "$ADB" -s "$T" push "$f" /tmp/scanner-src/ >/dev/null
done
[ "${CONF:-0}" = 1 ] && "$ADB" -s "$T" shell 'rm -f /opt/scanner/scanner.conf'
"$ADB" -s "$T" shell 'sed -i "s/\r$//" /tmp/scanner-src/install.sh && sh /tmp/scanner-src/install.sh'
sleep 4
"$ADB" -s "$T" shell 'tail -15 /tmp/scanner.log'
