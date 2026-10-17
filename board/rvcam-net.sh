#!/bin/sh
#
# RvCam 네트워크 관리자.
#
# 규칙은 하나다: **AP는 언제나 떠 있다.**
#
# 보드를 다른 장소로 옮기면 저장된 집 Wi-Fi는 거기 없다. 그것만으로 보드가 네트워크에서
# 완전히 사라지면 되살릴 방법은 USB나 시리얼을 꽂는 것뿐이다 — 그게 이 스크립트가
# 생기기 전 상황이었다. 그래서 AP를 먼저, 무조건 올리고 STA는 그 다음에 붙인다.
# STA가 실패해도 AP는 그대로이므로 앱이 들어와 새 Wi-Fi를 넣어줄 수 있다.
#
# AP와 STA는 같은 라디오 위의 두 인터페이스다 (AIC8800이 보고하는 조합:
# #{managed} <= 1, #{AP} <= 1). wlan0이 STA, wlan1이 AP다.
#
set -u

STA_IF=wlan0
AP_IF=wlan1
AP_ADDR=192.168.4.1
AP_MASK=255.255.255.0
DHCP_FROM=192.168.4.10
DHCP_TO=192.168.4.100

# 설정은 /userdata에 둔다. 별도 파티션이라 루트를 다시 구워도 살아남는다.
CFG=/userdata/rvcam
# 생성물은 tmpfs에. 매 부팅 새로 만들어지니 낡은 자격증명이 루트에 남지 않는다.
RUN=/var/run/rvcam
HOSTAPD_CONF=$RUN/hostapd.conf
WPA_CONF=$RUN/wpa_supplicant.conf
LOG=/tmp/rvcam-net.log

log() {
    echo "[$(date '+%H:%M:%S')] $*" >>"$LOG"
    echo "$*"
}

# 값마다 파일 하나. `KEY=value`를 source 하는 방식은 비밀번호에 따옴표나 달러 기호가
# 들어가는 순간 깨지거나 임의 실행이 된다. 파일 하나에 값 하나면 그 문제가 사라진다.
read_cfg() {
    if [ -s "$CFG/$1" ]; then
        tr -d '\r\n' <"$CFG/$1"
    else
        printf '%s' "${2:-}"
    fi
}

write_cfg() {
    mkdir -p "$CFG"
    printf '%s' "$2" >"$CFG/$1"
    chmod 600 "$CFG/$1"
}

# wpa_supplicant.conf의 따옴표 문자열에 넣기 위한 이스케이프.
esc() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

# 죽으라고 신호를 보내는 것과 죽은 것은 다르다. dnsmasq는 소켓을 쥐고 있으므로
# 종료를 기다리지 않고 다음 인스턴스를 띄우면 "Address already in use"로 실패한다.
kill_pidfile() {
    [ -f "$1" ] || return 0
    pid=$(cat "$1" 2>/dev/null)
    rm -f "$1"
    [ -n "${pid:-}" ] || return 0
    kill "$pid" 2>/dev/null
    i=0
    while [ $i -lt 20 ] && kill -0 "$pid" 2>/dev/null; do
        i=$((i + 1))
        sleep 1
    done
    # 20초를 줬는데도 살아 있으면 더 기다릴 이유가 없다.
    kill -9 "$pid" 2>/dev/null
    return 0
}

# 이름으로 잡아 죽인다. pid 파일이 사라졌는데 프로세스는 살아 있는 경우가 있고
# (rkipc가 `killall -9 udhcpc`를 돌리면 우리 pid 파일만 남는다), 그러면 라디오를
# 붙잡은 고아 데몬이 다음 기동을 조용히 망가뜨린다.
kill_orphans() {
    want=$1
    me=$$
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        [ "$pid" = "$me" ] && continue
        [ "$(cat "$p/comm" 2>/dev/null)" = "$want" ] || continue
        kill -9 "$pid" 2>/dev/null
    done
    return 0
}

