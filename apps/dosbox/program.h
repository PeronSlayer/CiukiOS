/* G17: bounded per-executable DOS profiles. Private disk format, not an ABI.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_PROGRAM_H
#define CIUKI_DOSBOX_PROGRAM_H
#include "settings.h"
#include "autorun.h"
#include <stdint.h>
#include <fcntl.h>

#define DOSBOX_PROFILE_MAGIC 0x32504644u /* DFP2 (fx-dosfull: screen usage 3) */
#define DOSBOX_PROFILE_MAGIC_V1 0x31504644u /* DFP1, migrated on load */
/* fx-dosfull: Screen usage (Windows 98 Screen tab, "Usage"; Resource Kit
 * ch. 25 "Running Games"). GRAPHICS is the default of every DOS program, as
 * the owner decided on 2026-10-27: its text screens stay in the window and its
 * first graphics mode switches to full screen. The DOS prompt itself starts
 * in a window. FULL is the whole session, text included; EXCLUSIVE also pauses
 * in the background (the MS-DOS mode equivalent, G17). */
enum { DOSBOX_SCREEN_WINDOW, DOSBOX_SCREEN_FULL, DOSBOX_SCREEN_EXCLUSIVE, DOSBOX_SCREEN_GRAPHICS };
#define DOSBOX_SCREEN_MAX DOSBOX_SCREEN_GRAPHICS
struct dosbox_profile {
    uint32_t magic;
    char key[97], command[97], directory[97], batch[97];
    uint32_t close_on_exit, zoom, memory, screen, background;
    uint32_t quickedit, fast_paste, warn, idle;
};

/* DOS executable names are case-insensitive. Arguments deliberately do not
 * identify another program. Quoted paths are not yet accepted by the typist. */
