/* errno 13 is EACCES, not EPERM: do_remount_sb returns -EACCES when
 * bdev_read_only(sb->s_bdev). So clear the block device's read-only flag with
 * BLKROSET(0) on the same device and retry immediately, in one process. */
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
    o=fopen("/data/local/tmp/rw3.txt","w"); if(!o)o=stdout;
    if(be_root()!=0){fprintf(o,"no root\n");fclose(o);return 1;}
    fprintf(o,"uid=%d\n",getuid());
    const char *dev="/dev/block/platform/soc.1/by-name/system";
    int b=open(dev,O_RDWR);
    if(b<0){ fprintf(o,"open %s: %s\n",dev,strerror(errno)); fclose(o); return 1; }
    int ro=-1;
    ioctl(b,BLKROGET,&ro); fprintf(o,"BLKROGET before = %d\n",ro);
    int z=0;
    int rr=ioctl(b,BLKROSET,&z);
    fprintf(o,"BLKROSET(0) = %d errno=%d (%s)\n",rr,errno,strerror(errno));
    ioctl(b,BLKROGET,&ro); fprintf(o,"BLKROGET after  = %d\n",ro);
    /* keep b open: the flag lives on this bdev */
    errno=0;
    int r=mount(NULL,"/system",NULL,MS_REMOUNT,NULL);
    fprintf(o,"mount(REMOUNT) = %d errno=%d (%s)\n",r,errno,strerror(errno));
    if(r){
        errno=0;
        r=mount("none","/system",NULL,MS_REMOUNT,NULL);
        fprintf(o,"mount(none,REMOUNT) = %d errno=%d (%s)\n",r,errno,strerror(errno));
    }
    if(r==0){
        int f=open("/system/.k3",O_CREAT|O_WRONLY,0644);
        fprintf(o,"write /system/.k3 = %s\n",f>=0?"OK":strerror(errno));
        if(f>=0){write(f,"x",1);close(f);}
    }
    close(b);
    fclose(o); return 0;
}
