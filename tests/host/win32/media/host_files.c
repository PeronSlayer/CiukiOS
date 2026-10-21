/* POSIX file access for the media host harnesses, kept apart from the
 * CiukiOS ABI headers (their O_* values differ from the host's).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned open_handles;
unsigned hf_open_handles(void) { return open_handles; }
int hf_open(const char *name) {
  int fd = open(name, O_RDONLY);
  if (fd >= 0) open_handles++;
  return fd;
}
long hf_read(int fd, void *p, unsigned long n) { return (long)read(fd, p, n); }
long hf_seek(int fd, long at, int how) {
  return (long)lseek(fd, at, how == 0 ? SEEK_SET : how == 1 ? SEEK_CUR : SEEK_END);
}
long hf_size(int fd) {
  struct stat st;
  return fstat(fd, &st) ? -1 : (long)st.st_size;
}
int hf_identity(int fd, uint32_t *serial, uint32_t *high, uint32_t *low) {
  struct stat st;
  if (fstat(fd, &st)) return -1;
  *serial = (uint32_t)st.st_dev;
  *high = (uint32_t)((uint64_t)st.st_ino >> 32);
  *low = (uint32_t)st.st_ino;
  return 0;
}
void hf_close(int fd) { if (!close(fd)) open_handles--; }
int hf_chdir(const char *path) { return chdir(path); }
int hf_write_file(const char *name, const void *p, unsigned long n) {
  int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  long w = (long)write(fd, p, n);
  close(fd);
  return w == (long)n ? 0 : -1;
}

/* Real host threads for the close/reopen-during-MCI_WAIT regression. */
#include <pthread.h>
#include <stdlib.h>
#include <assert.h>
static pthread_mutex_t lifetime_lock;
static pthread_once_t lifetime_once = PTHREAD_ONCE_INIT;
static void init_lifetime(void) {
  pthread_mutexattr_t attr;
  assert(!pthread_mutexattr_init(&attr));
  assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
  assert(!pthread_mutex_init(&lifetime_lock, &attr));
  pthread_mutexattr_destroy(&attr);
}
void hf_lock(void) { pthread_once(&lifetime_once, init_lifetime); pthread_mutex_lock(&lifetime_lock); }
void hf_unlock(void) { pthread_mutex_unlock(&lifetime_lock); }
static pthread_t waiter;
void hf_spawn(void *(*fn)(void *)) { assert(!pthread_create(&waiter, 0, fn, 0)); }
void hf_join(void) { assert(!pthread_join(waiter, 0)); }
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate;
void hf_gate_set(int value) {
  pthread_mutex_lock(&gate_lock); gate = value;
  pthread_cond_broadcast(&gate_cv); pthread_mutex_unlock(&gate_lock);
}
void hf_gate_wait(int value) {
  pthread_mutex_lock(&gate_lock);
  while (gate != value) pthread_cond_wait(&gate_cv, &gate_lock);
  pthread_mutex_unlock(&gate_lock);
}
