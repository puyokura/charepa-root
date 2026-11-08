# charepa-apk

An app that tries to take root on the Benesse **TAB-A03-BR2** and **TAB-A03-BS**
tablets (Android 5.1, kernel 3.10.33, Marvell PXA1928) without UART, for a
device you own.

Status: the ION write primitive and the credential grant are measured working on
a real BS tablet from an ordinary app process. The SELinux step and the module
load are not; the tablet reboots during the AVC search. Do not expect this to
root anything yet.

## What has been measured

All of this was run through `run-as com.termux` on a TAB-A03-BS (build 01.14.000),
so the process is an ordinary `u:r:untrusted_app:s0` app, uid 10145.

| step | result |
| --- | --- |
| `/dev/ion` open, allocate, share, `ION_IOC_PHYS` | works |
| `ION_IOC_CUSTOM` with a kernel pointer for `arg` | works (`iova=0x80010000`) |
| gate rejects a zero word, accepts 192, stores at `+4` | `validate` says PASS |
| walking the linear map for `cap_bset = 192` | 300-600 hits per 2 GB |
| granting `CAP_SYS_MODULE` into `cap_effective` | `capget` reads back `0x80010000` |
| `init_module` after the grant | still `EPERM` |
| `/dev/binder`, mmap, `epoll`, `BINDER_THREAD_EXIT` | works (unused so far) |
| `setrlimit(RLIMIT_NOFILE)` to the hard limit | works: soft 4096 |

The last two lines of the cred scan are why the BR2 recipe had to change: an app
cannot hold fd 10145, so this uses `cap_bset.cap[0] = 192` (192 fits under 4096)
as the value the gate looks for.

## Why it stops at SELinux

`init_module` runs `capable(CAP_SYS_MODULE)`, and SELinux answers that with
`selinux_capable()`: `cap_capable()` reads `cap_effective` (which now has bit 16)
and then `cred_has_capability()` asks the policy, which denies the
`capability` class for `untrusted_app`. The denial is cached in an `avc_node`
whose `tclass` is 4, so the same gate can find it and set bit 16 in
`av_decision.allowed`.

In practice the search for that node writes thousands of times and the tablet
panics first. Narrow windows still lose it.

## Layout

```
jni/charepa.c            the whole chain in one static binary
app/                     Activity that runs it and shows the log
.github/workflows/       builds the APK, publishes it as an artifact
```

## Build

The binary is static aarch64 and is built outside Gradle, because the tablet
needs a plain executable:

```sh
aarch64-linux-gnu-gcc -static -O2 -o charepa jni/charepa.c
```

Running it on a device you own, from an app context:

```sh
adb push charepa /data/local/tmp/
adb shell run-as com.termux cp /data/local/tmp/charepa /data/data/com.termux/files/
adb shell run-as com.termux /data/data/com.termux/files/charepa validate
```

`validate` only touches memory the process owns and prints PASS or FAIL. The
scan subcommands write into kernel memory on every match and can reboot the
tablet.

## Licence

MIT, see `LICENSE`.
