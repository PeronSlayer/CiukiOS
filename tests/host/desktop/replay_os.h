/* SPDX-License-Identifier: MIT
 * Forced include (-include) for the programs the app-case replay runs as
 * threads of one host process (app_replay.c): their process-level calls go to
 * the replay's virtual kernel, never to the host. Production sources are
 * compiled unchanged; only these names are redirected. */
#ifndef CIUKI_REPLAY_OS_H
#define CIUKI_REPLAY_OS_H
#include "replay_base.h"
int rp_nanosleep(const struct timespec *, struct timespec *);
int rp_clock_gettime(clockid_t, struct timespec *);
char *rp_getenv(const char *);
int rp_open(const char *, int, ...);
int rp_close(int);
int rp_munmap(void *, size_t);
void *rp_mmap(void *, size_t, int, int, int, off_t);
int rp_fcntl(int, int, ...);
pid_t rp_waitpid(pid_t, int *, int);
int rp_kill(pid_t, int);
pid_t rp_getpid(void);
DIR *rp_opendir(const char *);
struct dirent *rp_readdir(DIR *);
int rp_closedir(DIR *);
int rp_stat(const char *, struct stat *);
FILE *rp_fopen(const char *, const char *);
int rp_uname(struct utsname *);
_Noreturn void rp_exit(int);
#define nanosleep(a, b) rp_nanosleep(a, b)
#define clock_gettime(a, b) rp_clock_gettime(a, b)
#define getenv(a) rp_getenv(a)
#define open(...) rp_open(__VA_ARGS__)
#define close(a) rp_close(a)
#define munmap(a, b) rp_munmap(a, b)
#define mmap(...) rp_mmap(__VA_ARGS__)
#define fcntl(...) rp_fcntl(__VA_ARGS__)
#define waitpid(a, b, c) rp_waitpid(a, b, c)
#define kill(a, b) rp_kill(a, b)
#define getpid() rp_getpid()
#define opendir(a) rp_opendir(a)
#define readdir(a) rp_readdir(a)
#define closedir(a) rp_closedir(a)
#define stat(a, b) rp_stat(a, b)
#define fopen(a, b) rp_fopen(a, b)
#define uname(a) rp_uname(a)
#define _exit(a) rp_exit(a)
#endif
