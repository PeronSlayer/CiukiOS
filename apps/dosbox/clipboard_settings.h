/* SPDX-License-Identifier: MIT. Shared with the G17 Misc property page.
 * These mouse preferences never change the programmatic clipboard API. */
#ifndef DOSBOX_CLIPBOARD_SETTINGS_H
#define DOSBOX_CLIPBOARD_SETTINGS_H
#include "settings.h"
#include <fcntl.h>
#include <stdint.h>
struct dosbox_clipboard_settings { unsigned quick_edit, exclusive; };
static inline uint64_t dosbox_clipboard_key(const char *command)
{
    uint64_t key=14695981039346656037ull;
    const unsigned char *p=(const unsigned char *)command;
    while (*p==' ' || *p=='\t') p++;
    int quoted=*p=='"'; if (quoted) p++;
    for (;*p;p++) {
        unsigned c=*p;
        if (quoted ? c=='"' : c==' ' || c=='\t' || c=='\r' || c=='\n') break;
        if (c>='A' && c<='Z') c+='a'-'A';
        if (c=='/') c='\\';
        key=(key^c)*1099511628211ull;
    }
    return key;
}
static inline int dosbox_clipboard_path(uint64_t key,char *path,size_t size)
{
    int n=snprintf(path,size,DOSBOX_SETTINGS_DIR "/dos-clipboard-%08x%08x.cfg",(unsigned)(key>>32),(unsigned)key);
    return n<0 || (size_t)n>=size ? -ENAMETOOLONG : 0;
}
static inline void dosbox_clipboard_load(uint64_t key,struct dosbox_clipboard_settings *s)
{
    *s=(struct dosbox_clipboard_settings){0};
    char path[160],text[64];
    if (dosbox_clipboard_path(key,path,sizeof(path))) return;
    FILE *f=fopen(path,"rb"); if (!f) return;
    size_t n=fread(text,1,sizeof(text),f); int error=ferror(f); fclose(f);
    static const char grammar[]="DOS clipboard=1\nQuickEdit=0\nExclusive=0\n";
    if (error || n!=sizeof(grammar)-1 || (text[26]!='0' && text[26]!='1') || (text[38]!='0' && text[38]!='1')) return;
    unsigned quick=text[26]-'0',exclusive=text[38]-'0'; text[26]=text[38]='0';
    if (!memcmp(text,grammar,n)) *s=(struct dosbox_clipboard_settings){quick,exclusive};
}
static inline int dosbox_clipboard_save(uint64_t key,const struct dosbox_clipboard_settings *s)
{
    if (s->quick_edit>1 || s->exclusive>1) return -EINVAL;
    char path[160],temp[200],text[64];
    int e=dosbox_clipboard_path(key,path,sizeof(path)); if (e) return e;
    if (mkdir(DOSBOX_SETTINGS_DIR,0700) && errno!=EEXIST) return -errno;
    static unsigned serial;
    int fd=-1;
    for (unsigned tries=0;tries<16 && fd<0;tries++) {
        snprintf(temp,sizeof(temp),"%s.%lu-%u.tmp",path,(unsigned long)getpid(),++serial);
        fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600);
        if (fd<0 && errno!=EEXIST) return -errno;
    }
    if (fd<0) return -EEXIST;
    int n=snprintf(text,sizeof(text),"DOS clipboard=1\nQuickEdit=%u\nExclusive=%u\n",s->quick_edit,s->exclusive);
    ssize_t done=0;
    while (done<n) {
        ssize_t k=write(fd,text+done,(size_t)(n-done));
        if (k<0 && errno==EINTR) continue;
        if (k<=0) { e=k<0 ? -errno : -EIO; break; } done+=k;
    }
    if (!e && fsync(fd)) e=-errno;
    if (close(fd) && !e) e=-errno;
    if (!e && rename(temp,path)) e=-errno;
    if (e) unlink(temp);
    return e;
}
#endif
