/* Linux adapter for Win32 calls: real files and production registry service.
 * The shim is not guest or original-installer evidence.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../../../../apps/w32srv/registry.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include "host.h"
#include <ciuki/w32_diagnostics.h>
#include "../../../../sdk/win32/services/files.h"
#pragma clang diagnostic pop
char host_root[1024];
unsigned host_cancel,host_full,host_refusals,host_spawned,host_closed;
static DWORD error;
static struct wr_server registry;
static unsigned registry_live;
static char hive_path[1200];
static char hive_directory[1400];
static int path(const char *w,char *p){
  if(!w || strlen(w)>=260 || w[1]!=':' || (w[2]!='\\' && w[2]!='/')){error=123;return 0;}
  int n=snprintf(p,1400,"%s/%c",host_root,w[0]);assert(n>0 && n<1100);
  for(unsigned i=2;w[i];i++)p[n++]=w[i]=='\\'?'/':w[i];p[n]=0;return 1;
}
static BOOL fail(void){error=errno==ENOENT?2:errno==EEXIST?183:errno==ENOSPC?112:errno==ENOTEMPTY?145:5;return 0;}
DWORD W32API GetLastError(void){return error;}
void W32API SetLastError(DWORD e){error=e;}
HANDLE W32API GetProcessHeap(void){return (HANDLE)1;}
void *W32API HeapAlloc(HANDLE h,DWORD flags,DWORD n){(void)h;return flags&8?calloc(1,n?n:1):malloc(n?n:1);}
BOOL W32API HeapFree(HANDLE h,DWORD flags,void *p){(void)h;(void)flags;free(p);return 1;}
void *W32API HeapReAlloc(HANDLE h,DWORD flags,void *p,DWORD n){(void)h;(void)flags;return realloc(p,n);}
DWORD W32API HeapSize(HANDLE h,DWORD flags,const void *p){(void)h;(void)flags;(void)p;return ~0u;}
BOOL W32API CreateDirectoryA(const char *w,SECURITY_ATTRIBUTES *sa){(void)sa;char p[1400];return path(w,p) && (!mkdir(p,0700) || fail());}
BOOL W32API RemoveDirectoryA(const char *w){char p[1400];return path(w,p) && (!rmdir(p) || fail());}
BOOL W32API DeleteFileA(const char *w){char p[1400];return path(w,p) && (!unlink(p) || fail());}
BOOL W32API MoveFileA(const char *a,const char *b){char p[1400],q[1400];return path(a,p) && path(b,q) && (!rename(p,q) || fail());}
DWORD W32API GetFileAttributesA(const char *w){char p[1400];struct stat st;if(!path(w,p) || stat(p,&st)){fail();return ~0u;}return S_ISDIR(st.st_mode)?16:128;}
DWORD W32API GetFullPathNameA(const char *w,DWORD cap,char *p,char **part){
  char source[260],out[260];unsigned ends[130],depth=0,n=3;
  if(strlen(w)>=sizeof(source) || w[1]!=':'){error=123;return 0;}
  strcpy(source,w);for(char *q=source;*q;q++)if(*q=='/')*q='\\';
  out[0]=w[0];out[1]=':';out[2]='\\';out[3]=0;
  char *state=0;
  for(char *word=strtok_r(source+2,"\\",&state);word;word=strtok_r(0,"\\",&state)){
    if(!strcmp(word,"."))continue;
    if(!strcmp(word,"..")){if(depth)n=ends[--depth];out[n]=0;continue;}
    unsigned start=n;if(n>3)out[n++]='\\';size_t count=strlen(word);assert(count<sizeof(out)-n);
    memcpy(out+n,word,count+1);n+=(unsigned)count;ends[depth++]=start;
  }
  if(n>=cap)return n+1;memcpy(p,out,n+1);if(part)*part=0;return n;
}
HANDLE W32API CreateFileA(const char *w,DWORD access,DWORD share,SECURITY_ATTRIBUTES *sa,DWORD disposition,DWORD flags,HANDLE template){
  (void)share;(void)sa;(void)flags;(void)template;char p[1400];
  if(!path(w,p))return K32_INVALID;
  if(w[0]=='D' && (access&0x40000000u)){error=19;return K32_INVALID;}
  int of=access&0x40000000u?O_WRONLY:O_RDONLY;
  if(disposition==1)of|=O_CREAT|O_EXCL;else if(disposition==2)of|=O_CREAT|O_TRUNC;
  int fd=open(p,of,0600);if(fd<0){fail();if(error==183)error=80;return K32_INVALID;}
  return (HANDLE)(intptr_t)(fd+16);
}
BOOL W32API CloseHandle(HANDLE h){
  if((uintptr_t)h>=0x100000u){host_closed++;return 1;}
  return !close((int)(intptr_t)h-16);
}
BOOL W32API ReadFile(HANDLE h,void *p,DWORD n,DWORD *done,OVERLAPPED *ov){(void)ov;ssize_t r=read((int)(intptr_t)h-16,p,n);*done=r<0?0:(DWORD)r;return r>=0?1:fail();}
BOOL W32API WriteFile(HANDLE h,const void *p,DWORD n,DWORD *done,OVERLAPPED *ov){(void)ov;if(host_full){*done=0;error=112;return 0;}ssize_t r=write((int)(intptr_t)h-16,p,n);*done=r<0?0:(DWORD)r;return r>=0?1:fail();}
BOOL W32API FlushFileBuffers(HANDLE h){return !fsync((int)(intptr_t)h-16);}
DWORD W32API GetFileSize(HANDLE h,DWORD *high){struct stat st;assert(!fstat((int)(intptr_t)h-16,&st));if(high)*high=0;return (DWORD)st.st_size;}
int w32v_open(const void *name,int wide,struct w32v_file *f){char a[260];if(wide){if(!shell_ansi(name,a,260))return 0;name=a;}HANDLE h=CreateFileA(name,0x80000000u,7,0,3,0,0);if(h==K32_INVALID)return 0;f->handle=(uintptr_t)h;f->size=GetFileSize(h,0);return 1;}
int w32v_read(struct w32v_file *f,uint32_t off,void *out,uint32_t n){if(off>f->size || n>f->size-off)return 0;if(lseek((int)f->handle-16,off,SEEK_SET)!=(off_t)off)return 0;DWORD done;return ReadFile((HANDLE)f->handle,out,n,&done,0) && done==n;}
void w32v_close(struct w32v_file *f){CloseHandle((HANDLE)f->handle);}
DWORD W32API GetModuleFileNameA(HANDLE h,char *out,DWORD cap){(void)h;const char *s="C:\\Program Files\\Ciuki Fixture\\setup.exe";return shell_copy(out,cap,s)?(DWORD)strlen(s):cap;}
int w32_builtin_version(const char *path,uint16_t out[4]){const char *base=path;for(const char *p=path;*p;p++)if(*p=='/' || *p=='\\')base=p+1;if(w32_stricmp(base,"gdi32.dll"))return 0;out[0]=4;out[1]=10;out[2]=2222;out[3]=0;return 1;}
BOOL W32API CopyFileA(const char *a,const char *b,BOOL exclusive){
  HANDLE in=CreateFileA(a,0x80000000u,1,0,3,0,0);if(in==K32_INVALID)return 0;
  HANDLE out=CreateFileA(b,0x40000000u,0,0,exclusive?1:2,0,0);if(out==K32_INVALID){CloseHandle(in);return 0;}
  char bytes[4096];DWORD got,wrote;BOOL ok=1;
  do{ok=ReadFile(in,bytes,sizeof(bytes),&got,0);if(ok && got)ok=WriteFile(out,bytes,got,&wrote,0) && wrote==got;}while(ok && got);
  DWORD e=error;CloseHandle(in);CloseHandle(out);if(!ok)DeleteFileA(b);error=e;return ok;
}
struct find {DIR *dir;char path[1400];};
static BOOL next(struct find *f,WIN32_FIND_DATAA *d){
  struct dirent *e;
  while((e=readdir(f->dir))){if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
    memset(d,0,sizeof(*d));assert(strlen(e->d_name)<sizeof(d->name));strcpy(d->name,e->d_name);
    char p[1700];snprintf(p,sizeof(p),"%s/%s",f->path,e->d_name);struct stat st;assert(!stat(p,&st));d->attributes=S_ISDIR(st.st_mode)?16:128;return 1;}
  error=18;return 0;
}
HANDLE W32API FindFirstFileA(const char *w,WIN32_FIND_DATAA *d){
  char p[1400];if(!path(w,p))return K32_INVALID;char *last=strrchr(p,'/');if(!last || strcmp(last+1,"*")){error=2;return K32_INVALID;}*last=0;
  struct find *f=calloc(1,sizeof(*f));assert(f);strcpy(f->path,p);f->dir=opendir(p);
  if(!f->dir || !next(f,d)){if(f->dir)closedir(f->dir);free(f);return K32_INVALID;}return f;
}
BOOL W32API FindNextFileA(HANDLE h,WIN32_FIND_DATAA *d){return next(h,d);}
BOOL W32API FindClose(HANDLE h){struct find *f=h;closedir(f->dir);free(f);return 1;}
int W32API MessageBoxA(HANDLE w,const char *text,const char *title,DWORD flags){(void)w;(void)text;(void)title;(void)flags;return host_cancel?2:1;}
/* UI pixels/controls are independently exercised by f5-10C. This adapter
 * explicitly runs file operations with FOF_SILENT. */
