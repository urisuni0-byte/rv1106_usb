#!/bin/sh
# 보드에서 실행: scannerd + camhelper 를 /opt/scanner 에 설치하고 부팅 시 자동 실행을 등록한다.
#   sh install.sh            설치 + 시작
#   sh install.sh uninstall  제거
# USB 를 host 모드로 바꾸는 것은 별도 단계다 (usbmode.sh, README 참고).
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
DST=/opt/scanner
INIT=/etc/init.d/S96scanner

if [ "$1" = "uninstall" ]; then
    [ -x "$INIT" ] && "$INIT" stop || true
    rm -f "$INIT"
    rm -rf "$DST"
    echo "제거했습니다 (촬영 데이터 /userdata/scanner 는 남겨 둠)"
    exit 0
fi

for f in scannerd camhelper; do
    [ -f "$SRC/$f" ] || { echo "$SRC/$f 가 없습니다 (PC 에서 빌드 후 deploy.sh 로 올리세요)"; exit 1; }
done

# 예전 MJPEG 서버(uvc_stream.py)는 카메라를 계속 쥐고 있으므로 끈다
if [ -x /etc/init.d/S95uvcstream ]; then
    /etc/init.d/S95uvcstream stop || true
    rm -f /etc/init.d/S95uvcstream
    echo "예전 uvc-stream 서비스를 제거했습니다 (/opt/uvc-stream 파일은 남겨 둠)"
fi

[ -x "$INIT" ] && "$INIT" stop || true
mkdir -p "$DST"
cp "$SRC/scannerd" "$SRC/camhelper" "$SRC/usbmode.sh" "$DST/"
# 설정은 이미 있으면 보존
[ -f "$DST/scanner.conf" ] || cp "$SRC/scanner.conf" "$DST/"
cp "$SRC/S01bootlog" /etc/init.d/S01bootlog
cp "$SRC/S96scanner" "$INIT"

# Windows 에서 넘어온 CRLF 제거
sed -i 's/\r$//' "$DST/usbmode.sh" "$DST/scanner.conf" "$INIT" /etc/init.d/S01bootlog
chmod +x "$DST/scannerd" "$DST/camhelper" "$DST/usbmode.sh" "$INIT" /etc/init.d/S01bootlog
sync

[ -x /usr/bin/rvcam-net.sh ] || echo "주의: /usr/bin/rvcam-net.sh 가 없어 BLE Wi-Fi 프로비저닝이 동작하지 않습니다"
[ -e /sys/class/bluetooth/hci0 ] || echo "주의: hci0 가 없습니다 - scanner.conf 의 BLE_INIT 에 블루투스를 올리는 명령을 넣으세요"

"$INIT" start
echo "설치 완료: $DST  (로그 /tmp/scanner.log)"
echo "USB dr_mode: $(tr -d '\0' </proc/device-tree/usbdrd/usb@ffb00000/dr_mode 2>/dev/null)"