# rkipc는 인터페이스를 가리지 않고 DHCP 클라이언트를 붙인다 — 바이너리 안에
# `udhcpc -i %s -T 1 -A 0 -b -q`가 들어 있다. AP인 wlan1에는 DHCP 서버가 없으니
# 그 udhcpc는 실패하고, busybox 기본 스크립트의 deconfig가
# `ifconfig $interface 0.0.0.0`을 실행해 **AP 주소를 지워버린다**. 그러면 SSID는
# 계속 보이는데 붙은 단말이 주소를 못 받아 36초 뒤 떨어져 나간다.
#
# rkipc를 고칠 수는 없으므로 udhcpc 쪽에서 막는다. 호출자가 누구든 통한다.
install_udhcpc_guard() {
    script=/usr/share/udhcpc/default.script
    [ -f "$script" ] || return 0
    grep -q 'RvCam guard' "$script" && return 0
    cp "$script" "$script.rvcam-orig"
    sed -i "2i # RvCam guard: AP 인터페이스는 DHCP 클라이언트가 건드리면 안 된다.\n[ \"\${interface:-}\" = \"$AP_IF\" ] && exit 0" "$script"
    log "udhcpc 가드 설치: $AP_IF는 DHCP 클라이언트가 건드리지 못한다"
    return 0
}

# 위 가드는 새로 뜨는 udhcpc를 막는다. 이미 돌고 있는 놈은 따로 치워야 한다.
# 인터페이스를 받는다 — rkipc는 eth0에도 udhcpc를 붙이는데 그건 건드릴 이유가 없다.
kill_dhcp_client() {
    iface=$1
    me=$$
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        [ "$pid" = "$me" ] && continue
        [ "$(cat "$p/comm" 2>/dev/null)" = "udhcpc" ] || continue
        # cmdline은 NUL로 구분된다. 인터페이스 이름이 들어 있으면 그놈이다.
        case "$(tr '\0' ' ' <"$p/cmdline" 2>/dev/null)" in
            *" $iface "*) kill -9 "$pid" 2>/dev/null ;;
        esac
    done
    return 0
}

mac_suffix() {
    tr -d ':' </sys/class/net/$STA_IF/address 2>/dev/null |
        tr 'A-F' 'a-f' | cut -c9-12
}

# 주파수를 채널 번호로. 2.4 GHz와 5 GHz만 — 이 칩이 내는 건 그게 전부다.
freq_to_channel() {
    f=$1
    if [ "$f" -ge 2412 ] && [ "$f" -le 2472 ]; then
        echo $(((f - 2407) / 5))
    elif [ "$f" = 2484 ]; then
        echo 14
    elif [ "$f" -ge 5180 ] && [ "$f" -le 5865 ]; then
        echo $(((f - 5000) / 5))
    else
        echo ""
    fi
}

sta_channel() {
    f=$(iw dev $STA_IF link 2>/dev/null | sed -n 's/.*freq:[ \t]*\([0-9]*\).*/\1/p' | head -1)
    if [ -n "${f:-}" ]; then
        freq_to_channel "$f"
    fi
}

ensure_defaults() {
    mkdir -p "$CFG" "$RUN"
    chmod 700 "$CFG"
    install_udhcpc_guard
    # AP 이름에 MAC 뒷자리를 붙인다. 보드가 여러 대여도 서로 구분되고, 앱은 접두사
    # "RvCam-"만 알면 정확한 이름을 몰라도 붙을 수 있다.
    [ -s "$CFG/ap.ssid" ] || write_cfg ap.ssid "RvCam-$(mac_suffix)"
    [ -s "$CFG/ap.psk" ] || write_cfg ap.psk "rvcam12345"
    [ -s "$CFG/ap.channel" ] || write_cfg ap.channel "6"

    # 예전 설정을 한 번 물려받는다. 이 스크립트 이전에는 /etc/wpa_supplicant.conf가
    # 유일한 저장소였으므로 거기 있던 네트워크를 잃지 않게 한 번만 옮겨온다.
    if [ ! -f "$CFG/sta.ssid" ] && [ -f /etc/wpa_supplicant.conf ]; then
        old_ssid=$(sed -n 's/^[ \t]*ssid="\(.*\)"[ \t]*$/\1/p' /etc/wpa_supplicant.conf | head -1)
        old_psk=$(sed -n 's/^[ \t]*psk="\(.*\)"[ \t]*$/\1/p' /etc/wpa_supplicant.conf | head -1)
        if [ -n "${old_ssid:-}" ]; then
            log "예전 설정에서 '$old_ssid'를 물려받습니다"
            write_cfg sta.ssid "$old_ssid"
            write_cfg sta.psk "${old_psk:-}"
        fi
    fi
}

# ---------------------------------------------------------------- AP

