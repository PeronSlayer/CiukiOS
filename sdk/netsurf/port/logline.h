/* SPDX-License-Identifier: MIT
 * f6-22: one write(2) per stderr record line.
 * Ciuki's newlib opens stderr unbuffered and read/write (__SRW, as POSIX
 * requires), so vfprintf skips its __sbprintf path for unbuffered streams
 * and writes every conversion separately; the kernel console sink frames
 * each write as its own "[console]" record. "[netsurf] exit reason=" and its
 * value therefore reached the serial line and BOOT.LOG as separate records.
 * Header-only so every port unit (and its host unit tests) can use it. */
#ifndef CIUKI_NETSURF_LOGLINE_H
#define CIUKI_NETSURF_LOGLINE_H
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#define CIUKI_LOG_LINE_MAX 512
/* One line, newline-terminated (added when missing; a longer line is cut and
 * still ends with its newline). Returns its length, or -1. */
static inline int ciuki_line_vformat(char *line,size_t size,const char *format,va_list ap)
{
    if(size<2)return -1;
    int n=vsnprintf(line,size-1,format,ap);
    if(n<0)return -1;
    if((size_t)n>=size-1)n=(int)size-2;
    if(!n || line[n-1]!='\n'){line[n++]='\n';line[n]=0;}
    return n;
}
static inline void ciuki_write_all(int fd,const char *data,size_t bytes)
{
    while(bytes){
        ssize_t n=write(fd,data,bytes);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return;
        data+=n;bytes-=(size_t)n;
    }
}
/* The line goes out as one write(2) unless the console accepts fewer bytes
 * (it takes up to 1 KiB per write). Earlier buffered stderr text first. */
__attribute__((format(printf,1,2)))
static inline int ciuki_log_line(const char *format,...)
{
    char line[CIUKI_LOG_LINE_MAX];va_list ap;va_start(ap,format);
    int n=ciuki_line_vformat(line,sizeof(line),format,ap);va_end(ap);
    if(n<=0)return n;
    fflush(stderr);ciuki_write_all(2,line,(size_t)n);
    return n;
}
#endif
