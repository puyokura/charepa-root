#!/usr/bin/env bash
# fake adb: a stand-in that pretends to be a TAB-A03-BR2 so the runbook's own
# control flow can be exercised without the hardware.
#
# This is a SIMULATION. It proves nothing about a real BR2; it only proves the
# script does the right sequence: model guard, validate, first-root loop that
# gives up after N attempts, hook detection, and the reboot step.
#
# State lives in /tmp/br2sim/state so attempts accumulate across invocations.
set -u
S=/tmp/br2sim/state
mkdir -p "$S"
[ -f "$S/hook" ] || echo 0 > "$S/hook"
[ -f "$S/attempts" ] || echo 0 > "$S/attempts"

cmd="${1:-}"
shift 2>/dev/null || true

case "$cmd" in
  devices)
    echo "List of devices attached"
    echo -e "SIMBR2\tdevice"
    ;;
  version) echo "Android Debug Bridge version 1.0.41" ;;
  kill-server|start-server) : ;;
  install) echo "Success" ;;
  reboot)
    # a reboot means the hook survived: keep the flag, bump nothing
    echo "rebooting"
    ;;
  shell)
    line="${1:-}"
    case "$line" in
      *ro.product.model*) echo "TAB-A03-BR2" ;;
      *ro.serialno*)      echo "SIM00000001" ;;
      *ro.build.display*) echo "02.11.000 release-keys" ;;
      *ro.hardware*)      echo "pxa1928" ;;
      *"pm list packages"*) echo "package:com.charepa" ;;
      *"test -f /system/bin/debuggerd.real"*)
        # the loop's success condition, driven by the state file
        if [ "$(cat "$S/hook")" = "1" ]; then echo YES; fi ;;
      *"charepa all"*)
        n=$(( $(cat "$S/attempts") + 1 ))
        echo "$n" > "$S/attempts"
        # simulate: the sweep wins on the third attempt
        if [ "$n" -ge 3 ]; then
          echo 1 > "$S/hook"
          echo "cred=0xffffffc012345680 eff=0x80010000 module_errno=8"
          echo "after avc: module_errno=8 (8 = capability check passed)"
          echo "init_module rc=0 errno=0 (156576 bytes)"
          echo "hook: target=/system/bin/calibinit.sh (real=/system/bin/calibinit.sh.real)"
          echo "RESULT ROOT uid=0 eff=0xffffffff (persistent)"
        else
          echo "cred=0xffffffc012345680 eff=0x80010000 module_errno=1"
          echo "RESULT avc failed"
        fi
        ;;
      *"getenforce"*) echo "Enforcing" ;;
      *"ls -l"*) echo "-rwxr-xr-x root root 713240 /system/bin/debuggerd" ;;
      *"cat "*last.log*) echo "RESULT ROOT uid=0 (persistent)" ;;
      *) : ;;
    esac
    ;;
  *) : ;;
esac