write_hostapd_conf() {
    channel=$1
    ssid=$(read_cfg ap.ssid)
    psk=$(read_cfg ap.psk)
    # 5 GHz 채널이면 hw_mode도 따라가야 한다. STA 채널을 물려받을 때 생긴다.
    if [ "$channel" -ge 36 ] 2>/dev/null; then
        hw=a
    else
        hw=g
    fi

    {
        echo "interface=$AP_IF"
        echo "driver=nl80211"
        echo "ssid=$ssid"
        echo "hw_mode=$hw"
        echo "channel=$channel"
        echo "ieee80211n=1"
        echo "wmm_enabled=1"
        echo "auth_algs=1"
        echo "ignore_broadcast_ssid=0"
        echo "max_num_sta=8"
        echo "wpa=2"
        echo "wpa_key_mgmt=WPA-PSK"
        echo "rsn_pairwise=CCMP"
        echo "wpa_passphrase=$psk"
    } >"$HOSTAPD_CONF"
    chmod 600 "$HOSTAPD_CONF"
}

ap_running() {
    [ -f "$RUN/hostapd.pid" ] && kill -0 "$(cat "$RUN/hostapd.pid" 2>/dev/null)" 2>/dev/null
}

ap_stop() {
    kill_pidfile "$RUN/dnsmasq.pid"
    kill_pidfile "$RUN/hostapd.pid"
    ifconfig $AP_IF down 2>/dev/null
    iw dev $AP_IF del 2>/dev/null
    return 0
}

ap_start() {
    ensure_defaults
    ap_stop

    channel=${1:-}
    if [ -z "$channel" ]; then
        channel=$(read_cfg ap.channel 6)
    fi
    log "AP 시작 (채널 $channel)"

    # 라디오 위에 AP용 가상 인터페이스를 얹는다. wlan0은 STA 몫으로 남겨둔다.
    if ! iw dev $STA_IF interface add $AP_IF type __ap >>"$LOG" 2>&1; then
        log "AP 인터페이스 생성 실패"
        return 1
    fi
    ip link set $AP_IF up 2>>"$LOG"

    write_hostapd_conf "$channel"
    if ! hostapd -B -P "$RUN/hostapd.pid" "$HOSTAPD_CONF" >>"$LOG" 2>&1; then
        log "hostapd 시작 실패 — $LOG 확인"
        iw dev $AP_IF del 2>/dev/null
        return 1
    fi

    # hostapd가 인터페이스를 다시 만지므로 주소는 그 뒤에 준다.
    ap_ensure_addr
    ap_ensure_dnsmasq

    log "AP 준비됨: $(read_cfg ap.ssid) @ $AP_ADDR"
    return 0
}

ap_has_addr() {
    ip -4 addr show $AP_IF 2>/dev/null | grep -q "inet $AP_ADDR/"
}

# 주소를 주고 **실제로 붙었는지 확인한다**. 예전에는 ifconfig를 한 번 쏘고 성공했다고
# 로그를 찍었는데, rkipc의 udhcpc가 뒤에서 지워버리면 로그만 초록불이고 AP는
# 주소 없이 떠 있었다.
ap_ensure_addr() {
    kill_dhcp_client $AP_IF
    i=0
    while [ $i -lt 5 ]; do
        ap_has_addr && return 0
        ifconfig $AP_IF "$AP_ADDR" netmask "$AP_MASK" up 2>>"$LOG"
        i=$((i + 1))
        sleep 1
    done
    if ap_has_addr; then
        return 0
    fi
    log "AP 주소 $AP_ADDR 설정 실패 — 붙은 단말이 주소를 못 받습니다"
    return 1
}

dnsmasq_running() {
    [ -f "$RUN/dnsmasq.pid" ] && kill -0 "$(cat "$RUN/dnsmasq.pid" 2>/dev/null)" 2>/dev/null
}

# DHCP가 없으면 폰이 주소를 못 받아 AP에 붙어도 아무것도 못 한다.
# option 3(라우터)으로 보드를 알려주면 앱의 게이트웨이 자동 감지가 주소 칸을
# 그대로 채워준다.
ap_ensure_dnsmasq() {
    dnsmasq_running && return 0
    dnsmasq \
        --interface=$AP_IF --bind-interfaces --except-interface=lo \
        --dhcp-range=$DHCP_FROM,$DHCP_TO,$AP_MASK,12h \
        --dhcp-option=3,$AP_ADDR --dhcp-option=6,$AP_ADDR \
        --no-resolv --no-hosts --log-facility=/dev/null \
        --pid-file="$RUN/dnsmasq.pid" >>"$LOG" 2>&1
    if dnsmasq_running; then
        return 0
    fi
    log "dnsmasq 시작 실패 — 폰이 주소를 못 받습니다"
    return 1
}

