/* SPDX-License-Identifier: MIT -- constrained browser-to-Files and Win32 folder handoffs. */
#ifndef DESK_BROWSER_LAUNCH_H
#define DESK_BROWSER_LAUNCH_H
#include "../files/operations.h"
#include <stdio.h>
#include <string.h>
/* An absolute path for a Files launch: no empty, "." or ".." component, no
 * control character, shorter than limit. */
static inline int desk_launch_path(const char *path, size_t limit)
{
    if(!path || path[0]!='/' || !path[1] || strlen(path)>=limit || strstr(path,"//"))return 0;
    for(const char *p=path;*p;p++) {
        if((unsigned char)*p<32 || *p==127)return 0;
        if(*p=='/' && (p[1]=='.' && (!p[2] || p[2]=='/' || (p[2]=='.' && (!p[3] || p[3]=='/')))))return 0;
    }
    return 1;
}
static inline int desk_browser_file_argument(const char *arg)
{
    char prefix[48];snprintf(prefix,sizeof(prefix),"--file-action=%d:",FILES_OPEN_WITH);
    const char *path=NULL;
    if(!strncmp(arg,"--open=",7))path=arg+7;
    else if(!strncmp(arg,prefix,strlen(prefix)))path=arg+strlen(prefix);
    return desk_launch_path(path,128);
}
/* fx-q3vise: a Win32 program's ShellExecute(Ex) of a folder opens Files on
 * it, as Windows 98 opens an Explorer window (sdk/win32/shell/execute.c,
 * user32 CiukiShellOpenFolder). The argument is Files' positional folder
 * path: absolute, so never an option. */
static inline int desk_folder_argument(const char *arg)
{
    return desk_launch_path(arg,161);
}
#endif
