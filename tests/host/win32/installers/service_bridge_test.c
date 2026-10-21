/* Native production bridge/restart checks; no PE execution claim.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <w32/server.h>
extern void *installer_service_create(const char *);
extern uint32_t installer_service_destroy(void *);
extern uint32_t installer_service_exchange(void *,uint32_t,const void *,uint32_t,void *,uint32_t,uint32_t *);
extern unsigned installer_service_observe(void *,unsigned);
extern uint32_t installer_service_restart_begin(void *);
extern uint32_t installer_service_restart_phase(void *);
extern int installer_service_restart_tick(void *);
extern uint32_t installer_service_restart_next(void *,char *,uint32_t);
extern uint32_t installer_service_restart_end(void *);
static struct w32r_reply rpc(void *s,unsigned op,unsigned key,const char *path,const char *name,const void *data,unsigned n){
    uint8_t wire[4096],out[4096];unsigned p=(unsigned)strlen(path)+1,m=(unsigned)strlen(name)+1;
    struct w32r_request r={.version=1,.op=op,.key=key,.access=0x2001f,.type=op==W32R_SET?1u:0u,.path_bytes=p,.name_bytes=m,.data_bytes=n};
    memcpy(wire,&r,sizeof(r));memcpy(wire+sizeof(r),path,p);memcpy(wire+sizeof(r)+p,name,m);if(n)memcpy(wire+sizeof(r)+p+m,data,n);
    uint32_t used=0;assert(!installer_service_exchange(s,99,wire,sizeof(r)+p+m+n,out,sizeof(out),&used));
    struct w32r_reply reply;assert(used>=sizeof(reply));memcpy(&reply,out,sizeof(reply));assert(!reply.error);return reply;
}
int main(int argc,char **argv){
    assert(argc==2);char path[2048];const char *dirs[]={"C","C/WINDOWS","C/WINDOWS/TEMP","C/WINDOWS/Start Menu","C/WINDOWS/Start Menu/Programs","hives"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++){snprintf(path,sizeof(path),"%s/%s",argv[1],dirs[i]);assert(!mkdir(path,0700));}
    void *s=installer_service_create(argv[1]);assert(s);assert(!installer_service_observe(s,1));
    struct w32r_reply key=rpc(s,W32R_CREATE,W32R_HKLM,"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce","",0,0);
    const uint16_t command[]={'c','h','i','l','d','.','e','x','e',0};
    rpc(s,W32R_SET,key.key,"","NativeRestart",command,sizeof(command));rpc(s,W32R_CLOSE,key.key,"","",0,0);
    snprintf(path,sizeof(path),"%s/C/WINDOWS/TEMP/victim.exe",argv[1]);FILE *f=fopen(path,"wb");assert(f);assert(fputs("authored",f)>=0);assert(!fclose(f));
    snprintf(path,sizeof(path),"%s/C/WINDOWS/WININIT.INI",argv[1]);f=fopen(path,"wb");assert(f);assert(fputs("[rename]\nNUL=C:\\WINDOWS\\TEMP\\victim.exe\n",f)>=0);assert(!fclose(f));
    assert(!installer_service_restart_begin(s));assert(installer_service_restart_end(s)==170);
    snprintf(path,sizeof(path),"%s/C/WINDOWS/CIUKIRST.LOG",argv[1]);assert(!access(path,F_OK));
    for(unsigned i=0;i<600 && installer_service_restart_phase(s)==1;i++)installer_service_restart_tick(s);
    assert(installer_service_restart_phase(s)==2);
    snprintf(path,sizeof(path),"%s/C/WINDOWS/TEMP/victim.exe",argv[1]);assert(access(path,F_OK));
    snprintf(path,sizeof(path),"%s/C/WINDOWS/WININIT.BAK",argv[1]);assert(!access(path,F_OK));
    char text[2048];assert(!installer_service_restart_next(s,text,sizeof(text)));assert(!strcmp(text,"child.exe"));
    assert(installer_service_restart_next(s,text,sizeof(text))==259);assert(!installer_service_restart_end(s));
    snprintf(path,sizeof(path),"%s/C/WINDOWS/CIUKIRST.LOG",argv[1]);assert(access(path,F_OK));
    assert(!installer_service_destroy(s));puts("PASS production bridge registry lifecycle, WININIT deletion, backup, journal, RunOnce handoff");return 0;
}
