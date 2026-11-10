#!/usr/bin/env bash
# consistency.sh - check that the documented defaults match the code, that the
# shipped APK carries the assets it claims, and that every file the workflows
# reference exists.
#
# Written after two rounds of README-versus-code mismatches: the store budget
# was described as the fourth argument (it is the fifth), and the default was
# 2600 where the measured runs needed 8000.
#
# Run from the repository root: bash charepa-apk/tools/consistency.sh
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC="$ROOT/charepa-apk/jni/charepa.c"
README="$ROOT/charepa-apk/README.md"
APK="$ROOT/charepa-apk/charepa.apk"
fail=0

echo '== defaults: code vs README =='
win=$(grep -o 'int window = [0-9]*' "$SRC" | awk '{print $4}')
bud=$(grep -o 'long budget = [0-9]*' "$SRC" | awk '{print $4}')
echo "  code: window=$win budget=$bud"
if grep -q "default $win" "$README" && grep -q "default budget is $bud" "$README"; then
  echo "  README agrees"
else
  echo "  README does NOT agree"; fail=1
fi

echo '== the shipped APK carries these assets =='
if [ -f "$APK" ]; then
  work=$(mktemp -d)
  (cd "$work" && unzip -q "$APK")
  for f in charepa bootpayload myroot.ko; do
    a=$(sha256sum "$work/assets/arm64/$f" | cut -d' ' -f1)
    echo "  $f $a"
    if [ "$f" = myroot.ko ]; then
      b=$(sha256sum "$ROOT/charepa-apk/prebuilt/myroot.ko" | cut -d' ' -f1)
      [ "$a" = "$b" ] && echo "    matches prebuilt" || { echo "    DIFFERS from prebuilt"; fail=1; }
    fi
  done
  rm -rf "$work"
else
  echo "  APK missing"; fail=1
fi

echo '== files the workflows reference =='
for f in charepa-apk/build.sh charepa-apk/app/AndroidManifest.xml \
         charepa-apk/prebuilt/myroot.ko src/prebuilt/myroot.ko \
         src/tools/hunt2.sh Makefile docs/README.md; do
  if [ -e "$ROOT/$f" ]; then echo "  ok      $f"; else echo "  MISSING $f"; fail=1; fi
done

echo
[ "$fail" = 0 ] && echo "RESULT: consistent" || echo "RESULT: problems found"
exit "$fail"
