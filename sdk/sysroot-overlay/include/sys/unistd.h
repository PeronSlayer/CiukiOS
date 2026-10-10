/* SPDX-License-Identifier: MIT */
#ifndef _SYS_UNISTD_H
#define _SYS_UNISTD_H
#include <sys/types.h>
#include <stddef.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
extern char **environ;
void _exit(int) __attribute__((noreturn));
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
ssize_t pread(int, void *, size_t, off_t);
ssize_t pwrite(int, const void *, size_t, off_t);
off_t lseek(int, off_t, int);
int close(int);
int dup(int);
int dup2(int,int);
int fsync(int);
int ftruncate(int,off_t);
int unlink(const char *);
int rmdir(const char *);
int chdir(const char *);
char *getcwd(char *,size_t);
int isatty(int);
int brk(void *);
void *sbrk(ptrdiff_t);
pid_t getpid(void);
pid_t getppid(void);
pid_t fork(void);
pid_t vfork(void);
pid_t _Fork(void);
int execl(const char *,const char *,...);
int execle(const char *,const char *,...);
int execlp(const char *,const char *,...);
int execv(const char *,char *const []);
int execve(const char *,char *const [],char *const []);
int execvp(const char *,char *const []);
int execvpe(const char *,char *const [],char *const []);
int fexecve(int,char *const [],char *const []);
unsigned sleep(unsigned);
int usleep(useconds_t);
int getpagesize(void);
long sysconf(int);
int getentropy(void *,size_t);
int pipe(int [2]);
int chown(const char *,uid_t,gid_t);
int setuid(uid_t);
int setgid(gid_t);
int link(const char *,const char *);
int symlink(const char *,const char *);
#endif