struct shell_progress *shell_progress_begin(const SHELL_FILEOP *a){assert(a->flags&4);return 0;}
int shell_progress_tick(struct shell_progress *p,const char *file){(void)p;(void)file;return 1;}
void shell_progress_end(struct shell_progress *p){assert(!p);}
BOOL W32API CreateProcessA(const char *exe,char *command,SECURITY_ATTRIBUTES *ps,SECURITY_ATTRIBUTES *ts,BOOL inherit,DWORD flags,void *env,const char *cwd,struct shell_startup *s,struct shell_process *p){
  (void)ps;(void)ts;(void)inherit;(void)flags;(void)env;
  assert(s->size==sizeof(*s));assert(exe && command && !strstr(command,"/bin/sh"));
  FILE *f=fopen(hive_path,"a");assert(f);fprintf(f,"spawn exe=%s command=%s cwd=%s\n",exe,command,cwd?cwd:"");fclose(f);
  host_spawned++;*p=(struct shell_process){(HANDLE)(uintptr_t)(0x100000+host_spawned*2),(HANDLE)(uintptr_t)(0x100001+host_spawned*2),100,101};return 1;
}
void w32_diag(const char *a,const char *b,const char *c){(void)a;(void)b;(void)c;}
HANDLE W32API GetModuleHandleA(const char *name){(void)name;return 0;}
void *W32API GetProcAddress(HANDLE h,const char *name){(void)h;(void)name;return 0;}
void w32_report_refusal(uint32_t why,const char *name,uint32_t code){(void)name;assert(why==W32_REFUSE_MSI && code==120);host_refusals++;}
void *w32r_alloc(uint32_t n){return malloc(n?n:1);}
void w32r_free(void *p){free(p);}
void w32r_pause(void){}
void w32_registry_forget(uint32_t key){(void)key;}
void w32r_last_error(uint32_t e){error=e;}
uint32_t w32r_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used){size_t size=0;uint32_t e=wr_dispatch(&registry,77,in,n,out,cap,&size);*used=(uint32_t)size;return e;}
extern LONG RegCreateKeyA(DWORD,const char *,DWORD *);
extern LONG RegSetValueExA(DWORD,const char *,DWORD,DWORD,const void *,DWORD);
extern LONG RegCloseKey(DWORD);
void host_registry(const char *key,const char *name,const char *value){DWORD h;assert(!RegCreateKeyA(W32R_HKLM,key,&h));assert(!RegSetValueExA(h,name,0,1,value,(DWORD)strlen(value)+1));assert(!RegCloseKey(h));}
void host_init(const char *root){
  assert(strlen(root)<sizeof(host_root));strcpy(host_root,root);char p[1400];snprintf(p,sizeof(p),"%s/C",root);assert(!mkdir(p,0700));snprintf(p,sizeof(p),"%s/D",root);assert(!mkdir(p,0700));
  snprintf(hive_directory,sizeof(hive_directory),"%s/hives",root);assert(!mkdir(hive_directory,0700));static struct wr_io io;wr_posix_io(&io,hive_directory);assert(wr_init(&registry,&io,0));registry_live=1;
  snprintf(hive_path,sizeof(hive_path),"%s/spawn.txt",root);
}
void host_finish(void){if(registry_live){assert(!wr_flush(&registry));wr_destroy(&registry);registry_live=0;}}
void host_put(const char *w,const char *s){char p[1400];assert(path(w,p));FILE *f=fopen(p,"wb");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(!fclose(f));}