# ---------------------------------------------------------------- STA

write_wpa_conf() {
    ssid=$(read_cfg sta.ssid)
    psk=$(read_cfg sta.psk)
    {
        echo "ctrl_interface=/var/run/wpa_supplicant"
        # 0으로 둔다. 이 파일은 우리가 매번 새로 쓰는 것이고, wpa_supplicant가
        # 덧써버리면 /userdata의 설정과 어긋난다.
        echo "update_config=0"
        echo "country=KR"
        echo ""
        echo "network={"
        echo "	ssid=\"$(esc "$ssid")\""
        if [ -n "$psk" ]; then
            echo "	psk=\"$(esc "$psk")\""
        else
            echo "	key_mgmt=NONE"
        fi
        echo "	scan_ssid=1"
        echo "}"
    } >"$WPA_CONF"
    chmod 600 "$WPA_CONF"
}

sta_stop() {
    kill_pidfile "$RUN/udhcpc.pid"
    kill_pidfile "$RUN/wpa.pid"
    # pid 파일이 사라져도 supplicant는 살아남을 수 있다. 그 고아가 라디오를 쥐고 있으면
    # 다음 sta_start가 붙지 못하고, status는 sta_daemon=0이라고 거짓 보고한다.
    kill_orphans wpa_supplicant
    kill_dhcp_client $STA_IF
    # wpa_supplicant가 남긴 소켓이 있으면 다음 기동이 EADDRINUSE로 죽는다.
    rm -f /var/run/wpa_supplicant/$STA_IF
    ip addr flush dev $STA_IF 2>/dev/null
    return 0
}

sta_addr() {
    ip -4 addr show $STA_IF 2>/dev/null |
        sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1
}

# rkipc는 자기 네트워크 코드에서 `killall -9 udhcpc`를 돌린다. 우리 임대 요청이
# 하필 그때 진행 중이면 조용히 사라지고, STA는 결합만 된 채(wpa_state=COMPLETED,
# 신호도 정상) 주소가 없는 상태로 남는다. 그래서 한 번 쏘고 끝내지 않고 다시 시도한다.
sta_ensure_lease() {
    try=0
    while [ $try -lt 3 ]; do
        [ -n "$(sta_addr)" ] && return 0
        try=$((try + 1))
        udhcpc -i $STA_IF -b -t 8 -p "$RUN/udhcpc.pid" >>"$LOG" 2>&1
        i=0
        while [ $i -lt 12 ] && [ -z "$(sta_addr)" ]; do
            i=$((i + 1))
            sleep 1
        done
        [ -n "$(sta_addr)" ] && return 0
        log "DHCP 임대 실패 ($try/3) — 다시 시도합니다"
    done
    [ -n "$(sta_addr)" ]
}

sta_start() {
    ensure_defaults
    ssid=$(read_cfg sta.ssid)
    if [ -z "$ssid" ]; then
        log "저장된 Wi-Fi가 없습니다 — STA를 건너뜁니다"
        return 1
    fi

    sta_stop
    log "STA 시작: '$ssid'"
    follow_sta_channel "$ssid"
    write_wpa_conf
    ip link set $STA_IF up 2>>"$LOG"

    if ! wpa_supplicant -B -i $STA_IF -c "$WPA_CONF" -D nl80211,wext \
        -P "$RUN/wpa.pid" >>"$LOG" 2>&1; then
        log "wpa_supplicant 시작 실패"
        return 1
    fi

    state=""
    i=0
    while [ $i -lt 25 ]; do
        state=$(wpa_cli -i $STA_IF status 2>/dev/null | sed -n 's/^wpa_state=//p')
        [ "${state:-}" = "COMPLETED" ] && break
        i=$((i + 1))
        sleep 1
    done

    if [ "${state:-}" != "COMPLETED" ]; then
        log "'$ssid' 접속 실패 (상태: ${state:-없음})"
        sta_stop
        return 1
    fi

    if ! sta_ensure_lease; then
        log "'$ssid'에 붙었지만 DHCP 주소를 못 받았습니다"
        return 1
    fi
    addr=$(sta_addr)
    log "STA 연결됨: '$ssid' @ $addr"

    reconcile_ap_channel
    return 0
}

