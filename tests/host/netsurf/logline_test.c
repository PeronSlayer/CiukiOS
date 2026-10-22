/* f6-22: NetSurf's stderr record lines leave as one write(2) each.
 * On the T23 (image 5a10eef4) "[netsurf] layout slow elapsed_us=" and
 * "[netsurf] exit reason=" reached the log with their values on separate
 * "[console]" records: Ciuki's newlib writes an unbuffered, read/write
 * stderr one conversion at a time and the kernel frames every write. glibc
 * buffers a whole fprintf, so this test models the target with an 8-byte
 * stderr buffer holding earlier text: fprintf then splits the line at the
 * buffer boundary; the port helper must not.
 * fd 2 is a SOCK_SEQPACKET socket, so every write(2) stays one record.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include "sdk/netsurf/port/budget.h"
#include "sdk/netsurf/port/logline.h"
/* stderr is the socket under test: failures are reported on stdout. */
#define CHECK(x) do { if(!(x)) { printf("FAIL logline line %d: %s\n",__LINE__,#x);fflush(stdout);_exit(1); } } while(0)
static uint64_t clock_ns=1000000000u;
int __wrap_clock_gettime(clockid_t clock,struct timespec *out)
{ assert(clock==CLOCK_MONOTONIC);out->tv_sec=clock_ns/1000000000u;out->tv_nsec=clock_ns%1000000000u;return 0; }
static char writes[16][1100];
static size_t lengths[16];
static unsigned count,interrupt,partial;
static int peer=-1;
ssize_t __real_write(int,const void *,size_t);
/* Fault injection for the helper's own write calls (EINTR, short write). */
ssize_t __wrap_write(int fd,const void *data,size_t bytes)
{
    if(fd==2 && interrupt){interrupt--;errno=EINTR;return -1;}
    if(fd==2 && partial && bytes>partial){bytes=partial;partial=0;}
    return __real_write(fd,data,bytes);
}
/* Every record written to fd 2 since the last call. */
static void collect(void)
{
    count=0;
    for(;;){
        char buffer[1100];ssize_t n=recv(peer,buffer,sizeof(buffer)-1,0);
        if(n<0){CHECK(errno==EAGAIN || errno==EWOULDBLOCK);return;}
        CHECK(count<16);memcpy(writes[count],buffer,(size_t)n);writes[count][n]=0;lengths[count++]=(size_t)n;
    }
}
int main(void)
{
    int sv[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,sv));
    int saved=dup(2);CHECK(saved>=0 && dup2(sv[0],2)==2);peer=sv[1];
    CHECK(!fcntl(peer,F_SETFL,O_NONBLOCK));
    static char tiny[8];
    CHECK(!setvbuf(stderr,tiny,_IOFBF,sizeof(tiny)));
    /* The model splits a plain fprintf behind pending text, as the target
     * split every conversion: no record holds the whole line. */
    fputs("abc",stderr);fprintf(stderr,"[netsurf] split elapsed_us=%u\n",3300000u);fflush(stderr);
    collect();CHECK(count>2);
    for(unsigned i=0;i<count;i++)CHECK(!strstr(writes[i],"[netsurf] split elapsed_us=3300000\n"));
    /* layout slow: a 3.3 s local layout, as measured on the T23. */
    struct ciuki_layout_span span=ciuki_layout_begin(false);clock_ns+=3300000000u;
    fputs("abc",stderr);CHECK(ciuki_layout_end(span,true));
    collect();CHECK(count==2 && !strcmp(writes[0],"abc"));
    CHECK(!strcmp(writes[1],"[netsurf] layout slow elapsed_us=3300000 class=local accepted=1\n"));
    /* Earlier buffered stderr text goes first, never inside the line. */
    fputs("partial",stderr);
    CHECK(ciuki_log_line("[netsurf] exit reason=%s detail=%d","desktop-close-request",0)==53);
    collect();CHECK(count>=2 && !strcmp(writes[count-1],"[netsurf] exit reason=desktop-close-request detail=0\n"));
    /* A newline is never doubled; an overlong line is cut and terminated. */
    CHECK(ciuki_log_line("one\n")==4);collect();CHECK(count==1 && !strcmp(writes[0],"one\n"));
    char big[700];memset(big,'x',sizeof(big)-1);big[sizeof(big)-1]=0;
    CHECK(ciuki_log_line("%s",big)==CIUKI_LOG_LINE_MAX-1);
    collect();CHECK(count==1 && lengths[0]==CIUKI_LOG_LINE_MAX-1 && writes[0][CIUKI_LOG_LINE_MAX-2]=='\n');
    /* EINTR retries; a short console write is completed, not dropped. */
    interrupt=1;partial=10;
    CHECK(ciuki_log_line("[netsurf] layout slow elapsed_us=%u",1u)==35);
    collect();CHECK(count==2 && lengths[0]==10 && lengths[0]+lengths[1]==35);
    CHECK(!strncmp(writes[0],"[netsurf] ",10) && !strcmp(writes[1],"layout slow elapsed_us=1\n"));
    fflush(stderr);setvbuf(stderr,NULL,_IONBF,0);CHECK(dup2(saved,2)==2);close(saved);close(sv[0]);close(sv[1]);
    printf("NetSurf stderr records: layout slow and exit lines are single writes with their values (8-byte stderr model)\n");
    return 0;
}