static inline int dosbox_program_key(const char *command, char key[97])
{
    size_t n=strcspn(command," \t");
    if (!n) { memcpy(key,"@PROMPT",8); return 0; }
    if (n>96) { errno=EINVAL; return -1; }
    for (size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)command[i];
        if (c<33 || c>126 || c=='"') { errno=EINVAL; return -1; }
        if (c=='/') c='\\';
        key[i]=(char)(c>='a' && c<='z' ? c-32 : c);
    }
    key[n]=0; return 0;
}
static inline void dosbox_profile_default(struct dosbox_profile *p, const char *command)
{
    memset(p,0,sizeof(*p)); p->magic=DOSBOX_PROFILE_MAGIC;
    (void)dosbox_program_key(command,p->key);
    snprintf(p->command,sizeof(p->command),"%s",command);
    p->close_on_exit=1; p->background=1; p->warn=1; p->idle=50;
    p->screen=strcmp(p->key,"@PROMPT") ? DOSBOX_SCREEN_GRAPHICS : DOSBOX_SCREEN_WINDOW;
}
static inline int dosbox_profile_path(const char *key, char *path, size_t size)
{
    uint64_t hash=UINT64_C(14695981039346656037);
    for (const unsigned char *s=(const unsigned char *)key;*s;s++) { hash^=*s; hash*=UINT64_C(1099511628211); }
    int n=snprintf(path,size,DOSBOX_SETTINGS_DIR "/dos-program-%016llx.cfg",(unsigned long long)hash);
    if (n<0 || (size_t)n>=size) { errno=ENAMETOOLONG; return -1; }
    return 0;
}
static inline int dosbox_profile_text(const char *s, size_t size)
{
    const char *end=memchr(s,0,size); if (!end) return 0;
    for (;s<end;s++) { int shift; if (!dosbox_key_for(*s,&shift)) return 0; }
    return 1;
}
static inline int dosbox_profile_valid(const struct dosbox_profile *p)
{
    if (p->magic!=DOSBOX_PROFILE_MAGIC || !memchr(p->key,0,sizeof(p->key)) || !p->key[0] ||
        !dosbox_profile_text(p->command,sizeof(p->command)) ||
        !dosbox_profile_text(p->directory,sizeof(p->directory)) ||
        !dosbox_profile_text(p->batch,sizeof(p->batch))) return 0;
    /* Paths are arguments, never shell syntax. Keep them unambiguous. */
    static const char forbidden[]=" \"&|<>";
    if (strpbrk(p->directory,forbidden) || strpbrk(p->batch,forbidden)) return 0;
    if (p->command[0]) {
        char lines[DOSBOX_LINES_MAX][DOSBOX_COMMAND_MAX+4];
        if (dosbox_command_lines(p->command,lines)<0) return 0;
    }
    if (p->directory[0] && strlen(p->directory)>DOSBOX_COMMAND_MAX-3) return 0;
    return p->close_on_exit<=1 && p->zoom<=2 && p->memory<=1 && p->screen<=DOSBOX_SCREEN_MAX &&
           p->background<=1 && p->quickedit<=1 && p->fast_paste<=1 && p->warn<=1 && p->idle<=100;
}
/* 1 loaded, 0 missing, -1 invalid/I/O. The caller keeps complete defaults. */
static inline int dosbox_profile_load(struct dosbox_profile *p)
{
    char path[192]; struct dosbox_profile next;
    if (dosbox_profile_path(p->key,path,sizeof(path))) return -1;
    FILE *f=fopen(path,"rb"); if (!f) return errno==ENOENT ? 0 : -1;
    size_t n=fread(&next,1,sizeof(next),f);
    int extra=fgetc(f), error=ferror(f), close_error=fclose(f);
    /* fx-dosfull: a DFP1 record had no usage of its own: "Window" was its
     * default, so a program's Window becomes the new default; an explicit
     * full-screen choice (1, 2) is kept. The prompt keeps its window. */
    if (n==sizeof(next) && next.magic==DOSBOX_PROFILE_MAGIC_V1 && next.screen<=DOSBOX_SCREEN_EXCLUSIVE) {
        next.magic=DOSBOX_PROFILE_MAGIC;
        if (next.screen==DOSBOX_SCREEN_WINDOW && memchr(next.key,0,sizeof(next.key)) && strcmp(next.key,"@PROMPT"))
            next.screen=DOSBOX_SCREEN_GRAPHICS;
    }
    if (n!=sizeof(next) || extra!=EOF || error || close_error || !dosbox_profile_valid(&next) || strcmp(next.key,p->key)) {
        errno=EINVAL; return -1;
    }
    *p=next; return 1;
}
static inline int dosbox_profile_save(const struct dosbox_profile *p)
{
    if (!dosbox_profile_valid(p)) { errno=EINVAL; return -1; }
    char path[192],temp[224];
    if (dosbox_profile_path(p->key,path,sizeof(path))) return -1;
    /* Refuse hash collisions; a malformed profile for this name can be repaired. */
    FILE *old=fopen(path,"rb");
    if (old) {
        struct dosbox_profile previous;
        size_t n=fread(&previous,1,sizeof(previous),old); int error=ferror(old); fclose(old);
        if (error) { errno=EIO; return -1; }
        if (n==sizeof(previous) && memchr(previous.key,0,sizeof(previous.key)) && strcmp(previous.key,p->key)) {
            errno=EEXIST; return -1;
        }
    } else if (errno!=ENOENT) return -1;
    if (mkdir(DOSBOX_SETTINGS_DIR,0755) && errno!=EEXIST) return -1;
    snprintf(temp,sizeof(temp),"%s.tmp.%lu",path,(unsigned long)getpid());
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600); if (fd<0) return -1;
    const unsigned char *bytes=(const unsigned char *)p; size_t at=0;
    while (at<sizeof(*p)) {
        ssize_t n=write(fd,bytes+at,sizeof(*p)-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { int e=n<0 ? errno : EIO; close(fd); unlink(temp); errno=e; return -1; }
        at+=(size_t)n;
    }
    if (fsync(fd)) { int e=errno; close(fd); unlink(temp); errno=e; return -1; }
    if (close(fd)) { int e=errno; unlink(temp); errno=e; return -1; }
    if (rename(temp,path)) { int e=errno; unlink(temp); errno=e; return -1; }
    return 0;
}
/* fx-dosfull: the usage after the user's own switch (Alt+Enter, the toolbar
 * or the window menu), as Windows 98 remembers the mode a program last used.
 * Back to the window is Window. Into full screen from Window: graphics full
 * screen when the program is in a graphics mode, else the whole session; a
 * text screen chosen full screen also widens graphics-only usage. */
static inline uint32_t dosbox_usage_after(uint32_t usage, int fullscreen, int graphics)
{
    if (!fullscreen) return DOSBOX_SCREEN_WINDOW;
    if (usage==DOSBOX_SCREEN_WINDOW) return graphics ? DOSBOX_SCREEN_GRAPHICS : DOSBOX_SCREEN_FULL;
    if (usage==DOSBOX_SCREEN_GRAPHICS && !graphics) return DOSBOX_SCREEN_FULL;
    return usage;
}
static inline const char *dosbox_usage_name(uint32_t usage)
{
    static const char *const names[]={"window","full","exclusive","graphics"};
    return usage<=DOSBOX_SCREEN_MAX ? names[usage] : "invalid";
}
/* Store only the usage of the program named by key. Every other stored field
 * is kept; a program without a stored profile gets the defaults with an
 * empty command line, which keeps the command its launcher types (a Games
 * entry's arguments, a shortcut's), rather than freezing this launch's one. */
static inline int dosbox_profile_usage_save(const char *key, uint32_t usage)
{
    struct dosbox_profile p;
    if (usage>DOSBOX_SCREEN_MAX) { errno=EINVAL; return -1; }
    dosbox_profile_default(&p,key);
    if (strcmp(p.key,key)) { errno=EINVAL; return -1; }
    if (dosbox_profile_load(&p)<1) { dosbox_profile_default(&p,key); p.command[0]=0; }
    p.screen=usage;
    return dosbox_profile_save(&p);
}
/* The profile key of a DOS program opened by its native path (Files, desktop
 * icons): the command the desktop types for it (apps/desktop/win32_launch.c
 * desk_w32_dos_command: "C:" and the path, or "<drive>:" for /mnt/<drive>/).
 * A path with blanks cannot be typed and has no profile. */
