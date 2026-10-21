/* SPDX-License-Identifier: MIT
 * F2 has no symlinks. Resolve dot components against getcwd and validate each
 * existing component; never use upstream's copy-only realpath fallback. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "file.h"
char *ciuki_realpath(const char *path,char *out)
{
    char joined[PATH_MAX],base[PATH_MAX],resolved[PATH_MAX];
    if(!path || !*path) { errno=ENOENT;return NULL; }
    if(*path=='/') {
        if(strlen(path)>=sizeof(joined)) { errno=ENAMETOOLONG;return NULL; }
        strcpy(joined,path);
    } else {
        if(!getcwd(base,sizeof(base)))return NULL;
        if(snprintf(joined,sizeof(joined),"%s/%s",base,path)>=(int)sizeof(joined)) { errno=ENAMETOOLONG;return NULL; }
    }
    size_t used=1;resolved[0]='/';resolved[1]=0;
    const char *read=joined;
    while(*read) {
        while(*read=='/')read++;
        if(!*read)break;
        const char *end=read;while(*end && *end!='/')end++;
        size_t n=end-read;
        if(n==1 && *read=='.') {}
        else if(n==2 && read[0]=='.' && read[1]=='.') {
            while(used>1 && resolved[used-1]!='/')used--;
            if(used>1)used--;resolved[used]=0;
        } else {
            if(used>1)resolved[used++]='/';
            if(n>=sizeof(resolved)-used) { errno=ENAMETOOLONG;return NULL; }
            memcpy(resolved+used,read,n);used+=n;resolved[used]=0;
        }
        struct stat st;
        if(stat(resolved,&st))return NULL;
        if(*end && !S_ISDIR(st.st_mode)) { errno=ENOTDIR;return NULL; }
        read=end;
    }
    if(!out) { out=malloc(used+1);if(!out)return NULL; }
    memcpy(out,resolved,used+1);return out;
}
int ciuki_readable(const char *path)
{
    FILE *file=fopen(path,"rb");if(!file)return -1;
    return fclose(file);
}
