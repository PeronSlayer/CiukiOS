/* Private cwd/argv preparation: chdir here never changes the desktop cwd.
 * SPDX-License-Identifier: MIT */
#include "win32_adapter.h"
#include "../dosbox/autorun.h"
#include <ciuki/channel.h>
#include <ciuki/spawn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc,char **argv)
{
    int result=-EINVAL;
    int waited=0;
    int show=1;
    /* Preserve the old helper invocation for other in-tree callers. */
    int next=5;
    if(argc>5 && strcmp(argv[5],"--wait")){
        if(strlen(argv[5])!=1 || (argv[5][0]!='1' && argv[5][0]!='3' && argv[5][0]!='7'))goto reply;
        show=argv[5][0]-'0';next++;
    }
    if(argc!=next && !(argc==next+1 && !strcmp(argv[next],"--wait")))goto reply;
    waited=argc==next+1;
    if(strlen(argv[1])!=1 || argv[1][0]<'0' || argv[1][0]>'2')goto reply;
    int kind=argv[1][0]-'0';
    if(kind==DESK_W32_ELF)goto reply; /* native UI goes through the desktop launcher */
    char path[260],cwd[260];
    result=desk_w32_native_path(argv[2],path,sizeof(path));if(result)goto reply;
    if(argv[4][0]){
        result=desk_w32_native_path(argv[4],cwd,sizeof(cwd));if(result)goto reply;
        if(chdir(cwd)){result=-errno;goto reply;}
    }
    char command[1600];
    if(strchr(argv[2],'"')){result=-EINVAL;goto reply;}
    int len=snprintf(command,sizeof(command),"\"%s\"%s%s",argv[2],argv[3][0]?" ":"",argv[3]);
    if(len<0 || len>=(int)sizeof(command)){result=-E2BIG;goto reply;}
    char *child[34]={path,NULL};unsigned count=1;
    struct ciuki_spawn_fd map={3,3};unsigned maps=kind!=DESK_W32_PE;
    if(kind==DESK_W32_PE)child[count++]=command;
    else if(kind==DESK_W32_DOS){result=-EOPNOTSUPP;goto reply;}
    child[count]=NULL;
    /* G1: no TZ; the Win32 C runtime reads GetTimeZoneInformation. */
    char state[]="CIUKI_SHELL_SHOW=1";state[sizeof(state)-2]=(char)('0'+show);
    char *env[]={"LC_ALL=C","PATH=/Programs","HOME=/Documents",state,NULL};
    result=ciuki_spawn(path,child,env,maps?&map:NULL,maps,0);if(result<0)result=-errno;
    if(waited && result>0){
        int status=0,pid=result,r;
        do { r=waitpid(pid,&status,0); } while(r<0 && errno==EINTR);
        result=r<0 ? -errno : WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
    }
reply:;
    struct ciuki_message m={.length=8};desk_put32(m.data,(uint32_t)result);
    ciuki_channel_send(4,&m,0);close(4);close(3);
    if(result>0){int status;while(waitpid(result,&status,0)<0 && errno==EINTR){}return 0;}
    return waited && !result ? 0 : 1;
}