# 공유기 채널을 스캔으로 찾는다 (MHz). 없으면 빈 값.
sta_target_freq() {
    iw dev $STA_IF scan 2>/dev/null | awk -v s="$1" '
        /^BSS / { f = "" }
        /^[ \t]*freq:/ { f = $2 }
        /^[ \t]*SSID: / { sub(/^[ \t]*SSID: /, ""); if ($0 == s && f != "") { print f; exit } }'
}

# AIC8800 은 AP 가 떠 있는 채널 밖으로 STA 가 나가지 못한다 (AP 채널 6, 공유기 8 이면
# STA 가 SCANNING 에서 멈춘다). 그래서 붙기 **전에** AP 를 공유기 채널로 옮기고,
# 다음 부팅도 그 채널로 AP 를 올리도록 저장한다.
follow_sta_channel() {
    f=""
    i=0
    while [ $i -lt 3 ] && [ -z "$f" ]; do
        f=$(sta_target_freq "$1")
        i=$((i + 1))
    done
    f=${f%%.*}
    if [ -z "$f" ]; then
        log "'$1' 이 스캔에 안 보입니다 — AP 채널을 그대로 둡니다"
        return 0
    fi
    ch=$(freq_to_channel "$f")
    [ -n "${ch:-}" ] || return 0
    cur=$(iw dev $AP_IF info 2>/dev/null | sed -n 's/.*channel \([0-9]*\).*/\1/p' | head -1)
    [ "$(read_cfg ap.channel)" = "$ch" ] || write_cfg ap.channel "$ch"
    if ap_running && [ "${cur:-}" = "$ch" ]; then
        return 0
    fi
    log "AP 를 공유기 채널($ch)로 옮깁니다 (현재: ${cur:-없음})"
    ap_start "$ch"
}

# 라디오는 하나뿐이다. STA가 다른 채널의 공유기에 붙으면 드라이버에 따라 AP가
# 조용히 멈추거나 따라 옮겨간다. AP는 복구 경로이므로 여기서 반드시 확인하고,
# 죽었으면 STA 채널로 다시 세운다.
reconcile_ap_channel() {
    ch=$(sta_channel)
    [ -n "${ch:-}" ] || return 0

    cur=$(iw dev $AP_IF info 2>/dev/null | sed -n 's/.*channel \([0-9]*\).*/\1/p' | head -1)
    if ap_running && [ "${cur:-}" = "$ch" ]; then
        return 0
    fi
    log "AP를 STA 채널($ch)로 맞춥니다 (현재 채널: ${cur:-없음})"
    ap_start "$ch"
}

# ---------------------------------------------------------------- 상태

# key=value 줄로 낸다. 프로비저닝 데몬이 이걸 읽어 JSON으로 바꾼다 — 셸에서 JSON을
# 만들면 이스케이프를 두 곳에서 관리하게 된다.
status() {
    ensure_defaults
    echo "ap_if=$AP_IF"
    if ap_running; then echo "ap_up=1"; else echo "ap_up=0"; fi
    echo "ap_ssid=$(read_cfg ap.ssid)"
    echo "ap_addr=$AP_ADDR"
    # ap_up=1만으로는 붙을 수 있다는 뜻이 아니다. 주소가 없거나 DHCP가 죽어 있으면
    # SSID는 보이는데 단말이 주소를 못 받아 결합 직후 떨어진다 — 이걸 못 보고 있었다.
    if ap_has_addr; then echo "ap_addr_ok=1"; else echo "ap_addr_ok=0"; fi
    if dnsmasq_running; then echo "ap_dhcp=1"; else echo "ap_dhcp=0"; fi
    echo "ap_channel=$(iw dev $AP_IF info 2>/dev/null | sed -n 's/.*channel \([0-9]*\).*/\1/p' | head -1)"
    echo "ap_clients=$(iw dev $AP_IF station dump 2>/dev/null | grep -c '^Station')"

    echo "sta_if=$STA_IF"
    sta_ssid=$(read_cfg sta.ssid)
    if [ -n "$sta_ssid" ]; then echo "sta_configured=1"; else echo "sta_configured=0"; fi
    echo "sta_ssid=$sta_ssid"
    echo "sta_addr=$(sta_addr)"
    # supplicant가 아직 살아 있는지. 이게 없으면 "붙는 중"과 "포기했음"을 구분할 수
    # 없어서, 없는 공유기를 설정해둔 보드가 영원히 연결 중으로 보인다.
    if [ -f "$RUN/wpa.pid" ] && kill -0 "$(cat "$RUN/wpa.pid" 2>/dev/null)" 2>/dev/null; then
        echo "sta_daemon=1"
    else
        echo "sta_daemon=0"
    fi
    wstate=$(wpa_cli -i $STA_IF status 2>/dev/null | sed -n 's/^wpa_state=//p')
    echo "sta_wpa_state=${wstate:-}"
    echo "sta_signal=$(iw dev $STA_IF link 2>/dev/null | sed -n 's/.*signal:[ \t]*\(-\?[0-9]*\).*/\1/p' | head -1)"
    echo "rtsp_port=554"
}

