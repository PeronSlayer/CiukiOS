/* Two real Linux processes run the production profile parser and lease RPC.
 * The parent runs the production w32srv dispatcher, authenticating each socket.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../../../apps/w32srv/registry.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
/* Native ABI aliases are isolated after Linux declarations in this adapter. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <w32/kernel32.h>
#pragma clang diagnostic pop
#define CHECK(c) do {if(!(c)){fprintf(stderr,"profile processes %u: %s errno=%d\n",__LINE__,#c,errno);exit(1);}}while(0)
static char target[1024];
static struct w32_teb teb;
static struct w32_peb peb;
_Thread_local struct w32_teb *w32_model_teb=&teb;
static DWORD error;
static int endpoint=-1;
DWORD W32API GetLastError(void){return error;}
void W32API SetLastError(DWORD e){error=e;}
void *k32_alloc(DWORD n){return malloc(n);}
void k32_free(void *p,DWORD n){(void)n;free(p);}
void *w32_copy(void *a,const void *b,ULONG n){return memmove(a,b,n);}
void *w32_fill(void *a,uint8_t b,ULONG n){return memset(a,b,n);}
ULONG w32_strlen(const char *s){return s?(ULONG)strlen(s):0;}
int w32_copy_string(char *out,ULONG n,const char *s){if(strlen(s)>=n)return 0;strcpy(out,s);return 1;}
NTSTATUS W32API RtlEnterCriticalSection(RTL_CRITICAL_SECTION *p){(void)p;return 0;}
NTSTATUS W32API RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *p){(void)p;return 0;}
void W32API Sleep(DWORD ms){struct timespec t={0,(long)ms*1000000};nanosleep(&t,0);}
HANDLE W32API CreateFileA(const char *name,DWORD access,DWORD share,SECURITY_ATTRIBUTES *sa,DWORD disposition,DWORD flags,HANDLE template){
  (void)share;(void)sa;(void)flags;(void)template;CHECK(!strcmp(name,"C:\\WINDOWS\\test.ini"));
  int h=open(target,(access&0x40000000u ? O_WRONLY : O_RDONLY)|(disposition==2 ? O_CREAT|O_TRUNC : 0),0600);
  if(h<0){SetLastError(errno==ENOENT ? 2 : 5);return K32_INVALID;}return (HANDLE)(intptr_t)(h+1);
}
BOOL W32API CloseHandle(HANDLE h){return !close((int)(intptr_t)h-1);}
DWORD W32API GetFileSize(HANDLE h,DWORD *high){struct stat st;CHECK(!fstat((int)(intptr_t)h-1,&st));*high=0;return (DWORD)st.st_size;}
BOOL W32API ReadFile(HANDLE h,void *p,DWORD n,DWORD *done,OVERLAPPED *ov){(void)ov;ssize_t r=read((int)(intptr_t)h-1,p,n);*done=r<0 ? 0 : (DWORD)r;return r>=0;}
BOOL W32API WriteFile(HANDLE h,const void *p,DWORD n,DWORD *done,OVERLAPPED *ov){(void)ov;ssize_t r=write((int)(intptr_t)h-1,p,n);*done=r<0 ? 0 : (DWORD)r;return r>=0;}
BOOL W32API FlushFileBuffers(HANDLE h){return !fsync((int)(intptr_t)h-1);}
DWORD w32_server_exchange(const void *input,DWORD n,void *out,DWORD cap,DWORD *used){
  CHECK(endpoint>=0&&write(endpoint,input,n)==(ssize_t)n);ssize_t got=read(endpoint,out,cap);CHECK(got>0);*used=(DWORD)got;return 0;
}
static int absent(void *o,const char *n,uint8_t **p,size_t *s){(void)o;(void)n;(void)p;(void)s;return 0;}
static int save(void *o,const char *n,const void *p,size_t s){(void)o;(void)n;(void)p;(void)s;return 0;}
static int patch(void *o,const char *n,size_t at,const void *p,size_t s){(void)at;return save(o,n,p,s);}
static int sync_file(void *o,const char *n){(void)o;(void)n;return 0;}
int main(int argc,char **argv){
  CHECK(argc==2&&strlen(argv[1])<sizeof(target));strcpy(target,argv[1]);CHECK(!unlink(target)||errno==ENOENT);teb.ProcessEnvironmentBlock=&peb;
  int sockets[2][2];pid_t pid[2];
  for(unsigned c=0;c<2;c++)CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,sockets[c]));
  for(unsigned c=0;c<2;c++){
    pid[c]=fork();CHECK(pid[c]>=0);
    if(!pid[c]){
      endpoint=sockets[c][1];
      for(unsigned j=0;j<2;j++){close(sockets[j][0]);if(j!=c)close(sockets[j][1]);}
      for(unsigned i=0;i<100;i++){
        char key[32],value[32];snprintf(key,sizeof(key),"client%u-key%u",c,i);snprintf(value,sizeof(value),"%u",i+1000*c);
        CHECK(WritePrivateProfileStringA("concurrent",key,value,"test.ini"));
      }
      close(endpoint);_exit(0);
    }
  }
  for(unsigned c=0;c<2;c++)close(sockets[c][1]);
  struct wr_io io={.read=absent,.write=save,.patch=patch,.sync=sync_file};struct wr_server server;
  CHECK(wr_init(&server,&io,0));
  struct pollfd fds[2]={{sockets[0][0],POLLIN,0},{sockets[1][0],POLLIN,0}};unsigned live=2,grants=0,contention=0;
  while(live){
    CHECK(poll(fds,2,10000)>0);
    for(unsigned c=0;c<2;c++)if(fds[c].revents){
      uint8_t in[128],out[128];ssize_t n=read(fds[c].fd,in,sizeof(in));
      if(!n){CHECK(!wr_exit(&server,(uint32_t)pid[c]));close(fds[c].fd);fds[c].fd=-1;live--;continue;}
      CHECK(n>0);size_t used=0;CHECK(!wr_dispatch(&server,(uint32_t)pid[c],in,(size_t)n,out,sizeof(out),&used));
      struct w32r_reply *r=(void *)out;struct w32r_request *a=(void *)in;
      if(a->op==W32R_PROFILE_ACQUIRE){grants+=!r->error;contention+=r->error==170;}
      CHECK(write(fds[c].fd,out,used)==(ssize_t)used);
    }
  }
  for(unsigned c=0;c<2;c++){int status;CHECK(waitpid(pid[c],&status,0)==pid[c]&&WIFEXITED(status)&&!WEXITSTATUS(status));}
  CHECK(!server.profile_owner&&grants==200);wr_destroy(&server);
  for(unsigned c=0;c<2;c++)for(unsigned i=0;i<100;i++){
    char key[32],value[32],expected[32];snprintf(key,sizeof(key),"client%u-key%u",c,i);snprintf(expected,sizeof(expected),"%u",i+1000*c);
    CHECK(GetPrivateProfileStringA("concurrent",key,"missing",value,sizeof(value),"test.ini")==strlen(expected)&&!strcmp(value,expected));
  }
  CHECK(!unlink(target));printf("PASS profile processes writers=2 updates=200 contention=%u no lost updates\n",contention);return 0;
}
