/* G21 production model, real registry RPC and durable sidecar. SPDX-License-Identifier: MIT */
#include "startup.h"
#include "win32_adapter.h"
#include "../../../apps/w32srv/registry.h"
#include <assert.h>
#include <ciuki/devices.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static struct wr_server server;
static int launches, consumed, fail_launch, unavailable;
int ciuki_devices(struct ciuki_devices_info *s) { (void)s; errno=ENOSYS; return -1; }
uint32_t desk_w32_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{
    if (unavailable) return 1062;
    size_t size=0; uint32_t error=wr_dispatch(&server,42,in,n,out,cap,&size); *used=(uint32_t)size; return error;
}
int desk_w32_launch(enum desk_w32_kind k,const struct desk_launch_record *r)
{ (void)k; assert(desk_launch_valid(r)); launches++; return fail_launch ? -EIO : 123; }
int desk_w32_launch_wait(enum desk_w32_kind k,const struct desk_launch_record *r) { return desk_w32_launch(k,r); }
static int launch(const struct desk_launch_record *r) { return desk_w32_launch(DESK_W32_ELF,r); }
static int consume(const struct desk_startup_entry *e) { consumed++; return ciuki_win32_startup_consume(e); }
static struct wr_value *find_value(struct wr_key *k,const char *name)
{ for (struct wr_value *v=k->values;v;v=v->next) if (!strcmp(v->name,name)) return v; return NULL; }
static struct wr_key *key(unsigned user,const char *leaf)
{
    struct wr_key *k=user ? wr_child(&server,server.roots[1],".DEFAULT") : server.roots[0];
    const char *parts[]={"Software","Microsoft","Windows","CurrentVersion",leaf};
    for (unsigned i=0;i<5;i++) { struct wr_key *next=wr_child(&server,k,parts[i]); if (!next) next=wr_add_key(&server,k,parts[i],0); assert(next); k=next; }
    return k;
}
static void value(struct wr_key *k,const char *name,const char *text)
{
    uint8_t data[4096]; size_t n=strlen(text); assert(n<sizeof(data)/2);
    for (size_t i=0;i<=n;i++) { data[i*2]=(uint8_t)text[i]; data[i*2+1]=0; }
    assert(!wr_put(&server,k,name,1,data,(uint32_t)(n+1)*2));
}
static void file(const char *path,const void *bytes,size_t n)
{ FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(bytes,1,n,f)==n); assert(!fclose(f)); }
int main(int argc,char **argv)
{
    struct desk_startup_gate gate={0};
    assert(!desk_startup_due(&gate,0,1,10)); assert(!gate.deadline);
    assert(!desk_startup_due(&gate,1,1,20));
    struct ciuki_input_event shift={.type=CIUKI_INPUT_KEY,.code=CIUKI_KEY_RIGHTSHIFT,.value=1};
    desk_startup_key(&gate,&shift); assert(gate.skip);
    assert(!desk_startup_due(&gate,1,0,1000000020ull));
    assert(desk_startup_due(&gate,1,1,1000000020ull));
    assert(!desk_startup_due(&gate,1,1,2000000020ull));
    gate=(struct desk_startup_gate){.done=1};
    assert(!desk_startup_due(&gate,1,1,2000000020ull)); /* gate/test boot */
    assert(argc==2); char folder[260],registry[260],path[260];
    assert(strlen(argv[1])<160);
    snprintf(folder,sizeof(folder),"%s/StartUp",argv[1]); assert(!mkdir(folder,0700));
    snprintf(registry,sizeof(registry),"%s/Registry",argv[1]); assert(!mkdir(registry,0700));
    struct wr_io io; wr_posix_io(&io,argv[1]); assert(wr_init(&server,&io,0));
    value(key(0,"RunServicesOnce"),"service once","C:\\once.exe /s");
    value(key(0,"RunServices"),"service","C:\\service.exe");
    value(key(0,"RunOnce"),"machine once","C:\\machine.exe");
    value(key(1,"RunOnce"),"user once","C:\\user.exe");
    value(key(0,"Run"),"same","\"C:\\Program Files\\tray.exe\" /tray \"two words\"");
    value(key(1,"Run"),"same","C:\\usertray.exe");
    struct desk_launch_record record={.label="Startup fixture",.target="/Programs/fixture",.arguments="--tray",.show=7};
    uint8_t bytes[DESK_LINK_BYTES]; unsigned n=desk_link_encode(&record,bytes,sizeof(bytes)); assert(n);
    snprintf(path,sizeof(path),"%s/StartUp/tray.lnk",argv[1]); file(path,bytes,n);
    snprintf(path,sizeof(path),"%s/StartUp/native",argv[1]); file(path,"\177ELF",4);
    snprintf(path,sizeof(path),"%s/StartUp/readme.txt",argv[1]); file(path,"Ignore",6);
    struct desk_startup_entry *entries=calloc(DESK_STARTUP_MAX,sizeof(*entries)); assert(entries);
    int count=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(count==8);
    assert(entries[0].source==0 && entries[5].source==5);
    assert(!strcmp(entries[4].launch.arguments,"/tray \"two words\""));
    assert(desk_startup_list_at(folder,registry,entries,1)==-EOVERFLOW);
    count=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(count==8);
    /* Disabled identity is source-specific and survives reopen/snapshot. */
    assert(!desk_startup_enable_at(registry,&entries[4],0));
    count=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(count==8);
    assert(!entries[4].enabled && entries[5].enabled);
    for (int i=0;i<count;i++) if (!strcmp(entries[i].name,"tray.lnk")) {
        assert(entries[i].launch.show==7 && !strcmp(entries[i].launch.arguments,"--tray"));
        assert(!desk_startup_enable_at(registry,&entries[i],0));
        assert(!desk_startup_enable_at(registry,&entries[i],1));
    }
    assert(!desk_startup_execute(entries,(unsigned)count,1,consume,launch)); assert(!launches && !consumed);
    /* Snapshot is unchanged by Shift; once entries still exist. */
    count=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(count==8);
    assert(!desk_startup_execute(entries,(unsigned)count,0,consume,launch)); assert(launches==7 && consumed==3);
    count=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(count==5);
    assert(!desk_startup_execute(entries,(unsigned)count,0,consume,launch)); assert(launches==11 && consumed==3);
    assert(!desk_startup_enable_at(registry,&entries[1],1)); /* HKLM Run */
    /* A changed once command is never consumed through stale UI/worker data. */
    value(key(0,"RunOnce"),"changed","C:\\old.exe");
    count=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(count==6);
    value(key(0,"RunOnce"),"changed","C:\\new.exe");
    assert(ciuki_win32_startup_consume(&entries[1])==-EINVAL);
    assert(find_value(key(0,"RunOnce"),"changed"));
    count=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(count==6);
    fail_launch=1; assert(desk_startup_execute(entries,(unsigned)count,0,consume,launch)==-EIO); fail_launch=0;
    assert(!find_value(key(0,"RunOnce"),"changed"));
    wr_destroy(&server); assert(wr_init(&server,&io,0));
    assert(!find_value(key(0,"RunOnce"),"changed")); /* deletion survived storage reopen */
    /* Malformed entry is listed for disabling, other entries still run. */
    value(key(0,"Run"),"broken","\"unterminated");
    count=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(count==6);
    int before=launches; assert(desk_startup_execute(entries,(unsigned)count,0,consume,launch)==-EINVAL); assert(launches==before+5);
    value(key(0,"Run"),"","C:\\default.exe");
    assert(ciuki_win32_startup_list(entries,DESK_STARTUP_MAX)==5); /* empty value names are legal */
    struct wr_value *expanded=find_value(key(0,"Run"),""); assert(expanded); expanded->type=2;
    value(key(0,"Run"),"expand","%G21_STARTUP_EXEC% /tray");
    expanded=find_value(key(0,"Run"),"expand"); assert(expanded); expanded->type=2;
    assert(!setenv("G21_STARTUP_EXEC","C:\\expanded.exe",1));
    assert(ciuki_win32_startup_list(entries,DESK_STARTUP_MAX)==6);
    int found=0;
    for (unsigned i=0;i<6;i++) if (!strcmp(entries[i].name,"expand")) {
        assert(!entries[i].error && !strcmp(entries[i].launch.target,"C:\\expanded.exe")); found=1;
    }
    assert(found);
    snprintf(path,sizeof(path),"%s/StartUp/broken.lnk",argv[1]); file(path,"bad",3);
    count=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(count==9);
    found=0;
    for (int i=0;i<count;i++) if (!strcmp(entries[i].name,"broken.lnk")) { assert(entries[i].error==-EINVAL); found=1; }
    assert(found);
    unavailable=1; assert(desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX)==-EPIPE); unavailable=0;
    assert(desk_startup_command("\"C:\\space path\\x.exe\" /x",&record)==0);
    assert(desk_startup_command("\"C:\\x.exe\"junk",&record)==-EINVAL);
    assert(desk_startup_command("C:\\x.exe /x",&record)==0);
    assert(desk_startup_command("C:\\x.com",&record)==0);
    assert(desk_startup_command("relative/path",&record)==-EINVAL);
    /* sec-user-05 (Windows XP): the All Users StartUp folder, then the user's own. */
    int common=desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX); assert(common>0);
    char personal[300]; snprintf(personal,sizeof(personal),"%s/Personal",argv[1]); assert(!mkdir(personal,0700));
    snprintf(path,sizeof(path),"%s/mine.lnk",personal); file(path,bytes,n);
    count=desk_startup_list_folders_at(folder,personal,registry,entries,DESK_STARTUP_MAX); assert(count==common+1);
    assert(entries[count-1].source==6 && !strcmp(entries[count-1].name,"mine.lnk") && strstr(entries[count-1].command,"/Personal/mine.lnk"));
    for (int i=0;i<count-1;i++) assert(!strstr(entries[i].command,"/Personal/"));
    assert(desk_startup_list_folders_at(folder,folder,registry,entries,DESK_STARTUP_MAX)==common);   /* one folder once */
    assert(desk_startup_list_folders_at(folder,NULL,registry,entries,DESK_STARTUP_MAX)==common);
    snprintf(path,sizeof(path),"%s/Absent",argv[1]);
    assert(desk_startup_list_folders_at(folder,path,registry,entries,DESK_STARTUP_MAX)==common);    /* no StartUp of its own */
    assert(!setenv("USERPROFILE","/Windows/Profiles/u00000003",1));   /* a session whose StartUp is absent */
    assert(desk_startup_list_at(folder,registry,entries,DESK_STARTUP_MAX)==common); assert(!unsetenv("USERPROFILE"));
    assert(!desk_startup_begin(1));
    free(entries); wr_destroy(&server);
    puts("PASS G21 startup: sources, persistence, Shift, once, malformed, failure, bounds, All Users and personal StartUp"); return 0;
}
