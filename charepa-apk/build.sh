#!/usr/bin/env bash
# Build the APK without Gradle: aapt2, javac, d8, apksigner.
#
#   build.sh <output.apk> [ndk-gcc]
#
# The native chain is compiled for aarch64 statically and shipped in assets/arm64,
# because the tablet must run it as its own process.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$ROOT/charepa.apk}
CC=${2:-aarch64-linux-gnu-gcc}
BT=${BT:-/tmp/sdk/bt/android-11}
AAPT2=${AAPT2:-$BT/aapt2}
D8=${D8:-$BT/d8}
APKSIGNER=${APKSIGNER:-$BT/apksigner}
ANDROID_JAR=${ANDROID_JAR:-/tmp/sdk/plat/android-11/android.jar}
KEYSTORE=${KEYSTORE:-$ROOT/debug.keystore}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "== native =="
ASSETS="$ROOT/app/src/main/assets/arm64"
mkdir -p "$ASSETS"
for src in charepa bootpayload; do
  "$CC" -static -O2 -Wall -o "$ASSETS/$src" "$ROOT/jni/$src.c"
  printf '  %-12s ' "$src"; file "$ASSETS/$src" | sed 's/.*: //; s/, BuildID.*//'
done

# the module is prebuilt: it needs the vendor kernel tree, which CI does not have
install -m 0644 "$ROOT/prebuilt/myroot.ko" "$ASSETS/myroot.ko"
printf '  %-12s %s bytes\n' myroot.ko "$(wc -c < "$ASSETS/myroot.ko")"

echo "== resources =="
mkdir -p "$WORK/res"
"$AAPT2" compile --dir "$ROOT/app/src/main/res" -o "$WORK/res.zip"
"$AAPT2" link -o "$WORK/base.apk" \
  -I "$ANDROID_JAR" \
  --manifest "$ROOT/app/AndroidManifest.xml" \
  --java "$WORK/gen" \
  --min-sdk-version 22 --target-sdk-version 22 \
  -A "$ROOT/app/src/main/assets" \
  "$WORK/res.zip"

echo "== java =="
mkdir -p "$WORK/classes"
SRC=$(find "$ROOT/app/src/main/java" "$WORK/gen" -name '*.java')
javac -source 8 -target 8 -bootclasspath "$ANDROID_JAR" -d "$WORK/classes" $SRC
CLASSES=$(find "$WORK/classes" -name '*.class')

echo "== dex =="
mkdir -p "$WORK/dex"
"$D8" --min-api 22 --lib "$ANDROID_JAR" --output "$WORK/dex" $CLASSES

echo "== package =="
cp "$WORK/base.apk" "$WORK/unsigned.apk"
(cd "$WORK/dex" && zip -q -X "$WORK/unsigned.apk" classes.dex)
"$BT/zipalign" -f 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"

if [ ! -f "$KEYSTORE" ]; then
  echo "== debug keystore =="
  keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias charepa -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=charepa, OU=root, O=charepa, L=, ST=, C=JP" >/dev/null 2>&1
fi

"$APKSIGNER" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --min-sdk-version 22 --out "$OUT" "$WORK/aligned.apk"
"$APKSIGNER" verify --min-sdk-version 22 "$OUT"
echo "== done =="
ls -l "$OUT"
