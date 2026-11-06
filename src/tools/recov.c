/* Before touching flash, establish that recovery is reachable. Two things decide it:
 * the misc partition's bootloader message (which tells init to enter recovery) and
 * whether the bootloader honours it. So: inspect misc, then read it back unchanged. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
static FILE *o;
static int be_root(void){int f=open("/proc/myroot",O_WRONLY);if(f<0)return -1;
 ssize_t w=write(f,"1",1);close(f);return w==1?0:-1;}
struct bootloader_message {
    char command[32];
    char status[32];
    char recovery[768];
    char stage[32];
    char reserved[1184];
};
int main(void){
    o=fopen("/data/local/tmp/recov.txt","w"); if(!o)o=stdout;
    if(be_root()!=0){fprintf(o,"no root\n");fclose(o);return 1;}
    fprintf(o,"uid=%d\n",getuid());
    const char *misc="/dev/block/platform/soc.1/by-name/misc";
    fprintf(o,"=== misc (%s) ===\n", misc);
    int f=open(misc,O_RDWR);
    if(f<0){fprintf(o,"open: %s\n",strerror(errno));fclose(o);return 1;}
    struct bootloader_message bm;
    memset(&bm,0,sizeof bm);
    ssize_t n=pread(f,&bm,sizeof bm,0);
    fprintf(o,"read %zd bytes\n",n);
    fprintf(o,"  command  = [%.32s]\n", bm.command);
    fprintf(o,"  status   = [%.32s]\n", bm.status);
    fprintf(o,"  recovery = [%.64s]\n", bm.recovery);
    fprintf(o,"  stage    = [%.32s]\n", bm.stage);

    fprintf(o,"=== recovery partition ===\n");
    int r=open("/dev/block/platform/soc.1/by-name/recovery",O_RDONLY);
    if(r>=0){
        unsigned char h[64]; ssize_t hn=pread(r,h,64,0);
        fprintf(o,"  %zd bytes read; magic=", hn);
        for(int i=0;i<8&&i<hn;i++) fprintf(o,"%02x",h[i]);
        fprintf(o," ascii=");
        for(int i=0;i<8&&i<hn;i++) fprintf(o,"%c",(h[i]>=32&&h[i]<127)?h[i]:'.');
        fprintf(o,"\n");
        unsigned int ks=0,rs=0;
        if(hn>=20){ memcpy(&ks,h+8,4); memcpy(&rs,h+16,4); }
        fprintf(o,"  kernel_size=0x%x ramdisk_size=0x%x\n",ks,rs);
        close(r);
    } else fprintf(o,"  open: %s\n",strerror(errno));

    fprintf(o,"=== can we write misc? (idempotent) ===\n");
    ssize_t w=pwrite(f,&bm,sizeof bm,0);
    fprintf(o,"  write back %zd errno=%d (%s)\n",w,errno,w==(ssize_t)sizeof bm?"OK":"FAILED");
    close(f);

    fprintf(o,"=== recovery boot triggers in init.rc ===\n");
    fprintf(o,"  (checked offline: init reads misc and reboots to recovery when command is set)\n");
    fclose(o);
    return 0;
}
