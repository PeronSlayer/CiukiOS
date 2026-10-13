/* SPDX-License-Identifier: MIT */
#include "playlist.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *ext(const char *p) { const char *e=strrchr(p,'.'); return e && !strchr(e,'/') ? e : ""; }
int music_is_audio(const char *p) { return !strcasecmp(ext(p),".wav") || !strcasecmp(ext(p),".mp3") || !strcasecmp(ext(p),".ogg"); }
int music_is_playlist(const char *p) { return !strcasecmp(ext(p),".m3u"); }
int music_path(char out[MUSIC_PATH], const char *base, const char *entry)
{
    if (!entry || !*entry || strstr(entry,"://") || strchr(entry,':')) return -EINVAL;
    for (const unsigned char *p=(const unsigned char *)entry;*p;p++) if (*p<32 || *p==127 || *p=='\\') return -EINVAL;
    char joined[MUSIC_PATH], cwd[MUSIC_PATH];
    if (entry[0]=='/') { if (strlen(entry)>=sizeof(joined)) return -ENAMETOOLONG; strcpy(joined,entry); }
    else {
        const char *slash=base ? strrchr(base,'/') : NULL;
        int n;
        if (base && base[0]=='/' && slash) n=snprintf(joined,sizeof(joined),"%.*s/%s",(int)(slash-base),base,entry);
        else { if (!getcwd(cwd,sizeof(cwd))) return -errno; n=snprintf(joined,sizeof(joined),"%s/%s",cwd,entry); }
        if (n<0 || n>=(int)sizeof(joined)) return -ENAMETOOLONG;
    }
    size_t len=1; out[0]='/'; out[1]=0;
    char *p=joined;
    while (*p) {
        while (*p=='/') p++;
        char *end=strchr(p,'/'); size_t n=end ? (size_t)(end-p) : strlen(p);
        if (!n) break;
        if (n==1 && p[0]=='.') { /* ignore */ }
        else if (n==2 && p[0]=='.' && p[1]=='.') {
            if (len>1) { while (len>1 && out[len-1]!='/') len--; if (len>1) len--; out[len]=0; }
        } else {
            if (len>1) out[len++]='/';
            memcpy(out+len,p,n); len+=n; out[len]=0;
        }
        p+=n;
    }
    return 0;
}
int music_playlist_append(struct music_playlist *p, const char *path)
{
    char absolute[MUSIC_PATH]; int r=music_path(absolute,NULL,path); if (r) return r;
    if (!music_is_audio(absolute)) return -EINVAL;
    for (unsigned i=0;i<p->count;i++) if (!strcmp(p->path[i],absolute)) return 0;
    if (p->count==MUSIC_TRACKS) return -E2BIG;
    strcpy(p->path[p->count++],absolute); return 0;
}
static int line(struct music_playlist *p, const char *base, const char *text, size_t n)
{
    if (n && text[n-1]=='\r') n--;
    if (!n || text[0]=='#') return 0;
    if (n>=MUSIC_PATH || memchr(text,0,n)) return -EINVAL;
    char entry[MUSIC_PATH], path[MUSIC_PATH]; memcpy(entry,text,n); entry[n]=0;
    int r=music_path(path,base,entry); return r ? r : music_playlist_append(p,path);
}
int music_playlist_parse(struct music_playlist *p, const char *base, const char *text, size_t size)
{
    if (size>=3 && !memcmp(text,"\xef\xbb\xbf",3)) { text+=3; size-=3; }
    while (size) {
        const char *end=memchr(text,'\n',size); size_t n=end ? (size_t)(end-text) : size;
        int r=line(p,base,text,n); if (r) return r;
        size_t used=n+(end!=NULL); text+=used; size-=used;
    }
    return 0;
}
static int add(struct music_playlist *p, const char *path, unsigned depth, music_cancel cancel, void *ctx)
{
    if (cancel && cancel(ctx)) return -ECANCELED;
    struct stat st;
#ifdef S_ISLNK
    if (lstat(path,&st)) return -errno;
    if (S_ISLNK(st.st_mode)) return 0; /* host aliases never recurse */
#else
    /* The native FAT32 VFS/SDK has no symlinks or lstat. */
    if (stat(path,&st)) return -errno;
#endif
    if (S_ISDIR(st.st_mode)) {
        if (depth==8) return -E2BIG;
        DIR *d=opendir(path); if (!d) return -errno;
        int r=0; struct dirent *e;
        for (;;) {
            errno=0; e=readdir(d); if (!e) { if (errno) r=-errno; break; }
            if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
            char child[MUSIC_PATH]; int n=snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
            if (n<0 || n>=(int)sizeof(child)) { r=-ENAMETOOLONG; break; }
            r=add(p,child,depth+1,cancel,ctx); if (r) break;
        }
        closedir(d); return r;
    }
    if (!S_ISREG(st.st_mode)) return 0;
    if (music_is_audio(path)) return music_playlist_append(p,path);
    if (!music_is_playlist(path) || depth) return 0;
    FILE *f=fopen(path,"rb"); if (!f) return -errno;
    char buf[MUSIC_PATH]; int r=0; unsigned first=1; size_t total=0, n=0;
    for (;;) {
        int ch=fgetc(f);
        if (cancel && cancel(ctx)) { r=-ECANCELED; break; }
        if (ch==EOF || ch=='\n') {
            const char *text=buf; size_t length=n;
            if (first && length>=3 && !memcmp(text,"\xef\xbb\xbf",3)) { text+=3; length-=3; }
            first=0; r=line(p,path,text,length); n=0;
            if (r || ch==EOF) break;
        } else {
            if (!ch || n==sizeof(buf)-1 || ++total>140000) { r=-EINVAL; break; }
            buf[n++]=(char)ch;
        }
    }
    if (ferror(f)) r=-EIO;
    fclose(f); return r;
}
static int by_path(const void *a, const void *b) { return strcmp(a,b); }
int music_playlist_add(struct music_playlist *p, const char *path, music_cancel cancel, void *ctx)
{
    char absolute[MUSIC_PATH]; int r=music_path(absolute,NULL,path); if (r) return r;
    struct stat st; int directory=!stat(absolute,&st) && S_ISDIR(st.st_mode);
    unsigned start=p->count; r=add(p,absolute,0,cancel,ctx);
    if (r) p->count=start;
    else if (directory) qsort(p->path+start,p->count-start,sizeof(p->path[0]),by_path);
    return r;
}
int music_playlist_save(const struct music_playlist *p, const char *path, music_cancel cancel, void *ctx)
{
    if (!music_is_playlist(path)) return -EINVAL;
    char temp[MUSIC_PATH]; int n=snprintf(temp,sizeof(temp),"%s.tmp",path);
    if (n<0 || n>=(int)sizeof(temp)) return -ENAMETOOLONG;
    FILE *f=fopen(temp,"wb"); if (!f) return -errno;
    int r=fputs("#EXTM3U\n",f)<0 ? -EIO : 0;
    for (unsigned i=0;!r && i<p->count;i++) {
        if (cancel && cancel(ctx)) r=-ECANCELED;
        else if (fprintf(f,"%s\n",p->path[i])<0) r=-EIO;
    }
    if (fclose(f)) r=-EIO;
    if (!r && rename(temp,path)) r=-errno;
    if (r) unlink(temp);
    return r;
}
