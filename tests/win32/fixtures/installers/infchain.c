/* Execute the reused Wine APIs, without a parallel INF interpreter.
 * Return codes identify the first failed API; manifests are checked by
 * the host driver after execution, independently of these return codes.
 * SPDX-License-Identifier: MIT */
#include "fdi.h"
#include "inf_sections.h"
extern BOOL W32API GetExitCodeProcess(HANDLE,DWORD *);
extern void W32API ExitProcess(DWORD);
extern BOOL W32API TerminateProcess(HANDLE,DWORD);
#define FAIL(n) do { result=(n); goto cleanup; } while(0)
static int run(void)
{
    char base[260],inf[260],cab[260],tree[260],temp[260],command[800];
    int result=0;HANDLE setup=0,adv=0,api=0,h=K32_INVALID,process=0;
    DWORD key=0;void (W32API *close)(HANDLE)=0;
    LONG (W32API *regclose)(DWORD)=0;
    int reserved=0,created=0;tree[0]=0;
    DWORD n=GetModuleFileNameA(0,base,260);if(!n || n>=260)FAIL(1);
    unsigned last=0;for(unsigned i=0;i<n;i++)if(base[i]=='\\' || base[i]=='/')last=i;
    base[last]=0;if(!join(inf,base,"chain.inf") || !join(cab,base,"fixture.cab"))FAIL(2);
    if(!GetTempPathA(260,temp) || !(reserved=GetTempFileNameA(temp,"CIC",0,tree)!=0))FAIL(3);
    if(!DeleteFileA(tree))FAIL(3);
    reserved=0;
    if(!(created=CreateDirectoryA(tree,0)!=0))FAIL(3);
    if(!extract(tree,cab))FAIL(4);
    if(!join(inf,tree,"chain.inf"))FAIL(2);
    setup=LoadLibraryExA("setupapi.dll",0,0),adv=LoadLibraryExA("advpack.dll",0,0);
    if(!setup || !adv)FAIL(5);
    HANDLE (W32API *open)(const char *,const char *,DWORD,DWORD *)=GetProcAddress(setup,"SetupOpenInfFileA");
    inf_install install=GetProcAddress(setup,"SetupInstallFromInfSectionA");
    struct inf_queue_api queue={
        .init=GetProcAddress(setup,"SetupInitDefaultQueueCallback"),
        .callback=GetProcAddress(setup,"SetupDefaultQueueCallbackA"),
        .term=GetProcAddress(setup,"SetupTermDefaultQueueCallback")};
    close=GetProcAddress(setup,"SetupCloseInfFile");
    LONG (W32API *runsetup)(HANDLE,const char *,const char *,const char *,const char *,HANDLE *,DWORD,void *)=
        GetProcAddress(adv,"RunSetupCommandA");
    int (W32API *launch)(HANDLE,HANDLE,char *,int)=GetProcAddress(adv,"LaunchINFSectionA");
    LONG (W32API *launchex)(HANDLE,HANDLE,char *,int)=GetProcAddress(adv,"LaunchINFSectionExA");
    if(!open || !install || !close || !runsetup || !launch || !launchex ||
       !queue.init || !queue.callback || !queue.term)FAIL(6);
    api=LoadLibraryExA("advapi32.dll",0,0);if(!api)FAIL(17);
    LONG (W32API *regopen)(DWORD,const char *,DWORD *)=GetProcAddress(api,"RegCreateKeyA");
    LONG (W32API *regset)(DWORD,const char *,DWORD,DWORD,const void *,DWORD)=GetProcAddress(api,"RegSetValueExA");
    LONG (W32API *regquery)(DWORD,const char *,DWORD *,DWORD *,void *,DWORD *)=GetProcAddress(api,"RegQueryValueExA");
    regclose=GetProcAddress(api,"RegCloseKey");
    if(!regopen || !regset || !regquery || !regclose)FAIL(18);
    DWORD obsolete=9;
    if(regopen(0x80000002u,"Software\\CiukiOS\\InstallerFixture",&key) ||
       regset(key,"Obsolete",0,4,&obsolete,4) || regclose(key))FAIL(19);
    key=0;
    DWORD line=0;h=open(inf,0,2,&line);if(h==K32_INVALID)FAIL(7);
    /* Exercise the three SPINST subsets separately against the same tree. */
    int section_error=install_sections(h,tree,install,&queue);
    if(section_error)FAIL(section_error);
    close(h);h=K32_INVALID;
    DWORD type=0,bytes=4,installed=0;
    if(regopen(0x80000002u,"Software\\CiukiOS\\InstallerFixture",&key))FAIL(20);
    int registry_ok=!regquery(key,"Installed",0,&type,&installed,&bytes) && type==4 && bytes==4 && installed==1;
    bytes=4;if(regquery(key,"Obsolete",0,&type,&obsolete,&bytes)!=2)registry_ok=0;
    if(regclose(key))registry_ok=0;key=0;FreeLibrary(api);api=0;if(!registry_ok)FAIL(21);
    char ini[32];if(GetPrivateProfileStringA("Fixture","Installed","missing",ini,sizeof(ini),"fixture.ini")!=1 || ini[0]!='1')FAIL(22);
    const char expected[]="CiukiOS authored installer fixture. No commercial application data.\n";
    char payload[sizeof(expected)];DWORD got=0;
    HANDLE payload_file=CreateFileA("C:\\Program Files\\Ciuki INF Fixture\\payload.txt",0x80000000u,1,0,3,0,0);
    if(payload_file==K32_INVALID)FAIL(23);
    int copied=GetFileSize(payload_file,0)==sizeof(expected)-1 && ReadFile(payload_file,payload,sizeof(payload),&got,0) && got==sizeof(expected)-1;
    CloseHandle(payload_file);if(!copied)FAIL(24);
    for(unsigned i=0;i<got;i++)if(payload[i]!=expected[i])FAIL(24);
    if(runsetup(0,inf,"DefaultInstall",tree,"Ciuki INF fixture",0,1|4,0)<0)FAIL(11);
    const char *suffix=",DefaultInstall,1";unsigned at=0;
    for(unsigned i=0;inf[i];i++)command[at++]=inf[i];for(unsigned i=0;;i++){command[at++]=suffix[i];if(!suffix[i])break;}
    if(launch(0,0,command,0)!=0)FAIL(12);
    /* Ex extracts the INF from a cabinet in a writable temporary folder. */
    char tempinf[260],tempcab[260];
    if(!join(tempinf,tree,"chain.inf") || !join(tempcab,tree,"fixture.cab") || !CopyFileA(cab,tempcab,1))FAIL(25);
    if(!DeleteFileA(tempinf))FAIL(26);
    at=0;const char *infname="chain.inf";for(unsigned i=0;infname[i];i++)command[at++]=infname[i];
    const char *section=",DefaultInstall,";for(unsigned i=0;section[i];i++)command[at++]=section[i];
    for(unsigned i=0;tempcab[i];i++)command[at++]=tempcab[i];command[at++]=',';command[at++]='4';command[at]=0;
    if(launchex(0,0,command,0)!=0)FAIL(13);
    /* RunSetupCommand's executable mode returns a waitable process handle. */
    char child[260];if(!join(child,tree,"child.exe"))FAIL(14);
    at=0;command[at++]='"';for(unsigned i=0;child[i];i++)command[at++]=child[i];command[at++]='"';command[at]=0;
    DWORD code=~0u;
    if(runsetup(0,command,0,tree,0,&process,0,0)<0 || !process)FAIL(15);
    DWORD waited=WaitForSingleObject(process,30000);
    if(waited!=0){TerminateProcess(process,1);WaitForSingleObject(process,~0u);}
    if(waited!=0 || !GetExitCodeProcess(process,&code) || code)result=16;
cleanup:
    if(process)CloseHandle(process);
    if(h!=K32_INVALID && close)close(h);
    if(key && regclose)regclose(key);
    if(api)FreeLibrary(api);if(adv)FreeLibrary(adv);if(setup)FreeLibrary(setup);
    if(created){
        char file[260];const char *names[]={"child.done","child.exe","payload.txt","chain.inf","fixture.cab"};
        for(unsigned i=0;i<5;i++)if(join(file,tree,names[i]) && GetFileAttributesA(file)!=~0u && !DeleteFileA(file))result=result?result:16;
        if(!RemoveDirectoryA(tree))result=result?result:16;
    }else if(reserved && !DeleteFileA(tree))result=result?result:16;
    return result;
}
int W32API entry(void){ExitProcess((DWORD)run());return 1;}
