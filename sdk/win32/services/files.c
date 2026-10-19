/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "files.h"
__declspec(dllimport) uint32_t W32API CreateFileA(const char *,uint32_t,uint32_t,void *,uint32_t,uint32_t,uint32_t);
__declspec(dllimport) uint32_t W32API CreateFileW(const WCHAR *,uint32_t,uint32_t,void *,uint32_t,uint32_t,uint32_t);
__declspec(dllimport) uint32_t W32API GetFileSize(uint32_t,uint32_t *);
__declspec(dllimport) uint32_t W32API SetFilePointer(uint32_t,int32_t,int32_t *,uint32_t);
__declspec(dllimport) int W32API ReadFile(uint32_t,void *,uint32_t,uint32_t *,void *);
__declspec(dllimport) int W32API CloseHandle(uint32_t);
__declspec(dllimport) uint32_t W32API GetProcessHeap(void);
__declspec(dllimport) void *W32API HeapAlloc(uint32_t,uint32_t,uint32_t);
__declspec(dllimport) int W32API HeapFree(uint32_t,uint32_t,void *);
__declspec(dllimport) void W32API SetLastError(uint32_t);
int w32v_open(const void *name,int wide,struct w32v_file *f)
{
    uint32_t h=wide?CreateFileW(name,0x80000000,7,NULL,3,0,0):CreateFileA(name,0x80000000,7,NULL,3,0,0);
    if(h==UINT32_MAX)return 0;uint32_t high=0,size=GetFileSize(h,&high);
    if(high||size==UINT32_MAX){CloseHandle(h);return 0;}f->handle=h;f->size=size;return 1;
}
int w32v_read(struct w32v_file *f,uint32_t offset,void *out,uint32_t n)
{
    if(offset>f->size||n>f->size-offset||SetFilePointer((uint32_t)f->handle,(int32_t)offset,NULL,0)!=offset)return 0;
    uint32_t got=0;return ReadFile((uint32_t)f->handle,out,n,&got,NULL)&&got==n;
}
void w32v_close(struct w32v_file *f){CloseHandle((uint32_t)f->handle);}
void *w32lz_alloc(uint32_t n){return HeapAlloc(GetProcessHeap(),0,n);}
void w32lz_free(void *p){if(p)HeapFree(GetProcessHeap(),0,p);}
void w32r_last_error(uint32_t e){SetLastError(e);}
