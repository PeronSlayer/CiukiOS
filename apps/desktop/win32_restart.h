/* Host-testable, nonblocking desktop session/restart driver. SPDX-License-Identifier: MIT */
#ifndef DESK_WIN32_RESTART_H
#define DESK_WIN32_RESTART_H
#include <w32/server.h>
#include <stddef.h>
#include <stdint.h>
struct desk_restart_ops {
    void *ctx;
    uint32_t (*call)(void *,unsigned op,unsigned action,uint32_t flags,struct w32r_reply *,void *,size_t);
    int (*alive)(void *,uint32_t);
    int (*terminate)(void *,uint32_t);
    int (*launch)(void *,const char *); /* Win32 command line, returns PID */
    void (*notice)(void *,unsigned kind,uint32_t pid,uint32_t flags); /* 1 confirm,2 closing,3 resumed,4 plain end */
    void (*record)(void *,const char *,uint32_t);
};
struct desk_restart {
    unsigned stage,boot_checked;
    uint32_t flags,pid,pids[64],count,child;
    uint64_t deadline,next_poll;
    int error;
    char command[2048];
};
int desk_restart_begin(struct desk_restart *,const struct desk_restart_ops *,uint32_t,uint32_t,uint64_t);
void desk_restart_tick(struct desk_restart *,const struct desk_restart_ops *,uint64_t);
#endif
