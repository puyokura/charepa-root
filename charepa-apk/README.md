# charepa-apk

Root on the Benesse study tablets (TAB-A03-BS, TAB-A03-BR2) from an ordinary
installed app, and root again after every reboot. For devices you own.

## What is verified, and on what

Everything below was measured on a TAB-A03-BS (Android 5.1, kernel 3.10.33
#2 SMP PREEMPT, Marvell PXA1928, SELinux enforcing).

| Claim | Evidence |
| --- | --- |
| Root from the adb shell context | `charepa all` reached `init_module rc=0`, then `/proc/myroot` gave `uid=0(root)`, `Permissive` |
| Root survives a reboot | ten reboots, power cuts and USB drops: each time the payload ran in `u:r:init:s0`, `init_module rc=0`, recreated `/proc/myroot`, and one byte through it gave `uid=0` |
| Root survives deleting the app | `pm uninstall com.charepa`, then a reboot: `/proc/myroot` came back and answered, with no APK installed at all |
| The app itself can reach root | `am start --es mode all` ended with `RESULT ROOT uid=0`, from a process whose own `CapBnd` is `0000000000000000` |
| Running twice is safe | a unit whose hook is already installed answers `EEXIST` from `init_module`; that now counts as success, so the second run reports `already rooted` and `RESULT ROOT uid=0` instead of a false failure |
| The 4-byte write works at all | `charepa validate` prints `VALIDATE PASS` from inside the app, including on the binary CI produced |

The shipped `charepa.apk` was measured as the whole deliverable, twice.

Start from an unrooted unit — `/system/bin/debuggerd` back to its original,
`/system/lib/modules/myroot.ko` absent, no `/proc/myroot`, `Enforcing` — and
install only the APK. Then run its own staged copy:

```
run-as com.charepa /data/data/com.charepa/files/bin/charepa all \
        02000000 40000000 2048 2600
```

With nothing pushed from the host, attempt 6 reached the whole chain:

```
cred=0xffffffc02c8fcc00 eff=0x80010000 module_errno=1
AVC win phys=0x2c68cc00 off=704 probes=746508 stores=2415
after avc: module_errno=8 (8 = capability check passed)
init_module rc=0 errno=0 (156576 bytes)
hook: /system is rw
hook: /system/bin/debuggerd relabelled to u:object_r:system_file:s0
```

A reboot then brought root back through the payload:

```
CHAREPA_BOOT: context=u:r:init:s0
CHAREPA_BOOT: init_module rc=0 errno=0 (Success) bytes=156576
CHAREPA_BOOT: write /proc/myroot rc=1 errno=0 uid now 0
```

and `echo 1 > /proc/myroot` answered `uid=0(root) ... Permissive`.

TAB-A03-BR2 is **not** verified. The partitions are the same shape as BS
(`system` p16, `boot` p15, `recovery` p7), and the two models share USB ID
`0bb4:4e16`, the kernel version and the platform, but nothing has been run on
one. The workflow refuses to start unless `ro.product.model` is
`TAB-A03-BR2`, so it cannot be pointed at a BS by mistake.

## The two halves, and why the order matters

The chain has a half that needs kernel memory write, and a half that does not.

**The first root needs the sweep.** `find_cred` looks for a cred whose
`cap_bset` is `192` (`0xc0`, the value an unprivileged cred carries) and writes
the full capability mask into `cap_effective`. That alphabet has to fit in the
descriptor table, which is why it is `192` and not the process's uid. It is
present in the `shell`/`run-as` context (`CapBnd = 0xc0`) and **absent in an
app process** (`CapBnd = 0`), so the first root is driven from the adb shell
context and not from the app.

After `find_cred` the same process calls `init_module` once so SELinux caches
the capability-class `avc_node`, then finds that node and sets bit 16 in
`avc_decision.allowed`. The winning node sits about 64 MB below the cred; one
measured attempt gated a single node (`stores=1`) and the capability check
passed (`module_errno=8`).

**Every later root needs no sweep at all.** Once the hook is installed the
module is loaded at boot, so `/proc/myroot` already exists. Writing one byte to
it replaces the caller's credential with one cloned from `init_cred`. That is
what the app does, and it works from `CapBnd = 0`.

## What the payload does at boot

`/system/bin/debuggerd` is replaced by a static binary and relabelled
`u:object_r:system_file:s0`. A label with no exec type means no SELinux type
transition, so init starts the child in its own domain, which this policy does
not restrict. The original binary is kept as `debuggerd.real` and the payload
execs it afterwards.

`debuggerd.real` gets its own label back, `u:object_r:debuggerd_exec:s0`. Without
that the copy keeps the payload's `system_file` label, and the kernel logs

```
avc: denied { execute_no_trans } for pid=196 comm="debuggerd"
     path="/system/bin/debuggerd.real" scontext=u:r:kernel:s0
     tcontext=u:object_r:system_file:s0 tclass=file permissive=1
```

so the daemon starts in the wrong domain. It does not stop crash handling
outright — this unit writes tombstones and `debuggerd` holds its two crash
sockets either way — but starting it in the domain init intended is the correct
arrangement, and the fix is one `setxattr`.

Measured banner on every boot:

```
CHAREPA_BOOT: context=u:r:init:s0
CHAREPA_BOOT: write enforce=0 rc=-1 errno=13
CHAREPA_BOOT: init_module rc=0 errno=0 (Success) bytes=156576
CHAREPA_BOOT: write /proc/myroot rc=1 errno=0 uid now 0
```

The `enforce` write fails with EACCES in the init domain and is kept only as a
record; the module sets enforcement itself.

## Crash handling on this unit

Two measurements, because the first conclusion drawn from them was wrong.

`/data/tombstones` holds real captures written **after** the hook was installed:
`tombstone_05` at 04:33 is a `SIGSEGV` in `com.instagram.android`, and
`tombstone_02` at 04:29 is an `SIGABRT` from `dex2oat`. So crash reporting does
work with the payload in place.

What does not work is the `core_pattern` route: `init.rc` only sets
`|/system/bin/core-compressor %e %t %p` under
`on property:persist.sys.debug.coredump=enable`, that property is not set, the
binary `core-compressor` does not exist on this image, and
`/proc/sys/kernel/core_pattern` reads `core`. Crash reporting here runs through
`debuggerd`'s own sockets, not through a core pipe.

The wrong conclusion was worth keeping: a `kill -SEGV` on `sh` produced no
tombstone, and that was read as the hook breaking crash handling. `sh` is not a
zygote-managed process, so nothing captured it — the test was invalid, not the
hook.

## A trap worth recording

The sweep writes the carrier's IOVA into every word it matches, and one matched
word belonged to the page cache of `/data/local/tmp/myroot.ko`. The file's hash
moved from `507ebb47…` to `ddfddab6…`, with `0x00018000` replaced by
`0x80010000`, and `init_module` then answered `ENOEXEC` for a file that loads
fine at boot.

Both files the chain needs are therefore read into memory **before** the sweep
starts (`preload_chain`), and nothing is opened afterwards. When a run reaches
`module_errno=8` without needing an AVC win, the sweep is skipped entirely.

## Build

No Gradle. `build.sh` runs `aarch64-linux-gnu-gcc` for the two static binaries,
`aapt2` for resources, `javac` and `d8` for the dex, and `apksigner` at the end.

```sh
BT=<build-tools> ANDROID_JAR=<platform>/android.jar ./build.sh charepa.apk
```

`.github/workflows/apk.yml` at the repository root does the same on a stock
runner and uploads the APK as an artifact, plus a release asset for tags. The
workflow lives at the root because GitHub only picks up `/.github/workflows`;
a copy under `charepa-apk/` was never loaded (`gh workflow list` showed only
the root `build` job).

CI does not reproduce this repository's `charepa.apk` byte for byte, and the
measured hashes say why: the runner compiles with a newer gcc than the machine
used here, so `assets/arm64/charepa` comes out as d4239894… instead of
5ee0c4e9… (bootpayload differs the same way). The module is the prebuilt one
and does match, 507ebb47…. The APK kept in this directory is the build that was
measured on the tablet; treat a CI artifact as a fresh build of the same
source, not as the same bytes.

## Use

Install the APK. That is the entire installation step: the Activity copies its
three assets into `files/bin` on launch and the chain finds its companions
there through `/proc/self/exe`.

First root, on a unit with no hook yet. Start the app once so it stages the
files, then run the staged copy — measured, nothing needs to be pushed:

```sh
adb shell am start -n com.charepa/.MainActivity --es mode validate
adb shell 'run-as com.charepa /data/data/com.charepa/files/bin/charepa all'
```

Pass no range. The chain derives it from `/proc/iomem`, and an explicit
`02000000 40000000` was measured to exclude everything above 1 GB: with that
range most runs ended in `RESULT no cred`, while the derived range found the
cred seven times in a row and then five more.

The arguments, in order: `all [lo hi [window budget avclo avchi]]`, all optional.
The store budget is the *fifth*, not the fourth (the fourth is the page window,
default 2048). The default budget is 8000: runs that won used 392, 928, 1005 and
2415 stores, while runs that spent 2069 and 2387 and then stopped at the old
2600 cap reported `AVC budget spent ... AVC no win`. The node moves between
boots, so the budget has to cover the window rather than be tuned to one boot.

The sweep can miss: where the `avc_node` lands moves between boots, and a run
that stores into the wrong place reboots the tablet. Measured wins came at
attempts 5, 6, 7 and 23 of their loops, so repeat the second command until the
log says `init_module rc=0`. A run that reports `module_errno=8` has already
passed the capability check and goes straight to `init_module`.

Two things make a run fail that look like success. The sweep rewrites the page
cache holding `myroot.ko`, so a module pushed once goes bad after the first
attempt and every later `init_module` answers `ENOEXEC`; push a fresh copy
before each attempt, or watch the `load: fnv=` line for `MISMATCH`. And
`install_hook` only flushes at the end, so a run that dies inside it can leave
nothing behind after a reboot; the files are now `fsync`ed individually.

Once the hook is in, the app is the entry point:

```sh
adb shell am start -n com.charepa/.MainActivity --es mode all
```

It reports `RESULT ROOT uid=0` and exits 0. From then on any process that can
write one byte to `/proc/myroot` is root:

```sh
adb shell 'echo 1 > /proc/myroot; id'
```

`validate` only touches memory the process owns and prints PASS or FAIL, which
is the safe way to check the primitive on a given unit.

### When the USB connection drops

Measured repeatedly on this unit: the tablet disappears from `adb devices` while
a sweep runs, and `adb kill-server` does not bring it back. Cycling the root-hub
port does, in one try:

```sh
echo 1 > /sys/bus/usb/devices/usb1/1-0:1.0/usb1-port2/disable
sleep 2
echo 0 > /sys/bus/usb/devices/usb1/1-0:1.0/usb1-port2/disable
```

The device comes back with root intact; a `reboot` is not needed.

### What breaks adb, and what does not

Removing `/data` by hand while the framework is not running is enough to lose
adb until the tablet is restarted from its own screen. Measured here: after
`/data/app`, `/data/data`, `/data/system` and `/data/property` were deleted with
the device rooted, `pm list packages` returned zero, and on the next boot the
USB identity changed from `0bb4:4e16` (adb + MTP) to `0474:0317` with a single
interface, `iInterface = MTP`. `adb devices` and `fastboot devices` both stayed
empty through 10 minutes of polling and a port cycle, because the properties
that ask adbd to start were part of what had been deleted.

A factory reset clears `/data` and then lets the framework rebuild those
properties; deleting them from outside skips that step. Keeping `/data/misc/adb`
preserves the host's authorisation key, so adb returns without a new on-device
tap once the tablet boots normally again.

The recovery route is not available on this unit either: `misc` accepts a
`bootloader_message` (verified by writing `boot-recovery` at offset 0 and
`--wipe_data` at offset 64 with `pwrite`, then reading both back), but a reboot
leaves the block untouched and boots Android, so the bootloader here does not
consult it. `am broadcast MASTER_CLEAR` returns `result=0` with no receiver, and
there is no `/system/bin/recovery` inside the system image.

## Layout

```
jni/charepa.c            the chain, ending with the hook install
jni/bootpayload.c        what runs at boot from init's domain
prebuilt/myroot.ko       the module, built offline against the vendor tree
app/                     the Activity that stages and runs the binaries
tools/br2_verify.sh      the five checks for a BR2, refusing any other model
charepa.apk              the built, signed deliverable
```

## Licence

MIT, see `LICENSE`.
