#!/bin/bash
# luckfox-config 의 USB 모드 변경 로직(luckfox_usb_app)을 비대화형으로 호출.
# 부팅 파티션 DTB 의 /usbdrd/usb@ffb00000 dr_mode 를 바꾸며, 재부팅 후 적용됨.
# usage: usbmode.sh <host|peripheral|show>
set -e
mode="$1"
LF=/usr/bin/luckfox-config

show() {
    echo "running dr_mode: $(tr -d '\0' </proc/device-tree/usbdrd/usb@ffb00000/dr_mode)"
}

case "$mode" in
host | peripheral) ;;
show)
    show
    exit 0
    ;;
*)
    echo "usage: $0 <host|peripheral|show>"
    exit 1
    ;;
esac

# Run 섹션(메인 디스패치) 앞까지만 source 해서 함수와 변수만 가져옴
. <(sed '/^# --------------------------- Run ------------------------------/,$d' $LF)
LF_GUI_ENABLE=0
luckfox_config_init >/dev/null 2>&1 || true
echo "media: $LUCKFOX_CHIP_MEDIA"
[ -n "$LUCKFOX_CHIP_MEDIA" ] || { echo "unknown boot media"; exit 1; }

luckfox_usb_app "$mode"
sync

# 기록된 DTB 다시 읽어서 확인
luckfox_update_fdt
echo "boot DTB dr_mode: $(fdtget $LUCKFOX_FDT_DTB /usbdrd/usb@ffb00000 dr_mode 2>/dev/null ||
    grep -A30 'usb@ffb00000' $LUCKFOX_FDT_DUMP_TXT | grep -m1 dr_mode)"
show
echo "reboot to apply"
