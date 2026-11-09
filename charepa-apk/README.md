# charepa-apk

Root on the Benesse study tablets (TAB-A03-BS, TAB-A03-BR2) from an ordinary
installed app, and root again after every reboot. For devices you own.

## What is verified, and on what

Everything below was measured on a TAB-A03-BS (Android 5.1, kernel 3.10.33
#2 SMP PREEMPT, Marvell PXA1928, SELinux enforcing).

| Claim | Evidence |
| --- | --- |
| Root from the adb shell context | `charepa all` reached `init_module rc=0`, then `/proc/myroot` gave `uid=0(root)`, `Permissive` |
| Root survives a reboot | three consecutive reboots: the payload ran in `u:r:init:s0`, `init_module rc=0`, `/proc/myroot` recreated; one byte through it gave `uid=0` every time |
| The app itself can reach root | `am start --es mode all` ended with `RESULT ROOT uid=0`, from a process whose own `CapBnd` is `0000000000000000` |
| The 4-byte write works at all | `charepa validate` prints `VALIDATE PASS` from inside the app |

TAB-A03-BR2 is **not** verified. The partitions are the same shape as BS
(`system` p16, `boot` p15, `recovery` p7), but nothing has been run on one.

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
execs it afterwards, so crash handling still works.

Measured banner on every boot:

```
CHAREPA_BOOT: context=u:r:init:s0
CHAREPA_BOOT: write enforce=0 rc=-1 errno=13
CHAREPA_BOOT: init_module rc=0 errno=0 (Success) bytes=156576
CHAREPA_BOOT: write /proc/myroot rc=1 errno=0 uid now 0
```

The `enforce` write fails with EACCES in the init domain and is kept only as a
record; the module sets enforcement itself.

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

`.github/workflows/build.yml` does the same on a stock runner and uploads the
APK as an artifact, plus a release asset for tags.

## Use

Install the APK. First root, from an adb shell:

```sh
adb push charepa myroot.ko bootpayload /data/local/tmp/
adb shell 'run-as <any app you own> sh -c "cp /data/local/tmp/charepa $HOME/ && ..."' # see below
```

The chain reads its companions from the directory it lives in, falling back to
`/data/local/tmp`, so staging all three there is enough:

```sh
adb shell 'run-as com.termux /data/data/com.termux/files/charepa all 02000000 40000000 2048 2600'
```

The sweep can miss: where the `avc_node` lands moves between boots, and a run
that stores into the wrong place reboots the tablet. Measured hit rate was one
in about ten to one in about twenty attempts, so a loop is normal. A run that
reports `module_errno=8` before the sweep has already passed the capability
check and goes straight to `init_module`.

After that the app does the rest:

```sh
adb shell am start -n com.charepa/.MainActivity --es mode all
```

`validate` only touches memory the process owns and prints PASS or FAIL, which
is the safe way to check the primitive on a given unit.

## Layout

```
jni/charepa.c            the chain, ending with the hook install
jni/bootpayload.c        what runs at boot from init's domain
prebuilt/myroot.ko       the module, built offline against the vendor tree
app/                     the Activity that stages and runs the binaries
```

## Licence

MIT, see `LICENSE`.
