/* SPDX-License-Identifier: MIT
 * G27B native execution and libreg CLI. Existing spawn/file/registry ABI only. */
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include "terminal.h"
#include <ciuki/spawn.h>
#include <ciuki/proc.h>
#include "../../sdk/sysroot-overlay/include/ciuki/registry.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
extern char **environ;

int term_run_command(struct terminal *t,int n,char **a)
{
    if (n<1 || n>=TERM_ARGS) return -EINVAL;
    char path[FM_PATH]; int e=0;
    if (!strpbrk(a[0],"/\\:")) {
        int len=snprintf(path,sizeof(path),"/Programs/%s",a[0]);
        if (len<0 || (size_t)len>=sizeof(path)) return -ENAMETOOLONG;
    } else if ((e=term_path(t,a[0],path))) return e;
    char *args[TERM_ARGS];
    for (int i=0;i<n;i++) args[i]=a[i];
    args[n]=NULL;
    unsigned inherited=0; while (environ && environ[inherited]) inherited++;
    char **env=calloc((size_t)inherited+t->env_count+1,sizeof(*env));
    if (!env) return -ENOMEM;
    unsigned count=0;
    for (unsigned i=0;i<inherited;i++) {
        const char *eq=strchr(environ[i],'='); int override=0;
        for (unsigned j=0;eq && j<t->env_count;j++) {
            size_t len=(size_t)(eq-environ[i]);
            if (!strncasecmp(environ[i],t->env[j],len) && t->env[j][len]=='=') override=1;
        }
        if (!override) env[count++]=environ[i];
    }
    for (unsigned i=0;i<t->env_count;i++) env[count++]=t->env[i];
    int input[2]={-1,-1},output[2]={-1,-1};
    if (pipe(input) || pipe(output)) { e=-errno; goto done; }
    if (fcntl(input[1],F_SETFL,O_NONBLOCK)<0 || fcntl(output[0],F_SETFL,O_NONBLOCK)<0 ||
        fcntl(input[1],F_SETFD,FD_CLOEXEC)<0 || fcntl(output[0],F_SETFD,FD_CLOEXEC)<0) { e=-errno; goto done; }
    struct ciuki_spawn_fd maps[]={{input[0],0},{output[1],1},{output[1],2}};
    int pid=ciuki_spawn(path,args,env,maps,3,CIUKI_SPAWN_NEW_GROUP);
    if (pid<0) { e=-errno; goto done; }
    close(input[0]);input[0]=-1;close(output[1]);output[1]=-1;
    size_t sent=0; int exited=0,eof=0,status=0,killed=0;
    void (*old_pipe)(int)=signal(SIGPIPE,SIG_IGN);
    int old_busy=t->busy;t->busy=1;
    while (!exited || !eof) {
        if (t->host.pump) t->host.pump(t->host.ctx);
        if ((t->interrupted || t->quit || e) && !killed && !exited) {
            if (ciuki_proc_terminate(pid,SIGKILL,-1)<0 && errno!=ESRCH && !e) e=-errno;
            killed=1;
        }
        if (input[1]>=0) {
            if (sent<t->in_length && !killed && !e) {
                ssize_t written=write(input[1],t->in_data+sent,t->in_length-sent);
                if (written>0) sent+=(size_t)written;
                else if (written<0 && errno!=EAGAIN && errno!=EINTR) e=-errno;
            }
            if (sent==t->in_length || killed || e || exited) { close(input[1]);input[1]=-1; }
        }
        char buffer[1025];ssize_t got=read(output[0],buffer,sizeof(buffer)-1);
        if (got>0) { buffer[got]=0;term_write(t,buffer); }
        else if (!got) eof=1;
        else if (errno!=EAGAIN && errno!=EINTR) { eof=1;if(!e)e=-errno; }
        if (!exited) {
            int r=waitpid(pid,&status,WNOHANG);
            if (r==pid) exited=1;
            else if (r<0 && errno!=EINTR) { exited=1;if(!e)e=-errno; }
        }
        /* A descendant may retain stdout after the direct child exits. Drain
         * available bytes but never wait indefinitely for an unrelated writer. */
        if (exited && got<0 && errno==EAGAIN) eof=1;
        if (!exited && got<=0) { struct timespec pause={.tv_sec=0,.tv_nsec=1000000};nanosleep(&pause,NULL); }
    }
    t->busy=old_busy;signal(SIGPIPE,old_pipe);
    if (!e) e=killed ? -EINTR : (status & 0x7f) ? 128+(status & 0x7f) : (status>>8)&255;
    /* Positive child exit status is a shell result, not a native errno. */
    if (e>0) { t->errorlevel=e;term_printf(t,"Process exited with code %d.\n",e); }
 done:
    for (unsigned i=0;i<2;i++) { if(input[i]>=0)close(input[i]);if(output[i]>=0)close(output[i]); }
    free(env); return e;
}