# ---------------------------------------------------------------- 감시

# AP는 복구 경로다. 그게 조용히 망가지면 남는 수단은 USB를 꽂는 것뿐이므로, 부팅 때
# 한 번 올리고 잘 되기를 바라는 것으로는 부족하다. rkipc는 우리와 무관하게 인터페이스에
# DHCP 클라이언트를 붙이고 udhcpc를 통째로 죽인다 — 그 옆에서 AP를 계속 붙잡고 있어야 한다.
WATCH_EVERY=15

watchdog() {
    log "감시 시작 (${WATCH_EVERY}초 주기)"
    while true; do
        sleep $WATCH_EVERY

        if ! ap_running; then
            log "감시: hostapd가 죽었습니다 — AP를 다시 세웁니다"
            ap_start
            continue
        fi
        if ! ap_has_addr; then
            log "감시: $AP_IF 주소가 사라졌습니다 — 다시 설정합니다"
            ap_ensure_addr
        fi
        if ! dnsmasq_running; then
            log "감시: dnsmasq가 죽었습니다 — 다시 띄웁니다"
            ap_ensure_dnsmasq
        fi

        # STA가 결합은 됐는데 주소가 없으면 임대만 다시 받는다. 여기서 sta_start를
        # 부르면 라디오를 내렸다 올리므로 붙어 있는 앱의 스트림이 끊긴다.
        if [ -n "$(read_cfg sta.ssid)" ] &&
            [ "$(wpa_cli -i $STA_IF status 2>/dev/null | sed -n 's/^wpa_state=//p')" = "COMPLETED" ] &&
            [ -z "$(sta_addr)" ]; then
            log "감시: STA는 붙었는데 주소가 없습니다 — 임대를 다시 받습니다"
            sta_ensure_lease || log "감시: 임대 재시도 실패"
        fi
    done
}

watchdog_stop() {
    kill_pidfile "$RUN/watchdog.pid"
    return 0
}

watchdog_start() {
    watchdog_stop

    # 그냥 `watchdog &`로 띄우면 부모 셸이 끝나는 순간 SIGHUP으로 같이 죽는다.
    # init 스크립트나 adb에서 부르면 부모는 바로 끝나므로, 세션에서 떼어내지 않으면
    # 감시가 시작 로그만 찍고 사라진다 — 실제로 그렇게 죽는 것을 보고 고쳤다.
    self=/usr/bin/rvcam-net.sh
    [ -x "$self" ] || self=$0
    if command -v setsid >/dev/null 2>&1; then
        setsid "$self" watchdog >/dev/null 2>&1 &
    else
        nohup "$self" watchdog >/dev/null 2>&1 &
    fi
    echo $! >"$RUN/watchdog.pid"
    return 0
}

case "${1:-}" in
    boot)
        # 순서가 핵심이다. AP가 먼저, 항상. STA는 백그라운드로 붙이므로 집 Wi-Fi가
        # 없는 장소에서도 부팅이 AP 앞에서 25초씩 멈추지 않는다.
        ap_start
        (sta_start || log "STA 없이 AP만으로 계속합니다") &
        watchdog_start
        ;;
    watchdog) watchdog ;;
    watchdog-start) watchdog_start ;;
    watchdog-stop) watchdog_stop ;;
    ap-start) ap_start "${2:-}" ;;
    ap-stop) ap_stop ;;
    sta-start) sta_start ;;
    sta-stop) sta_stop ;;
    sta-restart)
        sta_stop
        sta_start
        ;;
    status) status ;;
    stop)
        watchdog_stop
        sta_stop
        ap_stop
        ;;
    *)
        echo "사용법: $0 {boot|ap-start|ap-stop|sta-start|sta-stop|sta-restart|status|watchdog-start|watchdog-stop|stop}"
        exit 1
        ;;
esac
