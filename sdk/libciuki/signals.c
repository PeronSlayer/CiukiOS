/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static sigset_t supported(void) { return CIUKI_SIGBIT(SIGINT)|CIUKI_SIGBIT(SIGILL)|CIUKI_SIGBIT(SIGABRT)|CIUKI_SIGBIT(SIGBUS)|CIUKI_SIGBIT(SIGFPE)|CIUKI_SIGBIT(SIGKILL)|CIUKI_SIGBIT(SIGUSR1)|CIUKI_SIGBIT(SIGSEGV)|CIUKI_SIGBIT(SIGUSR2)|CIUKI_SIGBIT(SIGPIPE)|CIUKI_SIGBIT(SIGTERM)|CIUKI_SIGBIT(SIGCHLD); }
static int valid(int s) { return s>0&&s<=(int)(sizeof(sigset_t)*8)&&(supported()&CIUKI_SIGBIT(s)); }
static int ret(uint32_t r) { int e=ciuki_error(r);if(e) { errno=e;return -1; }return r; }
int sigemptyset(sigset_t *s) { if(!s) { errno=EINVAL;return -1; }*s=0;return 0; }
int sigfillset(sigset_t *s) { if(!s) { errno=EINVAL;return -1; }*s=supported();return 0; }
int sigaddset(sigset_t *s,int n) { if(!s||!valid(n)) { errno=EINVAL;return -1; }*s|=CIUKI_SIGBIT(n);return 0; }
int sigdelset(sigset_t *s,int n) { if(!s||!valid(n)) { errno=EINVAL;return -1; }*s&=~CIUKI_SIGBIT(n);return 0; }
int sigismember(const sigset_t *s,int n) { if(!s||!valid(n)) { errno=EINVAL;return -1; }return !!(*s&CIUKI_SIGBIT(n)); }
int sigaction(int s,const struct sigaction *a,struct sigaction *old) {
    _Static_assert(sizeof(struct sigaction)==sizeof(struct ciuki_sigaction),"signal action wire size");
    struct sigaction copy;if(a) { copy=*a;copy.sa_restorer=__ciuki_sigreturn; }
    return ret(CU_CALL(SIGACTION,s,CU_PTR(a?&copy:NULL),CU_PTR(old),0,0,0));
}
int sigprocmask(int how,const sigset_t *s,sigset_t *old) { return ret(CU_CALL(SIGPROCMASK,how,CU_PTR(s),CU_PTR(old),0,0,0)); }
int kill(pid_t pid,int sig) { return ret(CU_CALL(KILL,pid,sig,0,0,0,0)); }
int _kill_r(struct _reent *r,int pid,int sig) { uint32_t v=CU_CALL(KILL,pid,sig,0,0,0,0);int e=ciuki_error(v);if(e) { r->_errno=e;return -1; }return v; }
int raise(int sig) { return ret(CU_CALL(THREAD_KILL,pthread_self(),sig,0,0,0,0)); }
__sighandler_t signal(int sig,__sighandler_t handler) { struct sigaction a={.sa_handler=handler},old;if(sigaction(sig,&a,&old)<0)return SIG_ERR;return old.sa_handler; }
void abort(void) {
    sigset_t set=CIUKI_SIGBIT(SIGABRT);(void)sigprocmask(SIG_UNBLOCK,&set,NULL);(void)raise(SIGABRT);
    struct sigaction action={.sa_handler=(__sighandler_t)SIG_DFL};(void)sigaction(SIGABRT,&action,NULL);(void)raise(SIGABRT);
    _exit(128+SIGABRT);
}

int _raise_r(struct _reent *r,int sig) { uint32_t v=CU_CALL(THREAD_KILL,pthread_self(),sig,0,0,0,0);int e=ciuki_error(v);if(e) { r->_errno=e;return -1; }return v; }
__sighandler_t _signal_r(struct _reent *r,int sig,__sighandler_t handler) { int saved=errno;__sighandler_t v=signal(sig,handler);if(v==SIG_ERR)r->_errno=errno;if(r!=__getreent())errno=saved;return v; }
