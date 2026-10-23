/* SPDX-License-Identifier: MIT
 * Setup's side of the headless hardware report: framing round trip, bad,
 * truncated and oversized frames, timeout with kill and reap, missing binary
 * and spawn refusal, with fake channel/process operations and virtual time. */
#include "../../../apps/setup/report_link.h"
#include "../../../apps/hwreport/report.h"
#include "../../../apps/hwreport/textframe.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 2048
struct fake {
    uint8_t frames[MAX_FRAMES][HR_FRAME_MAX]; size_t sizes[MAX_FRAMES]; unsigned count, next;
    int silent;              /* after the script: never ready (else: peer closed) */
    int pair_error, spawn_result, hang, child_status, ignore_kill_exit;
    uint64_t now; unsigned waits, longest_wait;
    unsigned spawns, kills, reaps, closed, sleeps; int open_fds;
    char path[64], args[128]; int child_fd, child_end;
    int killed_state;
};
static struct fake F;
static int pair(void *c,int fds[2]){(void)c;if(F.pair_error)return F.pair_error;fds[0]=40;fds[1]=41;F.open_fds=2;return 0;}
static int spawn(void *c,const char *path,char *const argv[],int child_end,int child_fd){
    (void)c;F.spawns++;snprintf(F.path,sizeof(F.path),"%s",path);F.args[0]=0;F.child_fd=child_fd;F.child_end=child_end;
    for(unsigned i=0;argv[i];i++){size_t k=strlen(F.args);snprintf(F.args+k,sizeof(F.args)-k,"%s%s",i?" ":"",argv[i]);}
    return F.spawn_result?F.spawn_result:77;
}
static int wait_readable(void *c,int fd,unsigned ms){
    (void)c;assert(fd==40);F.waits++;if(ms>F.longest_wait)F.longest_wait=ms;assert(ms>0&&ms<=ST_LINK_SLICE_MS);
    if(F.next<F.count)return 1;
    if(F.silent){F.now+=ms;return 0;}
    return 1;
}
static int recv_frame(void *c,int fd,uint8_t data[256],size_t *bytes){
    (void)c;assert(fd==40);
    if(F.next>=F.count)return 0;
    *bytes=F.sizes[F.next];memcpy(data,F.frames[F.next],*bytes);F.next++;return 1;
}
static int reap(void *c,int pid,int *status){
    (void)c;assert(pid==77);
    if(F.killed_state==2)return -ECHILD;
    if(F.killed_state==1&&!F.ignore_kill_exit){*status=9;F.reaps++;F.killed_state=2;return 1;}
    if(F.hang&&F.killed_state==0)return 0;
    if(F.killed_state==1){F.ignore_kill_exit--;return 0;}
    *status=F.child_status<<8;F.reaps++;F.killed_state=2;return 1;
}
static int kill_child(void *c,int pid){(void)c;assert(pid==77);F.kills++;F.killed_state=1;return 0;}
static void sleep_ms(void *c,unsigned ms){(void)c;F.sleeps++;F.now+=ms;}
static void close_fd(void *c,int fd){(void)c;assert(fd==40||fd==41);F.closed++;F.open_fds--;}
static uint64_t now_ms(void *c){(void)c;return F.now;}
static const struct st_link_io io={NULL,pair,spawn,wait_readable,recv_frame,reap,kill_child,sleep_ms,close_fd,now_ms};
/* Frames are produced by the real sender, hr_link_send. */
static int capture(void *c,const void *data,size_t bytes){(void)c;assert(F.count<MAX_FRAMES&&bytes<=HR_FRAME_MAX);memcpy(F.frames[F.count],data,bytes);F.sizes[F.count++]=bytes;return 0;}
static const struct hr_link_io sender={NULL,capture};
static void reset(void){memset(&F,0,sizeof(F));}
static char *pattern(size_t n){char *t=malloc(n?n:1);assert(t);for(size_t i=0;i<n;i++)t[i]=(i%61)==60?'\n':(char)('a'+(i*7)%26);return t;}
static char out[HR_LINK_MAX];
static void run(int expect,const char *reason,size_t length){
    size_t got=99;char why[ST_LINK_REASON];
    int r=st_link_report(&io,out,sizeof(out),&got,why);
    if(r!=expect||strcmp(why,reason))fprintf(stderr,"expected %d/%s got %d/%s\n",expect,reason,r,why);
    assert(r==expect&&!strcmp(why,reason));
    if(!expect)assert(got==length);else assert(!got);
    /* Every path closes both channel ends, and a started child is reaped. */
    assert(F.open_fds==0&&F.closed==2);
    if(F.spawns&&F.spawn_result>=0)assert(F.reaps==1);
    assert(!strcmp(F.path,F.spawns?ST_LINK_PROGRAM:"")||!F.spawns);
}
static void frames(const char *text,size_t n,int status){assert(!hr_link_send(&sender,text,n,status));}
void link_tests(void)
{
    /* Round trip at the chunk edges and a large report. */
    static const size_t sizes[]={1,HR_FRAME_CHUNK-1,HR_FRAME_CHUNK,HR_FRAME_CHUNK+1,5000,HR_LINK_MAX};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++){
        reset();char *t=pattern(sizes[i]);frames(t,sizes[i],0);
        run(0,"",sizes[i]);assert(!memcmp(out,t,sizes[i]));
        assert(F.spawns==1&&!strcmp(F.path,"/Programs/hwreport")&&!strcmp(F.args,"hwreport --rescue --text-fd=3")&&F.child_fd==3&&F.child_end==41);
        assert(F.kills==0&&F.longest_wait<=ST_LINK_SLICE_MS);free(t);
    }
    /* An empty text with status 0 is a valid transfer (Setup rejects it as "empty"). */
    reset();frames("",0,0);run(0,"",0);
    /* A report over the bound is cut by the sender and arrives as a status. */
    reset();{char *t=pattern(5000);frames(t,4000,-EFBIG);free(t);}run(-EIO,"status",0);assert(F.reaps==1&&!F.kills);
    /* Missing binary and spawn refusal: nothing to reap, both ends closed. */
    reset();F.spawn_result=-ENOENT;run(-ENOENT,"missing",0);assert(!F.reaps&&!F.kills&&!F.waits);
    reset();F.spawn_result=-EPERM;run(-EPERM,"spawn",0);
    reset();F.spawn_result=-EMFILE;run(-EMFILE,"spawn",0);
    reset();F.pair_error=-EMFILE;{size_t g;char why[ST_LINK_REASON];assert(st_link_report(&io,out,sizeof(out),&g,why)==-EIO&&!strcmp(why,"pair")&&!F.spawns&&!F.closed);}
    /* Child exit status: a failing exit after a clean transfer is a failure. */
    reset();frames("abc\n",4,0);F.child_status=1;run(-ECHILD,"exit",0);
    /* Bad frames: each kills and reaps the running child. */
    char *t=pattern(1000);
    reset();frames(t,1000,0);F.frames[1][0]^=0xff;F.hang=1;run(-EPROTO,"frame",0);assert(F.kills==1);                   /* bad magic */
    reset();frames(t,1000,0);F.frames[0][4]=2;F.hang=1;run(-EPROTO,"frame",0);assert(F.kills==1);                       /* bad version */
    reset();frames(t,1000,0);F.frames[1][6]=9;F.hang=1;run(-EPROTO,"frame",0);assert(F.kills==1);                       /* bad type */
    reset();frames(t,1000,0);F.next=1;F.hang=1;run(-EPROTO,"frame",0);assert(F.kills==1);                               /* DATA before BEGIN */
    reset();frames(t,1000,0);memmove(F.frames[1],F.frames[2],HR_FRAME_MAX);F.sizes[1]=F.sizes[2];F.count--;F.hang=1;run(-EPROTO,"frame",0);assert(F.kills==1); /* offset gap */
    reset();frames(t,1000,0);F.sizes[1]=5;F.hang=1;run(-EPROTO,"frame",0);assert(F.kills==1);                           /* short frame */
    reset();frames(t,1000,0);F.frames[F.count-1][16]^=1;F.hang=1;run(-EBADMSG,"checksum",0);assert(F.kills==1);        /* bad CRC */
    reset();frames(t,1000,0);hr_put32(F.frames[F.count-1]+12,999);F.hang=1;run(-EPROTO,"frame",0);assert(F.kills==1);  /* END total differs */
    reset();frames(t,1000,0);hr_put32(F.frames[0]+8,HR_LINK_MAX+1);F.hang=1;run(-EMSGSIZE,"size",0);assert(F.kills==1);/* oversized declaration */
    reset();frames(t,1000,0);hr_put32(F.frames[0]+8,10);F.hang=1;run(-EMSGSIZE,"size",0);assert(F.kills==1);           /* data beyond the declared total */
    reset();frames(t,1000,0);F.count--;F.hang=1;run(-EPIPE,"closed",0);assert(F.kills==1);                              /* truncated: closed before END */
    reset();frames(t,1000,0);F.count=1;F.hang=1;run(-EPIPE,"closed",0);assert(F.kills==1);                              /* only BEGIN */
    reset();frames(t,1000,0);{uint8_t extra[HR_FRAME_MAX];size_t n=hr_frame_begin(extra,5);memcpy(F.frames[F.count],extra,n);F.sizes[F.count++]=n;}
    {size_t g;char why[ST_LINK_REASON];assert(!st_link_report(&io,out,sizeof(out),&g,why)&&g==1000&&F.next==F.count-1&&F.kills==0);}           /* data after END is never read */
    /* Timeout: the sender goes silent after BEGIN. Waits stay within one slice,
     * the total within the deadline, the child is killed and reaped. */
    reset();frames(t,1000,0);F.count=1;F.silent=1;F.hang=1;run(-ETIMEDOUT,"timeout",0);
    assert(F.kills==1&&F.now>=ST_LINK_DEADLINE_MS&&F.now<=ST_LINK_DEADLINE_MS+ST_LINK_KILL_MS&&F.longest_wait<=ST_LINK_SLICE_MS);
    assert(F.waits>=ST_LINK_DEADLINE_MS/ST_LINK_SLICE_MS&&F.waits<=ST_LINK_DEADLINE_MS/ST_LINK_SLICE_MS+2);
    /* Complete transfer but a child that never exits: bounded wait, kill, reap. */
    reset();frames("abc\n",4,0);F.hang=1;run(-ETIMEDOUT,"timeout",0);assert(F.kills==1);
    /* A child that survives the first kill checks is still reaped after SIGKILL lands. */
    reset();frames(t,1000,0);F.frames[1][0]^=0xff;F.hang=1;F.ignore_kill_exit=5;run(-EPROTO,"frame",0);assert(F.kills==1&&F.sleeps>=5);
    free(t);
    puts("PASS report link: framing round trip, bad/truncated/oversized frames, status and exit failures, timeout with kill and reap, missing binary, spawn refusal");
}
