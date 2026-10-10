/* SPDX-License-Identifier: MIT */
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
static int excluded(void) { errno=ENOSYS;return -1; }
pid_t fork(void) { return excluded(); }
pid_t vfork(void) { return excluded(); }
pid_t _Fork(void) { return excluded(); }
int execl(const char *p,const char *a,...) { (void)p;(void)a;return excluded(); }
int execle(const char *p,const char *a,...) { (void)p;(void)a;return excluded(); }
int execlp(const char *p,const char *a,...) { (void)p;(void)a;return excluded(); }
int execv(const char *p,char *const a[]) { (void)p;(void)a;return excluded(); }
int execve(const char *p,char *const a[],char *const e[]) { (void)p;(void)a;(void)e;return excluded(); }
int execvp(const char *p,char *const a[]) { (void)p;(void)a;return excluded(); }
int execvpe(const char *p,char *const a[],char *const e[]) { (void)p;(void)a;(void)e;return excluded(); }
int fexecve(int f,char *const a[],char *const e[]) { (void)f;(void)a;(void)e;return excluded(); }
int _execve_r(struct _reent *r,const char *p,char *const a[],char *const e[]) { (void)p;(void)a;(void)e;r->_errno=ENOSYS;return -1; }
int _fork_r(struct _reent *r) { r->_errno=ENOSYS;return -1; }
int system(const char *command) { return command?excluded():0; }
FILE *popen(const char *command,const char *type) { (void)command;(void)type;excluded();return NULL; }
int pclose(FILE *p) { (void)p;return excluded(); }
int pipe(int fds[2]) { (void)fds;return excluded(); }
int chmod(const char *p,mode_t m) { (void)p;(void)m;return excluded(); }
int fchmod(int f,mode_t m) { (void)f;(void)m;return excluded(); }
mode_t umask(mode_t m) { (void)m;excluded();return (mode_t)-1; }
int chown(const char *p,uid_t u,gid_t g) { (void)p;(void)u;(void)g;return excluded(); }
int setuid(uid_t u) { (void)u;return excluded(); }
int setgid(gid_t g) { (void)g;return excluded(); }
int link(const char *a,const char *b) { (void)a;(void)b;return excluded(); }
int symlink(const char *a,const char *b) { (void)a;(void)b;return excluded(); }

int _link_r(struct _reent *r,const char *a,const char *b) { (void)a;(void)b;r->_errno=ENOSYS;return -1; }
int _getentropy_r(struct _reent *r,void *b,size_t n) { (void)b;(void)n;r->_errno=ENOSYS;return -1; }
struct tms;
clock_t _times_r(struct _reent *r,struct tms *t) { (void)t;r->_errno=ENOSYS;return -1; }

int getentropy(void *b,size_t n) { (void)b;(void)n;return excluded(); }
