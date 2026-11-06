#!/usr/bin/env bash
# Simulate the CI assertions against the checked-out tree.
set -eu
cd "$(dirname "$0")/.."

echo "== tools =="
rm -rf build
make -C . tools >/dev/null 2>&1
count=0
for f in build/*; do
  file "$f" | grep -q 'ARM aarch64' || { echo "$f is not aarch64"; exit 1; }
  file "$f" | grep -q 'statically linked' || { echo "$f is not static"; exit 1; }
  count=$((count + 1))
done
echo "verified $count binaries"

echo "== module identity =="
test -f src/prebuilt/myroot.ko
sha256sum src/prebuilt/myroot.ko
strings -a src/prebuilt/myroot.ko | grep -q 'vermagic=3.10.33'
echo "vermagic ok"

echo "== hunter syntax =="
bash -n src/tools/hunt2.sh
echo "hunt2.sh parses"

echo "== document fences =="
for f in docs/*.md README.md CONTRIBUTING.md; do
  n=$(grep -c '^```' "$f" || true)
  if [ $((n % 2)) -ne 0 ]; then echo "$f has an unclosed fence ($n)"; exit 1; fi
  echo "$f: $n fences"
done

echo "== no host identifiers =="
files=$(git ls-files | grep -v '\.ko$')
if echo "$files" | xargs grep -l -E 'C:\\Users|/home/|192\.168\.' 2>/dev/null | grep -q .; then
  echo "host path leak"; exit 1
fi
echo "clean"
