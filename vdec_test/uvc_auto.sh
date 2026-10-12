#!/bin/sh
# USB를 host 모드로 바꾼 뒤 UVC 카메라 MJPEG -> VDEC 테스트를 자동 실행하고 peripheral 로 복귀.
# 결과: $OUT/uvc_result.log, 첫 프레임 덤프: $OUT/uvc_<w>x<h>.mjpg/.yuv
# usage: nohup sh uvc_auto.sh > /dev/null 2>&1 &
OUT=${OUT:-/userdata/uvc_test}
BIN=${BIN:-/userdata/uvc_test/uvc_vdec_test}
OTG=/sys/devices/platform/ff3e0000.usb2-phy/otg_mode
DWC3_MODE=/sys/kernel/debug/usb/ffb00000.usb/mode
WAIT=${WAIT:-180}
FRAMES=${FRAMES:-150}
LOG=$OUT/uvc_result.log

mkdir -p $OUT
exec >$LOG 2>&1
echo "=== start $(date) ==="

echo "dr_mode=$(tr -d '\0' </proc/device-tree/usbdrd/usb@ffb00000/dr_mode)"
if [ -z "$SKIP_SWITCH" ]; then
    sleep 3   # adb 응답이 PC로 돌아갈 시간
    # dr_mode=peripheral 인 DT 에서는 이 런타임 전환이 무시됨 (DT 를 host 로 바꾸고 SKIP_SWITCH=1 로 실행)
    mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
    echo host > $OTG
    echo host > $DWC3_MODE
    sleep 2
    echo "otg_mode=$(cat $OTG) dwc3_mode=$(cat $DWC3_MODE)"
fi

dev=""
i=0
while [ $i -lt $WAIT ]; do
    for v in /sys/class/video4linux/video*; do
        if readlink -f $v/device 2>/dev/null | grep -q usb; then
            # uvcvideo 는 capture 노드와 metadata 노드를 만듦: MJPEG 지원 노드 선택
            n=/dev/$(basename $v)
            if v4l2-ctl -d $n --list-formats 2>/dev/null | grep -q MJPG; then
                dev=$n
                break 2
            fi
        fi
    done
    sleep 1
    i=$((i + 1))
done

if [ -z "$dev" ]; then
    echo "!!! no UVC MJPEG camera found within ${WAIT}s"
    lsusb 2>/dev/null
    dmesg | grep -iE 'usb|uvc' | tail -20
else
    echo "camera node: $dev (found after ${i}s)"
    dmesg | grep -iE 'uvc|usb [0-9]-' | tail -10
    echo "--- formats"
    v4l2-ctl -d $dev --list-formats-ext
    # MJPG 해상도 목록 추출
    sizes=$(v4l2-ctl -d $dev --list-formats-ext | awk '/MJPG/{m=1;next} /\[[0-9]+\]:/{m=0} m && /Size: Discrete/{print $3}' | sort -u)
    echo "MJPG sizes: $sizes"
    for s in $sizes; do
        w=${s%x*}
        h=${s#*x}
        # RAM 이 ~170MB 라 1080p 초과는 건너뜀
        if [ $((w * h)) -gt $((1920 * 1080)) ]; then
            echo "skip $s (> 1080p)"
            continue
        fi
        echo
        echo "===== $w x $h ====="
        $BIN $dev $w $h $FRAMES 30 $OUT/uvc_${w}x${h} &
        p=$!
        t=0
        while kill -0 $p 2>/dev/null; do
            sleep 1
            t=$((t + 1))
            [ $t -ge 60 ] && { kill -9 $p; echo "!!! TIMEOUT"; }
        done
        free | sed -n 2p
        sync
    done
fi

if [ -z "$SKIP_SWITCH" ]; then
    echo device > $DWC3_MODE
    echo peripheral > $OTG
    sleep 2
    echo "otg_mode=$(cat $OTG) dwc3_mode=$(cat $DWC3_MODE)"
fi
sync
echo "=== done $(date) ==="
