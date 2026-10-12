#!/usr/bin/env bash
# RV1106 USB 패드 앱 빌드 (Gradle 없이 Android SDK 빌드 도구만 사용)
#   bash android/build.sh            -> android/out/RV1106USB.apk
# 보드 AP 정보는 환경변수로 바꿀 수 있다 (기본값: RvCam 보드 기본 설정)
#   AP_SSID=RvCam-0b9b AP_PASSWORD=rvcam12345 bash android/build.sh
set -euo pipefail
cd "$(dirname "$0")"

SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
BT="$SDK/build-tools/37.0.0"
JAR="$SDK/platforms/android-34/android.jar"
export JAVA_HOME="${JAVA_HOME:-/c/Program Files/Android/Android Studio/jbr}"
export PATH="$JAVA_HOME/bin:$PATH"
EXE=""; [ -f "$BT/aapt2.exe" ] && EXE=".exe"
BAT=""; [ -f "$BT/d8.bat" ] && BAT=".bat"

SSID="${AP_SSID:-RvCam-0b9b}"
PW="${AP_PASSWORD:-rvcam12345}"
AP_IP="192.168.4.1"
PKG_DIR=com/rv1106/usbcam

rm -rf out build && mkdir -p out build/gen/$PKG_DIR build/classes build/res

# 1) 설정 상수
cat > build/gen/$PKG_DIR/Config.java <<EOF
package com.rv1106.usbcam;
final class Config {
    static final String AP_SSID = "$SSID";
    static final String AP_SSID_PREFIX = "RvCam-";
    static final String AP_PASSWORD = "$PW";
    static final String AP_IP = "$AP_IP";
}
EOF

# 2) 아이콘 (PNG 생성, PIL 이 없으면 res/ 에 있는 것을 그대로 쓴다)
python - <<'PY' || echo "아이콘 생성 건너뜀 (PIL 없음)"
from PIL import Image, ImageDraw
import os
for d, s in (("mdpi",48),("hdpi",72),("xhdpi",96),("xxhdpi",144),("xxxhdpi",192)):
    k = 8; S = s*k
    im = Image.new("RGBA", (S,S), (0,0,0,0)); g = ImageDraw.Draw(im)
    g.rounded_rectangle((0,0,S-1,S-1), radius=S*0.22, fill=(18,24,33,255))
    # 카메라 몸체 + 렌즈 (AuraCam 과 구분되게 주황색)
    g.rounded_rectangle((S*.16,S*.32,S*.84,S*.76), radius=S*.08, fill=(255,152,56,255))
    g.rounded_rectangle((S*.34,S*.24,S*.56,S*.36), radius=S*.03, fill=(255,152,56,255))
    g.ellipse((S*.34,S*.38,S*.66,S*.70), fill=(18,24,33,255))
    g.ellipse((S*.41,S*.45,S*.59,S*.63), fill=(232,236,241,255))
    # USB 표시 (작은 막대)
    g.rounded_rectangle((S*.66,S*.80,S*.84,S*.88), radius=S*.02, fill=(232,236,241,255))
    os.makedirs(f"res/mipmap-{d}", exist_ok=True)
    im.resize((s,s), Image.LANCZOS).save(f"res/mipmap-{d}/ic_launcher.png")
PY

# 3) 리소스 컴파일 + 링크
"$BT/aapt2$EXE" compile --dir res -o build/res.zip
"$BT/aapt2$EXE" link -I "$JAR" --manifest AndroidManifest.xml \
    --java build/gen -o build/base.apk build/res.zip --auto-add-overlay

# 4) Java -> class -> dex
javac -nowarn -Xlint:-options --release 8 -encoding UTF-8 \
    -classpath "$JAR" -d build/classes \
    $(find src build/gen -name '*.java')
"$BT/d8$BAT" --min-api 21 --lib "$JAR" --output build \
    $(find build/classes -name '*.class')

# 5) APK 조립, 정렬, 서명 (디버그 키)
cp build/base.apk build/unsigned.apk
python -c "import zipfile; z = zipfile.ZipFile('build/unsigned.apk', 'a', zipfile.ZIP_DEFLATED); z.write('build/classes.dex', 'classes.dex'); z.close()"
"$BT/zipalign$EXE" -f 4 build/unsigned.apk build/aligned.apk
KS="$HOME/.android/debug.keystore"
if [ ! -f "$KS" ]; then
    mkdir -p "$(dirname "$KS")"
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
"$BT/apksigner$BAT" sign --ks "$KS" --ks-pass pass:android \
    --out out/RV1106USB.apk build/aligned.apk
"$BT/apksigner$BAT" verify out/RV1106USB.apk
ls -la out/RV1106USB.apk
