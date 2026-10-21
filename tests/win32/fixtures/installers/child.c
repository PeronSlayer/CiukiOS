/* Authored setup child. A marker is written before ExitProcess, so the
 * parent's wait is checked against a real completed side effect.
 * SPDX-License-Identifier: MIT */
#include <w32/kernel32.h>
extern char *W32API GetCommandLineA(void);
extern DWORD W32API GetModuleFileNameA(HANDLE,char *,DWORD);
extern void W32API ExitProcess(DWORD);
int W32API entry(void){
    char path[260];DWORD n=GetModuleFileNameA(0,path,sizeof(path));
    if(!n || n>=sizeof(path))ExitProcess(2);
    unsigned end=0;for(unsigned i=0;i<n;i++)if(path[i]=='\\' || path[i]=='/')end=i+1;
    const char *name="child.done",*command=GetCommandLineA();
    for(const char *p=command;p && *p;p++){
        const char *a=p,*b="/restart-child";while(*a && *a==*b)a++,b++;
        if(!*b){const char *root="C:\\WINDOWS\\TEMP\\";end=0;while(root[end]){path[end]=root[end];end++;}name="CiukiInstallerFixture.restarted";break;}
    }
    for(unsigned i=0;;i++){if(end+i>=sizeof(path))ExitProcess(206);path[end+i]=name[i];if(!name[i])break;}
    HANDLE h=CreateFileA(path,0x40000000u,0,0,2,128,0);DWORD wrote=0;
    int ok=h!=K32_INVALID && WriteFile(h,"child completed\r\n",17,&wrote,0) && wrote==17 && FlushFileBuffers(h);
    if(h!=K32_INVALID)CloseHandle(h);
    ExitProcess(ok ? 0 : 3);return 3;
}
