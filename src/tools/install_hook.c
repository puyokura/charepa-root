/* Install the boot hook with a static loader, because insmod is a symlink to the
 * broken toolbox binary. Two files change on /system:
 *   /system/lib/modules/myroot.ko     the module
 *   /system/bin/kinitlog.sh           the script init runs at boot_completed
 * and the directory also gets /system/lib/modules/rootload, the static loader. */
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
static int cp(const char *s,const char *d,mode_t m){
    int in=open(s,O_RDONLY);
    if(in<0){fprintf(o,"  %s: %s\n",s,strerror(errno));return -1;}
    int out=open(d,O_CREAT|O_TRUNC|O_WRONLY,m);
    if(out<0){fprintf(o,"  %s: %s\n",d,strerror(errno));close(in);return -1;}
    char b[65536]; ssize_t n; long t=0;
    while((n=read(in,b,sizeof b))>0){ ssize_t off=0;
        while(off<n){ ssize_t w=write(out,b+off,n-off); if(w<=0)break; off+=w;} t+=n; }
    close(in); close(out); chmod(d,m);
    fprintf(o,"  %s -> %s (%ld bytes mode %o)\n",s,d,t,m&07777);
    return 0;
}
int main(void){
    o=fopen("/data/local/tmp/install_hook.txt","w"); if(!o)o=stdout;
    if(be_root()!=0){fprintf(o,"no root\n");fclose(o);return 1;}
    fprintf(o,"uid=%d\n",getuid());
    int b=open("/dev/block/platform/soc.1/by-name/system",O_RDWR);
    if(b<0){fprintf(o,"open: %s\n",strerror(errno));fclose(o);return 1;}
    ioctl(b,BLKROSET,&(int){0});
    if(mount(NULL,"/system",NULL,MS_REMOUNT,NULL)!=0){
        fprintf(o,"remount: %s\n",strerror(errno));close(b);fclose(o);return 1;}
    fprintf(o,"/system rw\n");

    cp("/data/local/tmp/myroot.ko","/system/lib/modules/myroot.ko",0644);
    cp("/data/local/tmp/modload","/system/lib/modules/rootload",0750);

    /* the hook calls the static loader, not insmod */
    const char *sh =
        "#!/system/bin/sh\n"
        "/system/lib/modules/rootload /system/lib/modules/myroot.ko\n"
        "exit 0\n";
    { int f=open("/system/bin/kinitlog.sh",O_CREAT|O_TRUNC|O_WRONLY,0755);
      if(f<0) fprintf(o,"  kinitlog.sh: %s\n",strerror(errno));
      else { write(f,sh,strlen(sh)); close(f); chmod("/system/bin/kinitlog.sh",0755);
             fprintf(o,"  wrote /system/bin/kinitlog.sh (%zu bytes)\n",strlen(sh)); } }

    struct stat st;
    const char *c[]={"/system/lib/modules/myroot.ko","/system/lib/modules/rootload",
                     "/system/bin/kinitlog.sh",NULL};
    for(int i=0;c[i];i++){
        if(stat(c[i],&st)==0) fprintf(o,"  %-40s %lld bytes mode=%o\n",
                                      c[i],(long long)st.st_size,st.st_mode&07777);
        else fprintf(o,"  %-40s %s\n",c[i],strerror(errno));
    }
    sync();
    ioctl(b,BLKROSET,&(int){1});
    mount(NULL,"/system",NULL,MS_REMOUNT|MS_RDONLY,NULL);
    fprintf(o,"/system ro\n");
    close(b); fclose(o);
    return 0;
}
