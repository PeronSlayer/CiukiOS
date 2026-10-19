/* Production shell adapters against the real registry dispatcher behind a
 * fake authenticated endpoint. SPDX-License-Identifier: MIT */
#include "win32_adapter.h"
#include "../../../apps/w32srv/registry.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct wr_server server;
static unsigned calls, launches, kind, malformed, unavailable;
static struct desk_launch_record launched;
uint32_t desk_w32_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{
    calls++;
    if(unavailable)return 1062;
    size_t size=0;
    uint32_t e=wr_dispatch(&server,42,in,n,out,cap,&size);*used=(uint32_t)size;
    if(malformed)((struct w32r_reply *)out)->data_bytes=W32R_DATA+1;
    return e;
}
int desk_w32_launch(enum desk_w32_kind k,const struct desk_launch_record *r)
{kind=k;launched=*r;launches++;return 123;}
static void value(struct wr_key *key,const char *name,const char *text)
{
    uint8_t data[4096];size_t n=strlen(text);assert(n<sizeof(data)/2);
    for(size_t i=0;i<=n;i++){data[i*2]=(uint8_t)text[i];data[i*2+1]=0;}
    assert(!wr_put(&server,key,name,1,data,(uint32_t)(n+1)*2));
}
static struct wr_key *root;
static struct wr_key *fixture(const char *name,const char *display,const char *command)
{
    struct wr_key *key=wr_add_key(&server,root,name,0);assert(key);
    value(key,"DisplayName",display);value(key,"UninstallString",command);
    value(key,"DisplayVersion","1.2");value(key,"Publisher","Ciuki fixture");return key;
}
static void file(const char *path,const uint8_t *bytes,size_t count)
{FILE *f=fopen(path,"wb");assert(f);assert(fwrite(bytes,1,count,f)==count);assert(!fclose(f));}
static void registry(const char *dir)
{
    struct wr_io io;wr_posix_io(&io,dir);assert(wr_init(&server,&io,0));
    struct w32r_request a={.version=1,.op=W32R_CREATE,.key=W32R_HKLM,.access=0xf003f};
    const char path[]="Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    uint8_t in[512],out[512];a.path_bytes=sizeof(path);a.name_bytes=1;
    memcpy(in,&a,sizeof(a));memcpy(in+sizeof(a),path,sizeof(path));in[sizeof(a)+sizeof(path)]=0;
    size_t n;assert(!wr_dispatch(&server,42,in,sizeof(a)+sizeof(path)+1,out,sizeof(out),&n));
    assert(!((struct w32r_reply *)out)->error);
    for(root=server.keys;root;root=root->next)if(!strcmp(root->name,path))break;
    /* Keys store full paths, inspect by the public child traversal. */
    if(!root){root=server.roots[0];assert(root);}
    struct wr_key *walk=wr_child(&server,server.roots[0],"Software");
    if(walk){const char *parts[]={"Microsoft","Windows","CurrentVersion","Uninstall"};
        for(unsigned i=0;i<4;i++)walk=wr_child(&server,walk,parts[i]);root=walk;}
    assert(root);
    char fixture_path[260],command[512];
    snprintf(fixture_path,sizeof(fixture_path),"%s/fixture.exe",dir);
    uint8_t pe[68]={0};pe[0]='M';pe[1]='Z';pe[60]=64;memcpy(pe+64,"PE\0\0",4);file(fixture_path,pe,sizeof(pe));
    snprintf(command,sizeof(command),"\"%s\" /remove \"keep spaces\"",fixture_path);
    struct wr_key *fixture_key=fixture("fixture","Fixture",command);
    fixture("missing-name","","C:\\fixture.exe /x");
    struct desk_uninstall_entry list[33];memset(list,0xa5,sizeof(list));
    int count=ciuki_win32_uninstall_list(list,32);assert(count==1);
    assert(!strcmp(list[0].name,"Fixture") && !strcmp(list[0].version,"1.2") && !strcmp(list[0].publisher,"Ciuki fixture"));
    assert((unsigned char)list[32].key[0]==0xa5);
    for(unsigned i=0;i<40;i++){char name[32];snprintf(name,sizeof(name),"program-%u",i);fixture(name,name,"C:\\fixture.exe /x");}
    assert(ciuki_win32_uninstall_list(list,33)==32);
    assert((unsigned char)list[32].key[0]==0xa5);
    assert(ciuki_win32_uninstall_run("..\\fixture")==-EINVAL);
    assert(ciuki_win32_uninstall_run("fixture")==123);
    assert(kind==DESK_W32_PE && !strcmp(launched.target,fixture_path) && !strcmp(launched.arguments,"/remove \"keep spaces\""));
    value(fixture_key,"UninstallString","\"C:\\literal.exe\" %SystemRoot%");
    count=ciuki_win32_uninstall_list(list,32);assert(count==32);
    int literal=0;for(int i=0;i<count;i++)if(!strcmp(list[i].key,"fixture")) {
        assert(!strcmp(list[i].command,"\"C:\\literal.exe\" %SystemRoot%"));literal=1;
    }assert(literal); /* REG_SZ never expands. */
    const uint8_t surrogate[]={0,0xd8,0,0};
    assert(!wr_put(&server,fixture_key,"UninstallString",2,surrogate,sizeof(surrogate)));
    assert(ciuki_win32_uninstall_run("fixture")==-EINVAL);
    assert(!setenv("CIUKI_UNINSTALL_DIR",dir,1));
    const char *expanded="\"%ciuki_uninstall_dir%/fixture.exe\" /remove \"keep spaces\"";
    uint8_t wide[512];size_t wn=strlen(expanded);
    for(size_t i=0;i<=wn;i++){wide[2*i]=(uint8_t)expanded[i];wide[2*i+1]=0;}
    assert(!wr_put(&server,fixture_key,"UninstallString",2,wide,(uint32_t)(wn+1)*2));
    assert(ciuki_win32_uninstall_run("fixture")==123);
    assert(!strcmp(launched.target,fixture_path) && !strcmp(launched.arguments,"/remove \"keep spaces\""));
    expanded="\"%SystemRoot%\\remove.exe\" %UNKNOWN_CIUKI_VARIABLE% tail%";wn=strlen(expanded);
    for(size_t i=0;i<=wn;i++){wide[2*i]=(uint8_t)expanded[i];wide[2*i+1]=0;}
    assert(!wr_put(&server,fixture_key,"UninstallString",2,wide,(uint32_t)(wn+1)*2));
    count=ciuki_win32_uninstall_list(list,32);assert(count==32);
    int found=0;for(int i=0;i<count;i++)if(!strcmp(list[i].key,"fixture")) {
        assert(!strcmp(list[i].command,"\"C:\\WINDOWS\\remove.exe\" %UNKNOWN_CIUKI_VARIABLE% tail%"));found=1;
    }assert(found);
    const char *patterns[]={"%UNKNOWN_CIUKI_VARIABLE%SystemRoot%","%%SystemRoot%%","%UNKNOWN_CIUKI_VARIABLE%%SystemRoot%"};
    const char *results[]={"%UNKNOWN_CIUKI_VARIABLE%SystemRoot%","%%SystemRoot%%","%UNKNOWN_CIUKI_VARIABLE%C:\\WINDOWS"};
    for(unsigned k=0;k<3;k++) {
        char expected[512];snprintf(command,sizeof(command),"\"C:\\fixture.exe\" %s",patterns[k]);wn=strlen(command);
        snprintf(expected,sizeof(expected),"\"C:\\fixture.exe\" %s",results[k]);
        for(size_t i=0;i<=wn;i++){wide[2*i]=(uint8_t)command[i];wide[2*i+1]=0;}
        assert(!wr_put(&server,fixture_key,"UninstallString",2,wide,(uint32_t)(wn+1)*2));
        count=ciuki_win32_uninstall_list(list,32);assert(count==32);found=0;
        for(int i=0;i<count;i++)if(!strcmp(list[i].key,"fixture")) {assert(!strcmp(list[i].command,expected));found=1;}
        assert(found);
    }
    char overflow[2048];memset(overflow,'x',sizeof(overflow)-1);overflow[sizeof(overflow)-1]=0;
    assert(!setenv("CIUKI_UNINSTALL_DIR",overflow,1));
    expanded="%CIUKI_UNINSTALL_DIR%";wn=strlen(expanded);
    for(size_t i=0;i<=wn;i++){wide[2*i]=(uint8_t)expanded[i];wide[2*i+1]=0;}
    assert(!wr_put(&server,fixture_key,"UninstallString",2,wide,(uint32_t)(wn+1)*2));
    assert(ciuki_win32_uninstall_run("fixture")==-EOVERFLOW);
    value(fixture_key,"UninstallString","C:\\Program Files\\unsafe.exe /x");
    assert(ciuki_win32_uninstall_run("fixture")==-EINVAL); /* reread, reject ambiguous path */
    malformed=1;assert(ciuki_win32_uninstall_list(list,32)==-EINVAL);malformed=0;
    unavailable=1;assert(ciuki_win32_uninstall_list(list,32)==-EPIPE);assert(desk_w32_flush()==-EPIPE);unavailable=0;
    assert(!desk_w32_flush());wr_destroy(&server);
    puts("PASS uninstall RPC: four fields, 32 record bound, malformed/unavailable endpoint, path validation, flush");
}
static unsigned spawned,records,fail_spawn;
static int spawn(void *ctx){assert(ctx==&spawned);spawned++;return fail_spawn ? -EIO : (int)(100+spawned);}
static void record(void *ctx,const char *text,int pid,uint64_t ns)
{assert(ctx==&spawned);assert(strstr(text,"[win32] w32srv"));if(pid>0)assert(ns<=2000000000ull);records++;}
static void supervision(void)
{
    struct desk_w32_supervisor s={0};struct desk_w32_supervisor_ops o={spawn,record,&spawned};
    desk_w32_supervise(&s,&o,0);assert(s.pid==101 && s.starts==1);
    assert(!desk_w32_service_exited(&s,&o,999,9,10));
    assert(desk_w32_service_exited(&s,&o,101,11,10));
    desk_w32_supervise(&s,&o,1000010);assert(s.pid==102 && records==1);
    assert(desk_w32_service_exited(&s,&o,102,9,1000020));
    fail_spawn=1;desk_w32_supervise(&s,&o,1000020);assert(!s.pid);
    unsigned before=spawned;desk_w32_supervise(&s,&o,1000021);assert(spawned==before);
    fail_spawn=0;desk_w32_supervise(&s,&o,101000020);assert(s.pid && s.starts==3);
    s.stopping=1;s.pid=0;before=spawned;desk_w32_supervise(&s,&o,2000000000);assert(spawned==before);
    puts("PASS supervision: crash/SIGKILL records, 1 ms replay recovery, bounded retry, shutdown suppression");
}
int main(int argc,char **argv)
{
    assert(argc==2);registry(argv[1]);supervision();
    uint8_t pe[68]={0};pe[0]='M';pe[1]='Z';pe[60]=64;memcpy(pe+64,"PE\0\0",4);
    char path[512];snprintf(path,sizeof(path),"%s/fixture.exe",argv[1]);file(path,pe,sizeof(pe));
    struct desk_launch_record r={.label="Fixture",.show=1};snprintf(r.target,sizeof(r.target),"%s",path);
    strcpy(r.arguments,"/remove \"keep spaces\"");assert(ciuki_win32_shell_launch(&r)==123 && kind==DESK_W32_PE);
    assert(!strcmp(launched.arguments,r.arguments));
    file(path,(const uint8_t *)"\177ELF",4);assert(ciuki_win32_shell_launch(&r)==123 && kind==DESK_W32_ELF);
    file(path,(const uint8_t *)"MZ",2);assert(ciuki_win32_shell_launch(&r)==123 && kind==DESK_W32_DOS);
    r.target[0]=0;assert(ciuki_win32_shell_launch(&r)==-EINVAL);
    char native[260];assert(desk_w32_native_path("",native,sizeof(native))==-EINVAL);assert(!desk_w32_native_path("C:\\Program Files\\a.exe",native,sizeof(native)));
    assert(!strcmp(native,"/Program Files/a.exe"));assert(desk_w32_native_path("D:\\a.exe",native,sizeof(native))==-ENODEV);
    assert(launches==5 && calls>0);puts("PASS shell launch: PE, ELF and DOS content classification; exact bounded record and path translation");
    return 0;
}
