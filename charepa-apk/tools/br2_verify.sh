#!/usr/bin/env bash
# br2_verify.sh - run the five checks on a TAB-A03-BR2
#
# Refuses to run unless ro.product.model is TAB-A03-BR2, so it cannot be
# pointed at a TAB-A03-BS by mistake (measured: on a BS it prints
# "NOT BR2: connected device is TAB-A03-BS - refusing to run" and exits 2).
#
# Steps: validate (no side effects), a first-root loop using only the APK's own
# staged binaries, the hook state, the app reaching root, and a reboot.
#
# The first-root command is the bare `all`. An explicit
# "02000000 40000000 2048 2600" was measured to fail: that range excludes
# physical memory above 1 GB, and with it most runs ended in
# "RESULT no cred", where the iomem-derived range found the cred repeatedly.
#
# Untested on BR2: nothing in this file has been run against one yet.

set -u
P=/sys/bus/usb/devices/usb1/1-0:1.0/usb1-port2
APK="$(cd "$(dirname "$0")/.." && pwd)/charepa.apk"

D() { timeout 600 sudo -n adb shell "$1" < /dev/null 2>&1 | tr -d '\r'; }

ensure() {
  for t in $(seq 1 6); do
    if sudo -n adb devices < /dev/null 2>&1 | grep -q 'device$'; then return 0; fi
    sudo -n adb kill-server >/dev/null 2>&1; sleep 2
    sudo -n adb start-server >/dev/null 2>&1; sleep 6
    if sudo -n adb devices < /dev/null 2>&1 | grep -q 'device$'; then return 0; fi
    sudo -n bash -c "echo 1 > $P/disable" 2>/dev/null; sleep 2
    sudo -n bash -c "echo 0 > $P/disable" 2>/dev/null; sleep 8
  done
  sudo -n adb devices < /dev/null 2>&1 | grep -q 'device$'
}

hook_installed() {
  local out
  out=$(D 'test -f /system/bin/debuggerd.real && test -f /system/lib/modules/myroot.ko && echo YES')
  [ "$(echo "$out" | tail -1)" = "YES" ]
}

ensure || { echo "no device"; exit 1; }

MODEL=$(D 'getprop ro.product.model')
SERIAL=$(D 'getprop ro.serialno')
BUILD=$(D 'getprop ro.build.display.id')
HW=$(D 'getprop ro.hardware')
echo "model=$MODEL serial=$SERIAL build=$BUILD hardware=$HW"

if [ "$MODEL" != "TAB-A03-BR2" ]; then
  echo "NOT BR2: connected device is $MODEL - refusing to run"
  exit 2
fi

echo '== 1. validate (no side effects) =='
sudo -n adb install -r -d "$APK" < /dev/null 2>&1 | tail -2
sleep 3
D 'pm list packages | grep charepa'
D 'am force-stop com.charepa'
D 'am start -W -n com.charepa/.MainActivity --es mode validate' | tail -2
sleep 12
D 'run-as com.charepa cat /data/data/com.charepa/files/last.log' | tail -8

echo '== 2. first root loop, using the APK only =='
for i in $(seq 1 40); do
  ensure || { echo "  lost the device at attempt $i"; continue; }
  if hook_installed; then echo "*** hook present before attempt $i ***"; break; fi
  # Re-stage from the APK. One sweep corrupts the staged myroot.ko (measured on
  # the BS: fnv 0x6b973846fa5576b0 -> 0x46eee07d4bcb11a4), and every later
  # init_module then answers ENOEXEC. stage() rewrites files/bin on each launch.
  D 'am force-stop com.charepa' >/dev/null 2>&1
  D 'am start -W -n com.charepa/.MainActivity --es mode validate' >/dev/null 2>&1
  sleep 8
  D 'run-as com.charepa /data/data/com.charepa/files/bin/charepa all' > "/tmp/br2_$i.txt" 2>&1
  printf '%s: ' "$i"
  grep -aE 'module_errno=|AVC win|init_module|hook:|RESULT' "/tmp/br2_$i.txt" 2>/dev/null | tr '\n' ' '
  echo
  ensure
  if hook_installed; then echo "*** hook installed at attempt $i (verified on the device) ***"; break; fi
done

echo '== 3. hook state =='
D 'ls -l /system/bin/debuggerd /system/bin/debuggerd.real /system/lib/modules/myroot.ko' 2>&1
D 'ls -lZ /system/bin/debuggerd /system/bin/debuggerd.real' 2>&1
D 'ls -l /proc/myroot' 2>&1
D 'getenforce'

echo '== 4. the app reaches root =='
D 'am force-stop com.charepa'
D 'am start -W -n com.charepa/.MainActivity --es mode all' | tail -2
sleep 14
D 'run-as com.charepa cat /data/data/com.charepa/files/last.log' | tail -10

echo '== 5. persistence across a reboot =='
D 'reboot' >/dev/null 2>&1
sleep 30
ensure
sleep 40
D 'ls -l /proc/myroot' 2>&1
D 'dmesg' | grep -aE 'CHAREPA_BOOT' | tail -6
D 'echo 1 > /proc/myroot; id; getenforce'
