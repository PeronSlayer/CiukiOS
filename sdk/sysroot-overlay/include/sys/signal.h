/* SPDX-License-Identifier: MIT */
#ifndef _SYS_SIGNAL_H
#define _SYS_SIGNAL_H
#include <sys/types.h>
typedef struct ciuki_siginfo siginfo_t;
typedef struct ciuki_ucontext ucontext_t;
typedef void (*__sighandler_t)(int);
enum { __ciuki_sig_dfl=SIG_DFL, __ciuki_sig_ign=SIG_IGN };
#undef SIG_DFL
#undef SIG_IGN
#define SIG_DFL ((__sighandler_t)__ciuki_sig_dfl)
#define SIG_IGN ((__sighandler_t)__ciuki_sig_ign)
#define SIG_ERR ((__sighandler_t)-1)
/* Default/ignore wire constants become handler pointers in the generated header. */
struct sigaction {
    union { __sighandler_t sa_handler; void (*sa_sigaction)(int,siginfo_t *,void *); };
    uint32_t sa_flags;
    sigset_t sa_mask;
    void (*sa_restorer)(void);
    uint32_t reserved;
};
int sigaction(int,const struct sigaction *,struct sigaction *);
int sigprocmask(int,const sigset_t *,sigset_t *);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *,int);
int sigdelset(sigset_t *,int);
int sigismember(const sigset_t *,int);
int kill(pid_t,int);
int raise(int);
__sighandler_t signal(int,__sighandler_t);
#endif
