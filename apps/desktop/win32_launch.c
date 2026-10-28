/* Bounded shell records; no shell interpreter. SPDX-License-Identifier: MIT */
#include "win32_adapter.h"
#include "../dosbox/autorun.h"
#include "exe_kind.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
int desk_w32_native_path(const char *path, char *out, size_t cap)
{
    if (!path || !out || !cap) return -EINVAL;
    unsigned skip=0;
    if (path[0] && path[0]!='/' && path[1]==':' && (path[2]=='/' || path[2]=='\\')) {
        if (path[0]!='C' && path[0]!='c') return -ENODEV;
        skip=2;
    } else if (path[0]!='/') return -EINVAL;
    size_t n=strlen(path+skip);
    if (n>=cap) return -ENAMETOOLONG;
    for (size_t i=0;i<=n;i++) out[i]=path[i+skip]=='\\' ? '/' : path[i+skip];
    return 0;
}
/* fx-autorun: the Windows name of a native path, the inverse of the
 * namespace the kernel loader uses (src/kernel/proc/win32/loader.c): C: is
 * the root, D: to Z: (and A:/B:) are /mnt/<letter>. On Windows a program's
 * GetModuleFileName and command line name its image "D:\\SETUP.EXE"; with
 * that name the kernel's IS5 admission finds SETUP.INS next to the stub
 * whatever the program's current drive is (a POSIX "/mnt/d/..." name is
 * read relative to the current drive by the Win32 loader namespace). */
int desk_w32_windows_path(const char *path, char *out, size_t cap)
{
    if (!path || !out || !cap) return -EINVAL;
    char drive='C'; const char *rest=path;
    if (path[0] && path[0]!='/' && path[1]==':' && (path[2]=='/' || path[2]=='\\' || !path[2])) {
        if (!((path[0]>='A' && path[0]<='Z') || (path[0]>='a' && path[0]<='z'))) return -EINVAL;
        drive=(char)(path[0]&~32); rest=path+2;
    } else if (path[0]!='/') return -EINVAL;
    else if (!strncmp(path,"/mnt/",5) && path[5]>='a' && path[5]<='z' && (path[6]=='/' || !path[6])) {
        drive=(char)(path[5]-'a'+'A'); rest=path+6;
    }
    while (*rest=='/' || *rest=='\\') rest++;
    size_t n=strlen(rest);
    if (n+4>cap) return -ENAMETOOLONG;
    out[0]=drive; out[1]=':'; out[2]='\\';
    for (size_t i=0;i<=n;i++) out[3+i]=rest[i]=='/' ? '\\' : rest[i];
    return 0;
}
/* fx-autorun: Start > Run, Files and My Computer, desktop shortcuts, the
 * Startup folder and AutoPlay all reach this classification. PE images and
 * 16-bit Windows (NE) programs go to the Win32 personality, whose spawn
 * admission starts the InstallShield 5 bootstrap for an IS5 setup stub and
 * refuses any other 16-bit Windows program with its own dialog; every other
 * MZ program (plain DOS, LE/LX DOS-extended, NE for OS/2) runs in the DOS
 * Box. The rule is exe_kind.h's (Wine get_image_params). */
