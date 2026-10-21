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
    if(argc!=5 && !(argc==6 && !strcmp(argv[5],"--wait")))goto reply;
    waited=argc==6;
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
    else if(kind==DESK_W32_DOS){
        /* The existing DOS autorun bridge splits an unquoted drive/path into
         * drive, CD and executable. Its bound and keyboard policy stay intact. */
        if(strpbrk(path," \t")){result=-EOPNOTSUPP;goto reply;}
        len=snprintf(command,sizeof(command),"C:%s%s%s",path,argv[3][0]?" ":"",argv[3]);
        if(!strncmp(path,"/mnt/",5) && path[5]>='a' && path[5]<='z' && path[6]=='/')
            len=snprintf(command,sizeof(command),"%c:%s%s%s",path[5]-'a'+'A',path+6,argv[3][0]?" ":"",argv[3]);
        for(unsigned i=2;i<(unsigned)len;i++)if(command[i]=='/')command[i]='\\';
        char lines[DOSBOX_LINES_MAX][DOSBOX_COMMAND_MAX+4];
        if(len<0 || len>(int)DOSBOX_COMMAND_MAX || dosbox_command_lines(command,lines)<0){result=-E2BIG;goto reply;}
        child[0]="/Programs/dosbox";child[count++]="--channel-fd=3";
        child[count++]=command;strcpy(path,"/Programs/dosbox");
    }
    child[count]=NULL;
    char *env[]={"LC_ALL=C","TZ=UTC0","PATH=/Programs","HOME=/Documents",NULL};
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
