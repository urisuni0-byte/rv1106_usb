#!/bin/bash
# fakecam 이 쓰는 시험 영상/사진을 만든다 (ffmpeg 필요).  bash make_media.sh <출력 디렉터리>
set -e
D=${1:-.}; FF=${FFMPEG:-ffmpeg}
mkdir -p "$D"
$FF -y -loglevel error -f lavfi -i testsrc2=size=640x480:rate=20 -t 10 -c:v libx264 -profile:v baseline \
    -preset ultrafast -tune zerolatency -x264-params aud=1:keyint=20 -b:v 2000k -pix_fmt yuv420p -f h264 "$D/preview.h264"
$FF -y -loglevel error -f lavfi -i testsrc2=size=2592x1944:rate=1,drawbox=c=white@0.35:t=fill -frames:v 1 "$D/white.jpg"
$FF -y -loglevel error -f lavfi -i testsrc2=size=2592x1944:rate=1,drawbox=c=purple@0.45:t=fill -frames:v 1 "$D/uv.jpg"
ls -la "$D"
