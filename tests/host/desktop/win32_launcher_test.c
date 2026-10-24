/* Isolated cwd and the actual native/PE/DOS spawn arguments. */
#include "win32_adapter.h"
#include <ciuki/spawn.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int desk_w32_launcher_main(int,char **);
static char path[260],words[34][1600],cwd[512];
static unsigned maps,launched,sent,waits;
static int wait_status,reply_result;
static unsigned show;
pid_t fixture_waitpid(pid_t pid,int *status,int options)
{assert(pid==123 && !options);waits++;*status=wait_status;return pid;}
ciuki_pid_t ciuki_spawn(const char *p,char *const argv[],char *const env[],const struct ciuki_spawn_fd *fds,uint32_t count,uint32_t flags)
{
    assert(!flags && env && !strcmp(env[0],"LC_ALL=C"));
    assert(env[3] && !strncmp(env[3],"CIUKI_SHELL_SHOW=",17) && !env[3][18]);
    show=(unsigned)(env[3][17]-'0');
    launched++;snprintf(path,sizeof(path),"%s",p);assert(getcwd(cwd,sizeof(cwd)));
    maps=count;if(count)assert(count==1 && fds[0].source==3 && fds[0].target==3);
    memset(words,0,sizeof(words));
    for(unsigned i=0;argv[i];i++){assert(i<34);snprintf(words[i],sizeof(words[i]),"%s",argv[i]);}
    return 123;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{assert(fd==4 && !flags && m->length==8);sent++;reply_result=(int32_t)desk_get32(m->data);return 0;}
int fixture_close(int fd){assert(fd==3 || fd==4);return 0;}
int desk_w32_launch_wait(enum desk_w32_kind k,const struct desk_launch_record *r)
{return desk_w32_launch(k,r);}
int desk_w32_launch(enum desk_w32_kind kind,const struct desk_launch_record *record)
{(void)kind;(void)record;assert(0);return 0;}
int main(int argc,char **argv)
{
    assert(argc==2);char *args[]={"shell-launch","0","C:\\Program Files\\fixture.exe","/remove \"keep spaces\"",argv[1],NULL};
    assert(!desk_w32_launcher_main(5,args));
    assert(!strcmp(path,"/Program Files/fixture.exe") && !maps && launched==1);
    assert(!strcmp(words[1],"\"C:\\Program Files\\fixture.exe\" /remove \"keep spaces\""));assert(!strcmp(cwd,argv[1]));
    assert(show==1);
    char text[]="one \"two words\" empty\\\"quote \"\"";char *native[6]={0};
    assert(desk_w32_arguments(text,native,6)==4);
    assert(!strcmp(native[0],"one") && !strcmp(native[1],"two words") && !strcmp(native[2],"empty\"quote") && !native[3][0]);
    args[1]="2";args[2]="C:\\GAMES\\TEST.COM";args[3]="-fast";
    assert(desk_w32_launcher_main(5,args)==1 && reply_result==-EOPNOTSUPP && launched==1);
    struct desk_launch_record dos={.show=1};strcpy(dos.target,"/mnt/d/SETUP.EXE");
    char command[260];assert(!desk_w32_dos_command(&dos,command,sizeof(command)) && !strcmp(command,"D:\\SETUP.EXE"));
    strcpy(dos.target,"/mnt/e/INSTALL.BAT");strcpy(dos.arguments,"/fast");
    assert(!desk_w32_dos_command(&dos,command,sizeof(command)) && !strcmp(command,"E:\\INSTALL.BAT /fast"));
    args[4]="D:\\bad";assert(desk_w32_launcher_main(5,args)==1 && launched==1);
    assert(sent==3);
    char *wait_args[]={"shell-launch","0","C:\\Program Files\\fixture.exe","/remove",argv[1],"--wait",NULL};
    unsigned before=waits;assert(!desk_w32_launcher_main(6,wait_args) && reply_result==0 && waits==before+1);
    wait_status=3<<8;assert(desk_w32_launcher_main(6,wait_args)==1 && reply_result==-EIO && waits==before+2);
    char *show_args[]={"shell-launch","0","C:\\fixture.exe","",argv[1],"7",NULL};
    assert(!desk_w32_launcher_main(6,show_args) && show==7);
    show_args[5]="3";assert(!desk_w32_launcher_main(6,show_args) && show==3);
    unsigned saved=launched;show_args[5]="10";
    assert(desk_w32_launcher_main(6,show_args)==1 && reply_result==-EINVAL && launched==saved);
    puts("PASS production PE/DOS launcher and shared native argv parser: full quoted command line, native argv quoting, DOS window, cwd isolation, bad cwd refusal");return 0;
}
