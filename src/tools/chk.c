#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
static FILE *o;
int main(void){
    o=fopen("/data/local/tmp/chk.txt","w"); if(!o)o=stdout;
    fprintf(o,"uid=%d\n",getuid());
    const char *p[]={"/system/lib/modules/myroot.ko","/system/etc/install-recovery.sh",NULL};
    for(int i=0;p[i];i++){
        struct stat st;
        if(stat(p[i],&st)==0) fprintf(o,"  %-46s %lld bytes\n",p[i],(long long)st.st_size);
        else fprintf(o,"  %-46s %s\n",p[i],strerror(errno));
    }
    /* can we read it back byte-identical? */
    { int f=open("/system/lib/modules/myroot.ko",O_RDONLY);
      if(f>=0){ char b[16]; ssize_t n=read(f,b,16);
        fprintf(o,"  read back %zd bytes: ",n);
        for(int i=0;i<n;i++) fprintf(o,"%02x",(unsigned char)b[i]);
        fprintf(o,"\n"); close(f);} }
    fclose(o); return 0;
}
