/* Bounded shell records; no shell interpreter. SPDX-License-Identifier: MIT */
#include "win32_adapter.h"
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
int desk_w32_identify(const char *path)
{
    FILE *f=fopen(path,"rb");if(!f)return -errno;
    uint8_t h[64];size_t n=fread(h,1,sizeof(h),f);
    size_t suffix=strlen(path);
    int result=suffix>=4&&!strcasecmp(path+suffix-4,".msi")?DESK_W32_PE:-ENOEXEC;
    if(n>=4 && !memcmp(h,"\177ELF",4)) result=DESK_W32_ELF;
    else if(n>=2 && h[0]=='M' && h[1]=='Z') {
        result=DESK_W32_DOS;
        if(n==sizeof(h)) {
            uint32_t at=desk_get32(h+60);uint8_t sig[4];
            if(at>=64 && at<=16*1024*1024 && !fseek(f,(long)at,SEEK_SET) &&
                fread(sig,1,4,f)==4 && !memcmp(sig,"PE\0\0",4)) result=DESK_W32_PE;
        }
    } else {
        size_t len=strlen(path);
        if (len>=4 && (!strcasecmp(path+len-4,".com") || !strcasecmp(path+len-4,".bat")) && n) result=DESK_W32_DOS;
    }
    if(ferror(f))result=-EIO;
    if(fclose(f))result=-EIO;
    return result;
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
