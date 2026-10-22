/* Authored FDI ABI/callback adapter; the decoder is the reused cabinet.dll.
 * SPDX-License-Identifier: MIT */
#include <w32/shell.h>
extern DWORD W32API GetModuleFileNameA(HANDLE,char *,DWORD);
extern DWORD W32API GetTempPathA(DWORD,char *);
extern DWORD W32API GetTempFileNameA(const char *,const char *,DWORD,char *);
extern DWORD W32API SetFilePointer(HANDLE,LONG,LONG *,DWORD);
struct fdi_error { int operation,type,error; };
struct fdi_notice { LONG size; char *name,*disk,*path; void *context; intptr_t file;
    uint16_t date,time,attributes,set,cabinet,folder; int error; };
static char extraction[260];
static unsigned extracted;
static HANDLE extraction_handles[3];
static void *fd_alloc(ULONG n){return HeapAlloc(GetProcessHeap(),0,n);}
static void fd_free(void *p){HeapFree(GetProcessHeap(),0,p);}
static intptr_t fd_open(char *name,int flags,int mode){
    (void)mode;
    return (intptr_t)CreateFileA(name,flags&3 ? 0xc0000000u : 0x80000000u,3,0,flags&0x100 ? 2 : 3,128,0);
}
static unsigned fd_read(intptr_t h,void *p,unsigned n){DWORD done=0;return ReadFile((HANDLE)h,p,n,&done,0)?done:~0u;}
static unsigned fd_write(intptr_t h,void *p,unsigned n){DWORD done=0;return WriteFile((HANDLE)h,p,n,&done,0)?done:~0u;}
static int fd_close(intptr_t h){return CloseHandle((HANDLE)h)?0:-1;}
static LONG fd_seek(intptr_t h,LONG n,int how){return (LONG)SetFilePointer((HANDLE)h,n,0,(DWORD)how);}
static int join(char out[260],const char *a,const char *b){
    unsigned i=0;for(;a[i] && i<259;i++)out[i]=a[i];
    if(a[i])return 0;
    if(i && out[i-1]!='\\'){if(i>=259)return 0;out[i++]='\\';}
    for(unsigned j=0;b[j];j++){if(i>=259)return 0;out[i++]=b[j];}out[i]=0;return 1;
}
static int equal(const char *a,const char *b){while(*a && *a==*b)a++,b++;return *a==*b;}
static intptr_t fd_notify(int kind,struct fdi_notice *n){
    if(kind==2){
        /* This fixture has exactly three flat members. Reject traversal,
         * alternate streams, directories, unexpected names and splits. */
        if(!equal(n->name,"payload.txt") && !equal(n->name,"child.exe") && !equal(n->name,"chain.inf"))return -1;
        char name[260];if(!join(name,extraction,n->name))return -1;
        unsigned i=equal(n->name,"payload.txt")?0u:equal(n->name,"child.exe")?1u:2u;
        extraction_handles[i]=CreateFileA(name,0x40000000u,0,0,1,128,0);
        return (intptr_t)extraction_handles[i];
    }
    if(kind==3){if(!CloseHandle((HANDLE)n->file))return 0;
        for(unsigned i=0;i<3;i++)if(extraction_handles[i]==(HANDLE)n->file)extraction_handles[i]=K32_INVALID;
        extracted++;return 1;}
    return kind==1 || kind==4 ? -1 : 0;
}
static int extract(const char *directory,const char *cabinet_path){
    HANDLE module=LoadLibraryExA("cabinet.dll",0,0);if(!module)return 0;
    void *(*create)(void *,void *,void *,void *,void *,void *,void *,int,struct fdi_error *)=
        GetProcAddress(module,"FDICreate");
    int (*copy)(void *,char *,char *,int,void *,void *,void *)=GetProcAddress(module,"FDICopy");
    int (*destroy)(void *)=GetProcAddress(module,"FDIDestroy");
    char folder[260];unsigned at=0,last=0;
    for(;cabinet_path[at] && at<259;at++){folder[at]=cabinet_path[at];if(folder[at]=='\\' || folder[at]=='/')last=at+1;}
    if(cabinet_path[at] || !last || !create || !copy || !destroy){FreeLibrary(module);return 0;}
    folder[last]=0;
    for(at=0;directory[at] && at<259;at++)extraction[at]=directory[at];extraction[at]=0;
    extracted=0;for(unsigned i=0;i<3;i++)extraction_handles[i]=K32_INVALID;struct fdi_error error={0};
    void *context=create(fd_alloc,fd_free,fd_open,fd_read,fd_write,fd_close,fd_seek,-1,&error);
    int ok=context && copy(context,(char *)cabinet_path+last,folder,0,fd_notify,0,0) && extracted==3;
    for(unsigned i=0;i<3;i++)if(extraction_handles[i]!=K32_INVALID){CloseHandle(extraction_handles[i]);ok=0;}
    if(context && !destroy(context))ok=0;
    FreeLibrary(module);return ok;
}
