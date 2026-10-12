#!/bin/sh
# 보드에서 실행: 이 폴더를 /opt/uvc-stream 에 설치하고 부팅 시 자동 실행을 등록한다.
#   sh install.sh            설치 + 시작
#   sh install.sh uninstall  제거
# USB 를 host 모드로 바꾸는 것은 별도 단계다 (usbmode.sh, README 참고).
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
DST=/opt/uvc-stream
INIT=/etc/init.d/S95uvcstream

if [ "$1" = "uninstall" ]; then
    [ -x "$INIT" ] && "$INIT" stop || true
    rm -f "$INIT"
    rm -rf "$DST"
    echo "제거했습니다"
    exit 0
fi

python3 -c "import ctypes, mmap, fcntl" || { echo "python3(ctypes) 가 필요합니다"; exit 1; }

[ -x "$INIT" ] && "$INIT" stop || true
mkdir -p "$DST/www"
for f in uvc_stream.py start.sh usbmode.sh; do
    cp "$SRC/$f" "$DST/"
done
cp "$SRC/www/index.html" "$DST/www/"
# 설정은 이미 있으면 보존
[ -f "$DST/stream.conf" ] || cp "$SRC/stream.conf" "$DST/"
cp "$SRC/S01bootlog" /etc/init.d/S01bootlog
cp "$SRC/S95uvcstream" "$INIT"

# Windows 에서 넘어온 CRLF 제거
sed -i 's/\r$//' "$DST"/*.sh "$DST/stream.conf" "$DST/uvc_stream.py" "$INIT" /etc/init.d/S01bootlog
chmod +x "$DST"/*.sh "$DST/uvc_stream.py" "$INIT" /etc/init.d/S01bootlog
sync

"$INIT" start
echo "설치 완료: $DST  (로그 /tmp/uvc-stream.log)"
echo "USB dr_mode: $(tr -d '\0' </proc/device-tree/usbdrd/usb@ffb00000/dr_mode)"
