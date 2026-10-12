/* SPDX-License-Identifier: MIT */
#ifndef CIUKIDE_JOB_H
#define CIUKIDE_JOB_H
#include "model.h"
#include <sys/types.h>
struct ide_process_ops {
    int (*start)(void *, const char *compiler, char *const argv[], int read_fds[2]);
    ssize_t (*read)(void *, int, char *, size_t);
    int (*wait)(void *, int pid, int *status); /* WNOHANG: 0, pid or -errno */
    void (*close)(void *, int);
};
struct ide_job { int pid, fd[2], reaped, code, io_error; struct ide_model *model; const struct ide_process_ops *ops; void *ctx; };
int ide_job_start(struct ide_job *, struct ide_model *, const struct ide_process_ops *, void *, const char *, const char *folder);
/* Bounded drain of both streams; returns 1 on completion, 0 while active. */
int ide_job_poll(struct ide_job *);
const struct ide_process_ops *ide_native_process(void);
#endif
