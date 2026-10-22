/* Authored self-extractor pattern: cabinet -> unique GetTempPath tree ->
 * waitable child -> cleanup. No host shell or fake child completion.
 * SPDX-License-Identifier: MIT */
#include "fdi.h"
extern BOOL W32API GetExitCodeProcess(HANDLE,DWORD *);
extern BOOL W32API TerminateProcess(HANDLE,DWORD);
extern void W32API ExitProcess(DWORD);
int W32API entry(void){
    char temporary[260],tree[260],cabinet_path[260],child[260],command[264],marker[260];
    int reserved=0,created=0;tree[0]=0;
    DWORD n=GetModuleFileNameA(0,cabinet_path,260);int ok=n && n<260;
    if(ok){unsigned last=0;for(unsigned i=0;i<n;i++)if(cabinet_path[i]=='\\' || cabinet_path[i]=='/')last=i;
        cabinet_path[last]=0;char folder[260];for(unsigned i=0;i<=last;i++)folder[i]=cabinet_path[i];ok=join(cabinet_path,folder,"fixture.cab");}
    /* GetTempFileName reserves a name: remove its file before mkdir. */
    if(ok)ok=GetTempPathA(260,temporary) && (reserved=GetTempFileNameA(temporary,"CIF",0,tree)!=0);
    if(ok){ok=DeleteFileA(tree);if(ok)reserved=0;}
    if(ok)ok=created=CreateDirectoryA(tree,0)!=0;
    if(ok)ok=extract(tree,cabinet_path) && join(child,tree,"child.exe") && join(marker,tree,"child.done");
    if(ok){
        unsigned i=0;command[i++]='"';for(unsigned j=0;child[j];j++)command[i++]=child[j];command[i++]='"';command[i]=0;
        struct shell_startup startup={.size=sizeof(startup)};struct shell_process process={0};
        ok=CreateProcessA(child,command,0,0,0,0,0,tree,&startup,&process);
        if(ok){DWORD code=~0u,wait=WaitForSingleObject(process.process,30000);
            if(wait!=0){TerminateProcess(process.process,1);WaitForSingleObject(process.process,~0u);}
            ok=wait==0 && GetExitCodeProcess(process.process,&code) && !code && GetFileAttributesA(marker)!=~0u;
            CloseHandle(process.thread);CloseHandle(process.process);}
    }
    /* After releasing the reserved file, a failed mkdir conveys no ownership
     * of a directory another actor may have created at the same name. */
    if(created){const char *names[]={"child.done","child.exe","payload.txt","chain.inf"};char file[260];
        for(unsigned i=0;i<4;i++)if(join(file,tree,names[i]) && GetFileAttributesA(file)!=~0u && !DeleteFileA(file))ok=0;
        if(!RemoveDirectoryA(tree))ok=0;
    }else if(reserved && !DeleteFileA(tree))ok=0;
    ExitProcess(ok ? 0 : 1);return 1;
}
