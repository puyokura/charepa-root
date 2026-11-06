/* insmod is a toolbox symlink and toolbox is broken on this build, so the boot hook
 * cannot rely on it. This is a static loader: it calls init_module directly. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/stat.h>
int main(int argc, char **argv){
    const char *path = argc > 1 ? argv[1] : "/system/lib/modules/myroot.ko";
    FILE *o = fopen("/data/local/tmp/modload.txt","w");
    if (!o) o = stdout;
    fprintf(o,"uid=%d euid=%d\n", getuid(), geteuid());
    fprintf(o,"loading %s\n", path);
    int f = open(path, O_RDONLY);
    if (f < 0){ fprintf(o,"open: %s\n", strerror(errno)); fclose(o); return 1; }
    struct stat st; fstat(f, &st);
    char *buf = malloc(st.st_size);
    ssize_t n = read(f, buf, st.st_size);
    close(f);
    fprintf(o,"read %zd/%lld bytes\n", n, (long long)st.st_size);
    errno = 0;
    long rc = syscall(105, buf, (unsigned long)n, "");
    fprintf(o,"init_module rc=%ld errno=%d (%s)\n", rc, errno, strerror(errno));
    if (rc == 0){
        int p = open("/proc/myroot", O_WRONLY);
        if (p >= 0){ if (write(p,"1",1)==1) fprintf(o,"/proc/myroot ok, uid=%d\n", getuid()); close(p); }
        else fprintf(o,"/proc/myroot: %s\n", strerror(errno));
    }
    fclose(o);
    return rc == 0 ? 0 : 1;
}
