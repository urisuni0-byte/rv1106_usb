#!/bin/sh
# stream.conf 를 읽어 uvc_stream.py 를 실행한다.
# 수동 실행:  ./start.sh            (Ctrl+C 로 종료)
# 설정 무시:  ./start.sh -W 1920 -H 1080 -f 30
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
CONF="${UVC_STREAM_CONF:-$HERE/stream.conf}"

DEVICE=""; WIDTH=1280; HEIGHT=720; FPS=30; PORT=8090
BIND=0.0.0.0; BUFFERS=4; AUDIO=1; AUDIO_RATE=48000
PHOTO_MAX=2592x1944; WEBRTC=0
[ -f "$CONF" ] && . "$CONF"

PY=$(command -v python3 || command -v python || true)
if [ -z "$PY" ]; then
    echo "오류: python3 이 없습니다." >&2
    exit 1
fi

set -- -W "$WIDTH" -H "$HEIGHT" -f "$FPS" -p "$PORT" -b "$BIND" \
       --buffers "$BUFFERS" --audio-rate "$AUDIO_RATE" --photo-max "$PHOTO_MAX" "$@"
[ -n "$DEVICE" ] && set -- -d "$DEVICE" "$@"
[ "$AUDIO" = "1" ] || set -- --no-audio "$@"
[ "$WEBRTC" = "1" ] || set -- --no-webrtc "$@"

exec "$PY" "$HERE/uvc_stream.py" "$@"
