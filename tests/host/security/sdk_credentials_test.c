/* Only the syscall boundary is substituted; exercise the real SDK adapters.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define getuid test_getuid
#define geteuid test_geteuid
#define getgid test_getgid
#define getegid test_getegid
#define getgroups test_getgroups
#define setuid test_setuid
#define seteuid test_seteuid
#define setgid test_setgid
#define setegid test_setegid
#define setreuid test_setreuid
#define setregid test_setregid
#include "../../../sdk/libciuki/proc.c"
static void *pointers[256];
static unsigned pointer_count, calls;
static int syscall_failure;
static struct ciuki_credential caller;
static struct sdk_reent reent;
struct sdk_reent *__getreent(void) { return &reent; }
uint32_t sdk_pointer(void *p)
{
    if (!p) return 0;
    if (++pointer_count >= 256) abort();
    pointers[pointer_count] = p;
    return pointer_count;
}
uint32_t ciuki_syscall(uint32_t nr,uint32_t b,uint32_t c,uint32_t d,uint32_t s,uint32_t i,uint32_t p)
{
    calls++;
    if (nr!=101 || c!=64 || d || s || i || p || !b) abort();
    if (syscall_failure) return (uint32_t)-syscall_failure;
    struct ciuki_security_args *a=pointers[b];
    if (a->version!=1 || a->size!=64) abort();
    if (a->operation==CIUKI_SECURITY_SESSION_QUERY) a->state=CIUKI_SECURITY_SESSION_ACTIVE;
    else {
        if (a->output_bytes!=64 || !a->output_address) abort();
        *(struct ciuki_credential *)pointers[a->output_address]=caller;
    }
    return 0;
}
#define VERIFY(x) do { if (!(x)) { fprintf(stderr,"SDK credentials line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    caller=(struct ciuki_credential){.version=1,.size=64,.uid=1000,.primary_gid=545,.group_count=2,.groups={544,545},.session_id=7,.flags=1};
    VERIFY(sizeof(uid_t)==4 && sizeof(gid_t)==4);
    VERIFY(getuid()==1000 && geteuid()==1000 && getgid()==545 && getegid()==545);
    gid_t groups[8];for(unsigned i=0;i<8;i++)groups[i]=0xfeed;
    unsigned before=calls;
    VERIFY(getgroups(-1,groups)==-1 && errno==EINVAL && calls==before);
    VERIFY(getgroups(0,0)==2);
    VERIFY(getgroups(1,groups)==-1 && errno==EINVAL && groups[0]==0xfeed);
    VERIFY(getgroups(8,groups)==2 && groups[0]==544 && groups[1]==545 && groups[2]==0xfeed);
    VERIFY(getgroups(8,0)==-1 && errno==EFAULT);
    struct ciuki_credential c;VERIFY(!ciuki_security_process(2,&c) && c.uid==1000);
    VERIFY(!ciuki_security_peer(3,&c) && c.session_id==7);
    uint32_t state=0;VERIFY(!ciuki_security_session(7,&state) && state==1);
    VERIFY(ciuki_security_session(7,0)==-1 && errno==EFAULT);
    syscall_failure=EIO;
    VERIFY(getuid()==0xffffffff && geteuid()==0xffffffff && errno==EIO);
    VERIFY(getgid()==0xffffffff && getegid()==0xffffffff && errno==EIO);
    state=0xfeed;VERIFY(ciuki_security_session(7,&state)==-1 && state==0xfeed);
    syscall_failure=0;before=calls;
    VERIFY(setuid(0)==-1 && errno==EPERM);VERIFY(seteuid(500)==-1 && errno==EPERM);
    VERIFY(setgid(544)==-1 && errno==EPERM);VERIFY(setegid(0)==-1 && errno==EPERM);
    VERIFY(setreuid(0,0)==-1 && errno==EPERM);VERIFY(setregid(0,0)==-1 && errno==EPERM);
    VERIFY(calls==before);
    puts("H01 SDK: kernel query transport, 32-bit ids, real/effective identity, sorted groups, short buffer, invalid sentinel, EPERM setters PASS");
    return 0;
}