int desk_w32_identify(const char *path)
{
    FILE *f=fopen(path,"rb");if(!f)return -errno;
    uint8_t h[DESK_EXE_HEADER];size_t n=fread(h,1,sizeof(h),f);
    size_t suffix=strlen(path);
    int result=suffix>=4&&!strcasecmp(path+suffix-4,".msi")?DESK_W32_PE:-ENOEXEC;
    if(n>=4 && !memcmp(h,"\177ELF",4)) result=DESK_W32_ELF;
    else if(n>=2 && h[0]=='M' && h[1]=='Z') {
        uint32_t at=desk_exe_lfanew(h,n);uint8_t next[DESK_EXE_HEADER];size_t got=0;
        if(at && !fseek(f,(long)at,SEEK_SET)) got=fread(next,1,sizeof(next),f);
        int kind=desk_exe_classify(h,n,got ? next : NULL,got);
        result=kind==DESK_EXE_WIN32 || kind==DESK_EXE_WIN16 ? DESK_W32_PE : DESK_W32_DOS;
    } else {
        size_t len=strlen(path);
        if (len>=4 && (!strcasecmp(path+len-4,".com") || !strcasecmp(path+len-4,".bat")) && n) result=DESK_W32_DOS;
    }
    if(ferror(f))result=-EIO;
    if(fclose(f))result=-EIO;
    return result;
}
/* Preparation only: the desktop owns the final DOS Box spawn and grants. */
int desk_w32_dos_command(const struct desk_launch_record *r,char *out,size_t cap)
{
    char path[260];int e=desk_w32_native_path(r->target,path,sizeof(path));if(e)return e;
    if(strpbrk(path," \t"))return -EOPNOTSUPP;
    if(r->directory[0]) {
        char requested[260],parent[260];
        e=desk_w32_native_path(r->directory,requested,sizeof(requested));if(e)return e;
        strcpy(parent,path);char *slash=strrchr(parent,'/');if(!slash)return -EINVAL;
        if(slash==parent)slash[1]=0;else *slash=0;
        if(strcasecmp(requested,parent))return -EOPNOTSUPP;
    }
    int n=snprintf(out,cap,"C:%s%s%s",path,r->arguments[0]?" ":"",r->arguments);
    if(!strncmp(path,"/mnt/",5) && path[5]>='a' && path[5]<='z' && path[6]=='/')
        n=snprintf(out,cap,"%c:%s%s%s",path[5]-'a'+'A',path+6,r->arguments[0]?" ":"",r->arguments);
    if(n<0 || (size_t)n>=cap || n>(int)DOSBOX_COMMAND_MAX)return -E2BIG;
    for(unsigned i=2;i<(unsigned)n && out[i]!=' ';i++)if(out[i]=='/')out[i]='\\';
    char lines[DOSBOX_LINES_MAX][DOSBOX_COMMAND_MAX+4];
    return dosbox_command_lines(out,lines)<0 ? -E2BIG : 0;
}
int ciuki_win32_shell_launch(const struct desk_launch_record *record)
{
    if(!record || !desk_launch_valid(record))return -EINVAL;
    char path[260];int error=desk_w32_native_path(record->target,path,sizeof(path));
    if(error)return error;
    int kind=desk_w32_identify(path);
    return kind<0 ? kind : desk_w32_launch((enum desk_w32_kind)kind,record);
}

int ciuki_win32_shell_launch_wait(const struct desk_launch_record *record)
{
    if(!record || !desk_launch_valid(record))return -EINVAL;
    char path[260];int e=desk_w32_native_path(record->target,path,sizeof(path));
    if(e)return e;
    int kind=desk_w32_identify(path);
    return kind<0 ? kind : desk_w32_launch_wait((enum desk_w32_kind)kind,record);
}

/* Windows CRT quote/backslash rules; caller supplies its argv bound. */
int desk_w32_arguments(char *text,char **argv,unsigned cap)
{
    char *read=text,*write=text;unsigned count=0;
    while(*read){
        while(*read==' ' || *read=='\t')read++;
        if(!*read)break;
        if(count==cap)return -E2BIG;
        argv[count++]=write;int quoted=0;
        while(*read && (quoted || (*read!=' ' && *read!='\t'))){
            unsigned slashes=0;while(*read=='\\'){slashes++;read++;}
            if(*read=='"'){
                for(unsigned j=0;j<slashes/2;j++)*write++='\\';
                if(slashes%2)*write++='"';else quoted=!quoted;
                read++;
            }else{
                while(slashes--)*write++='\\';
                if(*read)*write++=*read++;
            }
        }
        if(quoted)return -EINVAL;
        while(*read==' ' || *read=='\t')read++;
        *write++=0;
    }
    return (int)count;
}
