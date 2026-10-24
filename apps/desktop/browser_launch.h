/* SPDX-License-Identifier: MIT -- constrained browser-to-Files handoff. */
#ifndef DESK_BROWSER_LAUNCH_H
#define DESK_BROWSER_LAUNCH_H
#include "../files/operations.h"
#include <stdio.h>
#include <string.h>
static inline int desk_browser_file_argument(const char *arg)
{
    char prefix[48];snprintf(prefix,sizeof(prefix),"--file-action=%d:",FILES_OPEN_WITH);
    const char *path=NULL;
    if(!strncmp(arg,"--open=",7))path=arg+7;
    else if(!strncmp(arg,prefix,strlen(prefix)))path=arg+strlen(prefix);
    if(!path || path[0]!='/' || !path[1] || strlen(path)>=128 || strstr(path,"//"))return 0;
    for(const char *p=path;*p;p++) {
        if((unsigned char)*p<32 || *p==127)return 0;
        if(*p=='/' && (p[1]=='.' && (!p[2] || p[2]=='/' || (p[2]=='.' && (!p[3] || p[3]=='/')))))return 0;
    }
    return 1;
}
#endif
