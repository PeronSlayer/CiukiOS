/* SPDX-License-Identifier: MIT
 * Run actual host children through the production native-command controller;
 * exercise libreg against real scratch roots and G29 format via a fake lease. */
#include "../../../apps/terminal/terminal.h"
#include <ciuki/spawn.h>
#include "../../../sdk/sysroot-overlay/include/ciuki/registry.h"
#include <ciuki/devices.h>
#include <assert.h>
#include <stdarg.h>
#include <strings.h>
static struct terminal app;
static char output[2*TERM_CAPTURE];static size_t used;
static unsigned pumps,interrupt_at,queries,writes;static int format_error;
void term_write(struct terminal *t,const char *s)
{ (void)t;size_t n=strlen(s);assert(n<sizeof(output)-used);memcpy(output+used,s,n+1);used+=n; }
void term_printf(struct terminal *t,const char *s,...)
{ char b[20000];va_list args;va_start(args,s);vsnprintf(b,sizeof(b),s,args);va_end(args);term_write(t,b); }
int term_path(struct terminal *t,const char *in,char *out)
{ (void)t;if(strlen(in)>=FM_PATH)return -ENAMETOOLONG;strcpy(out,in);return 0; }
int ciuki_devices(struct ciuki_devices_info *p) { (void)p;errno=ENOSYS;return -1; }
ciuki_pid_t ciuki_spawn(const char *path,char *const args[],char *const env[],const struct ciuki_spawn_fd *maps,uint32_t count,uint32_t flags)
{
    (void)flags;int pid=fork();
    if(!pid) {
        for(unsigned i=0;i<count;i++)assert(dup2(maps[i].source,maps[i].target)>=0);
        for(int fd=3;fd<256;fd++)close(fd);
        execve(path,args,env);_exit(127);
    }
    return pid;
}
int ciuki_proc_terminate(ciuki_pid_t pid,int signo,int auth)
{ assert(auth==-1);return kill(pid,signo); }
static void pump(void *arg)
{ (void)arg;pumps++;if(interrupt_at && pumps>=interrupt_at)app.interrupted=1; }
static int format(void *ctx,int write,struct ciuki_media_format *f)
{
    (void)ctx;assert(f->drive==0);if(format_error)return format_error;
    if(!write) { queries++;f->generation=55;f->sectors=2880;f->filesystem=12;f->supported_filesystems=1;strcpy(f->label,"DISK"); }
    else { writes++;assert(f->generation==55 && f->flags==CIUKI_FORMAT_QUICK && !strcmp(f->label,"TEST"));f->cluster_bytes=512; }
    return 0;
}
static void clear(void) { used=0;output[0]=0; }
static void run(void)
{
    app.host.pump=pump;
    static char input[200001];memset(input,'a',sizeof(input)-1);app.in_data=input;app.in_length=sizeof(input)-1;
    char *cat[]={"/bin/cat"};clear();assert(!term_run_command(&app,1,cat));assert(used==sizeof(input)-1 && pumps>1);
    app.in_data=NULL;app.in_length=0;
    char *error[]={"/bin/sh","-c","echo stdout; echo stderr >&2; exit 7"};clear();
    assert(term_run_command(&app,3,error)==7 && strstr(output,"stdout") && strstr(output,"stderr"));
    char env[]="G27_NATIVE_TEST=visible";app.env[0]=env;app.env_count=1;
    char *echo[]={"/bin/sh","-c","printf '%s' \"$G27_NATIVE_TEST\""};clear();
    assert(!term_run_command(&app,3,echo) && !strcmp(output,"visible"));app.env_count=0;
    char *sleep[]={"/bin/sleep","30"};interrupt_at=pumps+5;clear();
    assert(term_run_command(&app,2,sleep)==-EINTR);app.interrupted=0;interrupt_at=0;
    assert(term_run_command(&app,0,cat)==-EINVAL);
}
static void formats(void)
{
    app.host.format=format;char *args[]={"FORMAT","A:","/Q","/V:TEST","YES"};
    assert(term_format_command(&app,4,args)==-ECANCELED && queries==1 && !writes);
    app.capture=output;assert(term_format_command(&app,5,args)==-ECANCELED && !writes);app.capture=NULL;
    assert(!term_format_command(&app,5,args) && writes==1);
    args[1]="C:";assert(term_format_command(&app,5,args)==-EINVAL && writes==1);args[1]="A:";
    args[2]="/S";assert(term_format_command(&app,5,args)==-EINVAL);args[2]="/Q";
    format_error=-EAGAIN;assert(term_format_command(&app,5,args)==-EAGAIN && writes==1);format_error=0;
}
static void registry(const char *scratch)
{
    char directory[1024];snprintf(directory,sizeof(directory),"%s/registry",scratch);assert(!mkdir(directory,0700));
    char *add[]={"REG","ADD","System\\G27","Number","42"};clear();
    assert(!term_registry_at(&app,5,add,directory));
    char *query[]={"REG","QUERY","System\\G27","Number"};clear();
    assert(!term_registry_at(&app,4,query,directory) && strstr(output,"42"));
    char *string[]={"REG","ADD","System\\G27","Text","STRING","hello world"};assert(!term_registry_at(&app,6,string,directory));
    clear();assert(!term_registry_at(&app,3,query,directory) && strstr(output,"hello world"));
    char *del[]={"REG","DELETE","System\\G27","Number","/F"};
    assert(term_registry_at(&app,4,del,directory)==-EINVAL);assert(!term_registry_at(&app,5,del,directory));
    assert(term_registry_at(&app,4,query,directory)==-ENOENT);
    char path[1024];snprintf(path,sizeof(path),"%s/export.reg",scratch);
    char *export[]={"REG","EXPORT","System\\G27",path};assert(!term_registry_at(&app,4,export,directory));
    char *import[]={"REG","IMPORT","System",path};assert(!term_registry_at(&app,4,import,directory));
    add[4]="bad literal";assert(term_registry_at(&app,5,add,directory)<0);
}
int main(int argc,char **argv)
{
    assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);run();formats();registry(argv[1]);
    puts("terminal-native: PASS (child streams, exit code, environment, cancellation, format confirmation/generation, registry CRUD/import/export)");
}
