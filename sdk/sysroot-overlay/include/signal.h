/* SPDX-License-Identifier: MIT */
#ifndef _SIGNAL_H_
#define _SIGNAL_H_
#include <sys/signal.h>
typedef int sig_atomic_t;
typedef __sighandler_t _sig_func_ptr;
struct _reent;
_sig_func_ptr _signal_r(struct _reent *,int,_sig_func_ptr);
int _raise_r(struct _reent *,int);
#endif
