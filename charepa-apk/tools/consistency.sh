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

echo '== the FNV constant guard matches the prebuilt module =='
# charepa refuses to hand the kernel a module whose FNV-1a 64 differs from
# KO_FNV. That constant is a literal in the source, so it goes stale silently if
# the module is ever rebuilt. Check it here rather than find out on a unit.
mod="$ROOT/charepa-apk/prebuilt/myroot.ko"
if [ -f "$mod" ] && command -v python3 >/dev/null 2>&1; then
  want=$(grep -o '#define KO_FNV 0x[0-9a-f]*' "$SRC" | awk '{print $3}')
  real=$(python3 - "$mod" <<'PY'
import sys
h = 0xcbf29ce484222325
with open(sys.argv[1], 'rb') as f:
    while True:
        b = f.read(8192)
        if not b: break
        for c in b:
            h ^= c
            h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
print("0x%016x" % h)
PY
)
  echo "  code: $want"
  echo "  file: $real"
  if [ "$want" = "$real" ]; then echo "  matches"; else echo "  MISMATCH"; fail=1; fi
else
  echo "  skipped (module or python3 missing)"
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
         src/tools/hunt2.sh Makefile docs/README.md \n         charepa-apk/tools/target_test.c; do
  if [ -e "$ROOT/$f" ]; then echo "  ok      $f"; else echo "  MISSING $f"; fail=1; fi
done

echo '== docs/README.md index: lines and bytes per file =='
# The index carried rounded K values and was measured wrong for two files
# (3,539 listed where the file had 3,538 lines, 139K where it had 139,470 bytes).
#
# This block is deliberately small. The first version pulled the numbers apart
# with sed and awk field splitting, and when fed a rewritten index it matched
# thousands of lines and reported "ok" for the same file seven times. Fixed names
# and two plain greps are harder to get wrong than a general parser.
INDEX="$ROOT/docs/README.md"
stale=0
if [ -f "$INDEX" ]; then
  for name in charepa-root-full-story.md charepa-root-source-code.md br2-compatibility.md; do
    p="$ROOT/docs/$name"
    if [ ! -f "$p" ]; then echo "  MISSING docs/$name"; stale=1; continue; fi
    row=$(grep -F "\`$name\` |" "$INDEX" | head -1)
    if [ -z "$row" ]; then echo "  NOT IN INDEX $name"; stale=1; continue; fi
    # "| `name` | 3,538 | 153,575 |" -> the two numbers
    stated_lines=$(printf '%s' "$row" | cut -d'|' -f3 | tr -d ' ,')
    stated_bytes=$(printf '%s' "$row" | cut -d'|' -f4 | tr -d ' ,')
    real_lines=$(wc -l < "$p")
    real_bytes=$(wc -c < "$p")
    if [ "$stated_lines" = "$real_lines" ] && [ "$stated_bytes" = "$real_bytes" ]; then
      echo "  ok      $name ($real_lines lines, $real_bytes bytes)"
    else
      echo "  STALE   $name: index says $stated_lines lines / $stated_bytes bytes"
      echo "                  file has $real_lines lines / $real_bytes bytes"
      stale=1
    fi
  done
else
  echo "  docs/README.md missing"; fail=1
fi
[ "$stale" = 0 ] || fail=1

echo
[ "$fail" = 0 ] && echo "RESULT: consistent" || echo "RESULT: problems found"
exit "$fail"
