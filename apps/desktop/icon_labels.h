/* SPDX-License-Identifier: MIT
 * Shared, I/O-free reader for the desktop.cfg namespace display names. */
#ifndef CIUKI_ICON_LABELS_H
#define CIUKI_ICON_LABELS_H
#include <stddef.h>
#include <string.h>
static inline int icon_label_key(char *out, const char *name)
{
    static const char hex[]="0123456789abcdef";
    size_t n=strlen(name); if (n>764) return 0; /* 10 + 2*n + '=' + NUL fits 1540. */
    memcpy(out,"iconlabel.",10);
    for (size_t i=0;i<n;i++) { unsigned c=(unsigned char)name[i]; out[10+i*2]=hex[c>>4]; out[11+i*2]=hex[c&15]; }
    out[10+n*2]='='; out[11+n*2]=0; return 1;
}
static inline int icon_label_read(const char *text, const char *name, char *out, size_t size)
{
    char key[1540]; if (!text || !name || !out || !size || !icon_label_key(key,name)) return 0;
    size_t n=strlen(key);
    for (const char *p=text; *p;) {
        const char *end=strchr(p,'\n'); if (!end) end=p+strlen(p);
        size_t bytes=(size_t)(end-p);
        if (bytes>=n && !memcmp(p,key,n)) {
            size_t len=bytes-n;
            if (len && p[n+len-1]=='\r') len--;
            if (!len || len>=64 || len>=size) return 0;
            for (size_t i=0;i<len;i++) if ((unsigned char)p[n+i]<32) return 0;
            memcpy(out,p+n,len); out[len]=0; return 1;
        }
        p=*end ? end+1 : end;
    }
    return 0;
}
#endif
