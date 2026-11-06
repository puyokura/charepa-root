/* k37: the -EFAULT is raised at syscall ENTRY, not inside the pipe code.
 *
 * Read from fs/read_write.c rw_copy_check_uvector, which every readv/writev
 * goes through before any file operation:
 *
 *     for (seg = 0; seg < nr_segs; seg++) {
 *         void __user *buf = iov[seg].iov_base;
 *         ssize_t len = (ssize_t)iov[seg].iov_len;
 *         if (len < 0) { ret = -EINVAL; goto out; }
 *         if (type >= 0 && unlikely(!access_ok(vrfy_dir(type), buf, len))) {
 *             ret = -EFAULT;                 <- HERE, before the file is touched
 *             goto out;
 *         }
 *         ...
 *     }
 *
 * So a kernel address in iov_base is rejected while the iovec array is still
 * being copied in, long before pipe_read or pipe_write runs. That single fact
 * explains every -EFAULT measured so far:
 *
 *   k27 readv  : iov[4].base = core_pattern   -> rejected at entry
 *   k29 writev : iov[4].base = _text          -> rejected at entry
 *   k35 writev : no UAF at all, same address  -> rejected at entry
 *   k36 writev : merge path, same address     -> rejected at entry
 *
 * and it corrects the earlier attribution to pipe_read's fault-in / access_ok,
 * which never got a chance to run.
 *
 * This tool separates entry-level rejection from file-level behaviour by using
 * three different file types. If the error is identical for /dev/null, /dev/zero
 * and a pipe, the pipe code is not involved.
 *
 * It also pins down the range semantics: access_ok checks base+len against
 * addr_limit, so a user base with a length that reaches into kernel space must
 * fail as well, while the same base with a small length must succeed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/mman.h>

typedef uint64_t u64; typedef uint8_t u8;

static FILE *LOG;
static void say(const char *fmt, ...){
    char b[300]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, LOG); fflush(LOG);
}

static ssize_t try_writev(int fd, u64 addr, size_t len){
    struct iovec iov; iov.iov_base = (void*)(uintptr_t)addr; iov.iov_len = len;
    errno = 0;
    ssize_t r = writev(fd, &iov, 1);
    return r < 0 ? -errno : r;
}
static ssize_t try_readv(int fd, u64 addr, size_t len){
    struct iovec iov; iov.iov_base = (void*)(uintptr_t)addr; iov.iov_len = len;
    errno = 0;
    ssize_t r = readv(fd, &iov, 1);
    return r < 0 ? -errno : r;
}

int main(void){
    LOG = fopen("/data/local/tmp/k37.txt","w"); if (!LOG) LOG = stdout;
    setbuf(LOG, NULL);
    say("k37 pid=%d uid=%d\n", getpid(), getuid());

    u8 *pg = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    memset(pg, 0x33, 4096);

    int dn = open("/dev/null", O_WRONLY);
    int dz = open("/dev/zero", O_RDONLY);
    int pp[2]; pipe(pp);
    say("/dev/null=%d /dev/zero=%d pipe=%d,%d page=%p\n", dn, dz, pp[0], pp[1], (void*)pg);

    struct { const char *what; int fd; u64 addr; size_t len; int rd; } T[] = {
      {"writev /dev/null  user  page", dn, (u64)(uintptr_t)pg, 16, 0},
      {"writev /dev/null  _text     ", dn, 0xffffffc000080000ULL, 16, 0},
      {"writev /dev/null  .data     ", dn, 0xffffffc000bee000ULL, 16, 0},
      {"writev pipe       user  page", pp[1], (u64)(uintptr_t)pg, 16, 0},
      {"writev pipe       _text     ", pp[1], 0xffffffc000080000ULL, 16, 0},
      {"writev pipe       .data     ", pp[1], 0xffffffc000bee000ULL, 16, 0},
      {"readv  /dev/zero  user  page", dz, (u64)(uintptr_t)pg, 16, 1},
      {"readv  /dev/zero  _text     ", dz, 0xffffffc000080000ULL, 16, 1},
      {"readv  /dev/zero  .data     ", dz, 0xffffffc000bee000ULL, 16, 1},
      /* range semantics: a valid user base with a length reaching kernel space */
      {"writev pipe       usr+4G    ", pp[1], (u64)(uintptr_t)pg, (size_t)0x100000000ULL, 0},
      {"writev pipe       usr+huge  ", pp[1], (u64)(uintptr_t)pg, (size_t)-1, 0},
      {"writev pipe       usr+4095  ", pp[1], (u64)(uintptr_t)pg, 4095, 0},
    };
    int n = (int)(sizeof T / sizeof T[0]);
    for (int i = 0; i < n; i++){
        ssize_t r = T[i].rd ? try_readv(T[i].fd, T[i].addr, T[i].len)
                            : try_writev(T[i].fd, T[i].addr, T[i].len);
        say("%s len=0x%llx -> %zd\n", T[i].what,
            (unsigned long long)T[i].len, r);
    }

    close(pp[0]); close(pp[1]);
    if (dn >= 0) close(dn);
    if (dz >= 0) close(dz);
    say("K37 done\n");
    return 0;
}