static int fail(void) { return errno ? -errno : -EIO; }
int term_registry_at(struct terminal *t,int n,char **a,const char *directory)
{
    if (n<3) { term_write(t,"REG QUERY root\\key [name]\nREG ADD root\\key [name literal | name STRING text]\nREG DELETE root\\key [name] /F\nREG IMPORT root file\nREG EXPORT root\\key file\nLiteral: quoted string, integer, true/false, or hex:bytes.\n"); return n==1 ? 0 : -EINVAL; }
    char *key=malloc(strlen(a[2])+1);if(!key)return -ENOMEM;
    strcpy(key,a[2]);char *sep=strchr(key,'\\');if(!sep)sep=strchr(key,'/');
    if(sep)*sep=0;
    struct reg_root *root=directory ? reg_open_root_at(key,directory) : reg_open_root(key);
    if(sep)*sep='\\';
    if(!root) { free(key);return fail(); }
    int e=0;
    if (!strcasecmp(a[1],"QUERY")) {
        if(n==4) {
            char *text=malloc(REG_DATA_MAX*4+32);size_t size=REG_DATA_MAX*4+32;
            if(!text)e=-ENOMEM;else if(reg_get_text(root,key,a[3],text,&size))e=fail();
            else term_printf(t,"%s\\%s = %s\n",key,a[3],text);
            free(text);
        } else if(n==3) {
            char *child=malloc(REG_NAME_BYTES+1),*text=malloc(REG_DATA_MAX*4+32);
            if(!child || !text)e=-ENOMEM;
            for(unsigned i=0;!e;i++) {
                if(reg_enum_keys(root,key,i,child,REG_NAME_BYTES+1)) { if(errno!=ENOENT)e=fail();break; }
                term_printf(t,"%s\\%s\n",key,child);
            }
            for(unsigned i=0;!e;i++) {
                struct reg_value_info info;size_t size=REG_DATA_MAX*4+32;
                if(reg_enum_values(root,key,i,&info)) { if(errno!=ENOENT)e=fail();break; }
                if(reg_get_text(root,key,info.name,text,&size))e=fail();
                else term_printf(t,"  %s = %s\n",info.name,text);
            }
            free(child);free(text);
        } else e=-EINVAL;
    } else if (!strcasecmp(a[1],"ADD")) {
        if(n!=3 && n!=5 && n!=6)e=-EINVAL;
        else if(n==6 && strcasecmp(a[4],"STRING"))e=-EINVAL;
        else if(reg_create_key(root,key))e=fail();
        else if(n==5 && reg_set_text(root,key,a[3],a[4]))e=fail();
        else if(n==6 && reg_set_string(root,key,a[3],a[5]))e=fail();
    } else if (!strcasecmp(a[1],"DELETE")) {
        if((n!=4 && n!=5) || strcasecmp(a[n-1],"/F"))e=-EINVAL;
        else if(reg_delete(root,key,n==5 ? a[3] : NULL))e=fail();
    } else if (!strcasecmp(a[1],"IMPORT") || !strcasecmp(a[1],"EXPORT")) {
        char file[FM_PATH];e=n==4 ? term_path(t,a[3],file) : -EINVAL;
        if(!e && (!strcasecmp(a[1],"IMPORT") ? reg_import(root,file) : reg_export(root,key,file)))e=fail();
    } else e=-EINVAL;
    if(!e && strcasecmp(a[1],"QUERY"))term_write(t,"Registry operation completed.\n");
    reg_close_root(root);free(key);return e;
}
int term_registry_command(struct terminal *t,int n,char **a)
{ return term_registry_at(t,n,a,NULL); }
