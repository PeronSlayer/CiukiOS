/* Native transport unit fixture only; never claimed as NSIS/PE evidence.
 * Actually executes in a separate process, writes through the H1 protocol,
 * then exits 23 so wait/query checks cannot synthesize a successful exit.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int io(int fd,void *p,size_t n,int writing){
    char *b=p;while(n){ssize_t r=writing?write(fd,b,n):read(fd,b,n);if(r<=0)return 0;b+=r;n-=(size_t)r;}return 1;
}
static int exchange(int fd,uint32_t *record,unsigned bytes){
    uint32_t header[]={0x32494643,1,bytes,bytes},reply[2];
    return io(fd,header,sizeof(header),1)&&io(fd,record,bytes,1)&&io(fd,reply,sizeof(reply),0)&&!reply[0]&&reply[1]==bytes&&io(fd,record,bytes,0);
}
int main(int argc,char **argv){
    if(argc!=5)return 70;int fd=atoi(argv[4]);
    /* Stay running until the parent has observed STILL_ACTIVE and a
     * zero-timeout wait. A timed sleep would be flaky on this shared host. */
    unsigned released=0;
    for(unsigned i=0;i<10000;i++){
        uint32_t gate[128]={1,72,0x109,0,1,0,72,0,0,0,0x80000000,7,3};
        strcpy((char *)gate+72,"C:\\WINDOWS\\TEMP\\transport-child.release");
        if(!exchange(fd,gate,sizeof(gate)))return 74;
        if(!gate[17]){
            uint32_t close[18]={1,72,0x100,0,3,0,gate[16]};
            if(!exchange(fd,close,sizeof(close)) || close[15])return 75;
            released=1;break;
        }
        if(gate[17]!=2)return 76;
        struct timespec delay={0,1000000};nanosleep(&delay,0);
    }
    if(!released)return 77;
    uint32_t packet[128]={1,72,0x109,0,1,0,72,0,0,0,0x40000000,7,2};
    strcpy((char *)packet+72,"C:\\WINDOWS\\TEMP\\transport-child.done");
    if(!exchange(fd,packet,sizeof(packet)) || packet[17])return 71;
    uint32_t handle=packet[16];
    memset(packet,0,sizeof(packet));packet[0]=1;packet[1]=72;packet[2]=0x109;packet[4]=3;packet[5]=handle;packet[8]=72;packet[9]=6;
    memcpy((char *)packet+72,"child\n",6);
    if(!exchange(fd,packet,sizeof(packet)) || packet[17] || packet[16]!=6)return 72;
    memset(packet,0,sizeof(packet));packet[0]=1;packet[1]=72;packet[2]=0x100;packet[4]=3;packet[6]=handle;
    if(!exchange(fd,packet,sizeof(packet)) || packet[15])return 73;
    return 23;
}
