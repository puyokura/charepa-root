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
static void st1(const char *p){
    struct stat s;
    if(lstat(p,&s)!=0){ fprintf(o,"  %-44s %s\n",p,strerror(errno)); return; }
    fprintf(o,"  %-44s mode=%07o size=%lld",p,s.st_mode,(long long)s.st_size);
    if(S_ISLNK(s.st_mode)){char t[256]={0}; if(readlink(p,t,255)>0) fprintf(o," -> %s",t);}
    fprintf(o,"\n");
}
int main(void){
    o=fopen("/data/local/tmp/lsx.txt","w"); if(!o)o=stdout;
    int r=be_root(); fprintf(o,"root=%d uid=%d\n",r,getuid());
    const char *p[]={
        "/system/bin/calibinit.sh","/system/bin/log_on_boot.sh",
        "/system/bin/dumpall.sh","/system/bin/tcpdump_on_boot.sh",
        "/system/bin/sh","/system/bin/toolbox","/system/bin/kinitlog.sh",
        "/system/lib/modules/myroot.ko","/system/lib/modules/rootload", NULL};
    for(int i=0;p[i];i++) st1(p[i]);
    fclose(o); return 0;
}
