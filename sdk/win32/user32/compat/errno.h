#ifndef U32_COMPAT_ERRNO_H
#define U32_COMPAT_ERRNO_H
extern int u32_errno;
#define errno u32_errno
#define EINVAL 22
#define EPIPE 32
#define EAGAIN 11
#endif
