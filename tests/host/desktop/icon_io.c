/* POSIX read boundary fault injection for the real icon reader.
 * Linked with --wrap=pread by test_icons.py; production has no test flags.
 * SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
ssize_t __real_pread(int,void *,size_t,off_t);
ssize_t __wrap_pread(int fd,void *buffer,size_t bytes,off_t offset)
{
    static int interrupted;
    const char *limit=getenv("ICONS_IO_LIMIT"), *eof=getenv("ICONS_IO_EOF_AT");
    if (getenv("ICONS_IO_INTERRUPT") && !interrupted++) { errno=EINTR; return -1; }
    if (eof && offset>=(off_t)strtoul(eof,NULL,10)) return 0;
    if (limit) { size_t n=strtoul(limit,NULL,10); if (n && bytes>n) bytes=n; }
    return __real_pread(fd,buffer,bytes,offset);
}
