/* POSIX file access for the media host harnesses, kept apart from the
 * CiukiOS ABI headers (their O_* values differ from the host's).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
int hf_open(const char *name) { return open(name, O_RDONLY); }
long hf_read(int fd, void *p, unsigned long n) { return (long)read(fd, p, n); }
long hf_seek(int fd, long at, int how) {
  return (long)lseek(fd, at, how == 0 ? SEEK_SET : how == 1 ? SEEK_CUR : SEEK_END);
}
long hf_size(int fd) {
  struct stat st;
  return fstat(fd, &st) ? -1 : (long)st.st_size;
}
void hf_close(int fd) { close(fd); }
int hf_chdir(const char *path) { return chdir(path); }
int hf_write_file(const char *name, const void *p, unsigned long n) {
  int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  long w = (long)write(fd, p, n);
  close(fd);
  return w == (long)n ? 0 : -1;
}