static inline int dosbox_native_key(const char *path, char key[97])
{
    char command[DOSBOX_COMMAND_MAX+1];
    if (!path || path[0]!='/' || strpbrk(path," \t")) { errno=EINVAL; return -1; }
    int n=!strncmp(path,"/mnt/",5) && path[5]>='a' && path[5]<='z' && path[6]=='/' ?
        snprintf(command,sizeof(command),"%c:%s",path[5]-'a'+'A',path+6) : snprintf(command,sizeof(command),"C:%s",path);
    if (n<0 || (size_t)n>=sizeof(command)) { errno=ENAMETOOLONG; return -1; }
    return dosbox_program_key(command,key);
}
/* 1: an MS-DOS program the desktop starts in a DOS Box (desk_w32_identify:
 * MZ without a PE header, or a non-empty .COM/.BAT); 0: not; -1: errno. */
static inline int dosbox_dos_program(const char *path)
{
    FILE *f=fopen(path,"rb"); if (!f) return -1;
    unsigned char h[64]; size_t n=fread(h,1,sizeof(h),f);
    int dos=0;
    if (n>=2 && h[0]=='M' && h[1]=='Z') {
        dos=1;
        if (n==sizeof(h)) {
            uint32_t at=(uint32_t)h[60]|(uint32_t)h[61]<<8|(uint32_t)h[62]<<16|(uint32_t)h[63]<<24;
            unsigned char sig[4];
            if (at>=64 && at<=16u*1024u*1024u && !fseek(f,(long)at,SEEK_SET) && fread(sig,1,4,f)==4 && !memcmp(sig,"PE\0\0",4)) dos=0;
        }
    } else if (n>=2 && h[0]==0x7f && h[1]=='E') dos=0;
    else {
        size_t len=strlen(path);
        if (n && len>=4 && path[len-4]=='.') {
            char ext[4]; for (unsigned i=0;i<3;i++) { char c=path[len-3+i]; ext[i]=(char)(c>='a' && c<='z' ? c-32 : c); }
            ext[3]=0; dos=!strcmp(ext,"COM") || !strcmp(ext,"BAT");
        }
    }
    int error=ferror(f); if (fclose(f)) error=1;
    if (error) { errno=EIO; return -1; }
    return dos;
}
/* Prefix commands wait for the actual prompt, just like existing CD steps.
 * The final command's Enter remains the only LAUNCH arm point. */
static inline int dosbox_profile_line(const char *line, struct dosbox_key *keys, unsigned at, unsigned capacity)
{
    for (size_t i=0;;i++) {
        int shift=0;
        unsigned code=line[i] ? dosbox_key_for(line[i],&shift) : CIUKI_KEY_ENTER;
        if (!code || at+4>capacity) return -1;
        uint16_t group=(uint16_t)at, main=(uint16_t)(at+!!shift);
        if (shift) keys[at++]=(struct dosbox_key){CIUKI_KEY_LEFTSHIFT,1,0,group,main};
        keys[at++]=(struct dosbox_key){(uint16_t)code,1,0,group,main};
        keys[at++]=(struct dosbox_key){(uint16_t)code,0,(uint8_t)!line[i],group,main};
        if (shift) keys[at++]=(struct dosbox_key){CIUKI_KEY_LEFTSHIFT,0,0,group,main};
        if (!line[i]) break;
    }
    return (int)at;
}
static inline int dosbox_profile_keys(const struct dosbox_profile *p, struct dosbox_key *keys, unsigned capacity)
{
    unsigned at=0;
    if (!dosbox_profile_valid(p)) return -1;
    const char *prefix[]={p->directory,p->batch};
    for (unsigned i=0;i<2;i++) if (*prefix[i]) {
        char line[100];
        if (!i && prefix[i][1]==':') {
            char drive[3]={prefix[i][0],':',0};
            int n=dosbox_profile_line(drive,keys,at,capacity);
            if (n<0) return -1;
            at=(unsigned)n;
        }
        snprintf(line,sizeof(line),"%s%s",i ? "" : "CD ",prefix[i]);
        int n=dosbox_profile_line(line,keys,at,capacity);
        if (n<0) return -1;
        at=(unsigned)n;
    }
    if (*p->command) {
        /* An explicit working directory must win over the auto-CD convenience. */
        const char *line=p->command;
        if (p->directory[0]) {
            /* A full executable path is valid at any working directory. */
            int n=dosbox_profile_line(line,keys,at,capacity);
            if (n<0) return -1;
            at=(unsigned)n;
        } else {
            int n=dosbox_command_keys(line,keys+at,capacity-at); if (n<0) return -1;
            /* Rebase focus-resume indices from the appended command. */
            for (unsigned j=0;j<(unsigned)n;j++) { keys[at+j].group+=(uint16_t)at; keys[at+j].main+=(uint16_t)at; }
            at+=(unsigned)n;
        }
    }
    return (int)at;
}
#endif
