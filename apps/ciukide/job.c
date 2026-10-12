/* SPDX-License-Identifier: MIT */
#include "job.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
/* Target status macros are in the public ABI; host builds use POSIX's macros. */
#ifdef __CIUKIOS__
#include <ciuki/abi.h>
#endif
int ide_job_start(struct ide_job *j, struct ide_model *m, const struct ide_process_ops *ops, void *ctx,
                  const char *compiler, const char *folder)
{
    if (!folder || folder[0]!='/' || strlen(folder)>=IDE_NAME) return -EINVAL;
    int r=ide_build_begin(m); if (r) return r;
    memset(j,0,sizeof(*j)); j->fd[0]=j->fd[1]=-1; j->model=m; j->ops=ops; j->ctx=ctx;
    char names[IDE_SOURCES+1][IDE_NAME], libs[IDE_LIBRARIES][68];
    char *argv[IDE_SOURCES+IDE_LIBRARIES+4]; unsigned at=0;
    argv[at++]=(char *)compiler; argv[at++]="-o";
    int n=snprintf(names[IDE_SOURCES],IDE_NAME,"%s/%s",folder,m->project.output);
    if (n<0 || n>=IDE_NAME) { ide_build_end(m,1); return -ENAMETOOLONG; }
    argv[at++]=names[IDE_SOURCES];
    for (unsigned i=0;i<m->project.source_count;++i) {
        n=snprintf(names[i],IDE_NAME,"%s/%s",folder,m->project.sources[i]);
        if (n<0 || n>=IDE_NAME) { ide_build_end(m,1); return -ENAMETOOLONG; }
        argv[at++]=names[i];
    }
    for (unsigned i=0;i<m->project.library_count;++i) {
        snprintf(libs[i],sizeof(libs[i]),"-l%s",m->project.libraries[i]); argv[at++]=libs[i];
    }
    argv[at]=NULL;
    j->pid=ops->start(ctx,compiler,argv,j->fd);
    if (j->pid<0) { r=j->pid; j->pid=0; ide_build_end(m,1); return r; }
    return 0;
}
int ide_job_poll(struct ide_job *j)
{
    if (!j->pid) return 0;
    char buf[512];
    for (unsigned stream=0;stream<2;++stream) for (unsigned budget=0;budget<16 && j->fd[stream]>=0;++budget) {
        ssize_t n=j->ops->read(j->ctx,j->fd[stream],buf,sizeof(buf));
        if (n>0) { ide_output(j->model,stream,buf,(size_t)n); continue; }
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) break;
        if (n<0) j->io_error=errno ? errno : EIO;
        ide_output_end(j->model,stream); j->ops->close(j->ctx,j->fd[stream]); j->fd[stream]=-1;
    }
    if (!j->reaped) {
        int status=0, r=j->ops->wait(j->ctx,j->pid,&status);
        if (r==j->pid) { j->reaped=1; j->code=WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status); }
        else if (r<0 && r!=-EINTR) { j->reaped=1; j->io_error=-r; j->code=1; }
    }
    if (j->reaped && j->fd[0]<0 && j->fd[1]<0) {
        ide_build_end(j->model,j->io_error ? 1 : j->code); j->pid=0; return 1;
    }
    return 0;
}
