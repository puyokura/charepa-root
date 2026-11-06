#!/usr/bin/env bash
# hunt2.sh - walk the address space one window at a time until k178 reports win=1.
#
# Each window is a separate k178 process, so a window that kills the tablet costs one
# window and a power cycle, not the whole run. Device liveness is checked before every
# window, and the window list is ordered by how often the AVC node landed there during
# the session (42000000 first, then outward).
#
# Usage:
#     ADB_SERIAL=<serial> ./hunt2.sh [workdir] [logfile]
#
#   ADB_SERIAL  adb serial of the tablet (required). `adb devices` prints it.
#   workdir     directory holding the built k178 binary (default: .)
#   logfile     where to append progress (default: ./hunt2.log)
#
# The script never assumes sudo: set ADB_SUDO=1 if adb needs elevation on your host.
set -u

: "${ADB_SERIAL:?set ADB_SERIAL to the adb serial shown by 'adb devices'}"
WORKDIR="${1:-.}"
LOG="${2:-./hunt2.log}"

ADB=(adb)
if [ "${ADB_SUDO:-0}" = "1" ]; then
  ADB=(sudo -n adb)
fi

[ -x "$WORKDIR/k178" ] || { echo "k178 not found in $WORKDIR"; exit 2; }
mkdir -p "$(dirname "$LOG")"
exec >>"$LOG" 2>&1
echo "=== hunt2 $(date) serial=$ADB_SERIAL workdir=$WORKDIR ==="

wait_dev() {
  for _ in $(seq 1 60); do
    if "${ADB[@]}" devices 2>/dev/null | grep -q "$ADB_SERIAL	device"; then
      if timeout 8 "${ADB[@]}" exec-out "cat /proc/uptime" </dev/null 2>/dev/null | grep -q .; then
        return 0
      fi
    fi
    sleep 10
  done
  return 1
}

for lo in 42000000 4a000000 52000000 5a000000 62000000 02000000 0a000000 \
          12000000 1a000000 2a000000 32000000 3a000000 22000000 6a000000 72000000; do
  hi=$(printf '%08x' $((0x$lo + 0x08000000)))
  echo "--- window $lo..$hi $(date) ---"
  wait_dev || { echo "no device"; continue; }
  sleep 5
  "${ADB[@]}" push "$WORKDIR/k178" /data/local/tmp/k178 </dev/null 2>/dev/null
  "${ADB[@]}" shell "chmod 755 /data/local/tmp/k178" </dev/null 2>/dev/null
  timeout 300 "${ADB[@]}" shell "cd /data/local/tmp; ./k178 $lo $hi" </dev/null 2>/dev/null
  sleep 2
  R=$(timeout 20 "${ADB[@]}" exec-out "cat /data/local/tmp/k178.txt" </dev/null 2>/dev/null | grep -a "^RESULT")
  echo "$R"
  case "$R" in *win=1*) echo "*** WIN $lo ***"; exit 0;; esac
done
echo "=== no win $(date) ==="
exit 1
