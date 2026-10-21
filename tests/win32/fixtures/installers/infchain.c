/* Execute the reused Wine APIs, without a parallel INF interpreter.
 * Return codes identify the first failed API; manifests are checked by
 * the host driver after execution, independently of these return codes.
 * SPDX-License-Identifier: MIT */
#include "fdi.h"
#include "inf_sections.h"
extern BOOL W32API GetExitCodeProcess(HANDLE,DWORD *);
extern void W32API ExitProcess(DWORD);
static int run(void)
{
    char base[260],inf[260],cab[260],tree[260],temp[260],command[800];
    DWORD n=GetModuleFileNameA(0,base,260);if(!n || n>=260)return 1;
    unsigned last=0;for(unsigned i=0;i<n;i++)if(base[i]=='\\' || base[i]=='/')last=i;
    base[last]=0;if(!join(inf,base,"chain.inf") || !join(cab,base,"fixture.cab"))return 2;
    if(!GetTempPathA(260,temp) || !GetTempFileNameA(temp,"CIC",0,tree) || !DeleteFileA(tree) || !CreateDirectoryA(tree,0))return 3;
    if(!extract(tree,cab))return 4;
    if(!join(inf,tree,"chain.inf"))return 2;
    HANDLE setup=LoadLibraryExA("setupapi.dll",0,0),adv=LoadLibraryExA("advpack.dll",0,0);
    if(!setup || !adv)return 5;
    HANDLE (W32API *open)(const char *,const char *,DWORD,DWORD *)=GetProcAddress(setup,"SetupOpenInfFileA");
    inf_install install=GetProcAddress(setup,"SetupInstallFromInfSectionA");
    struct inf_queue_api queue={
        .init=GetProcAddress(setup,"SetupInitDefaultQueueCallback"),
        .callback=GetProcAddress(setup,"SetupDefaultQueueCallbackA"),
        .term=GetProcAddress(setup,"SetupTermDefaultQueueCallback")};
    void (W32API *close)(HANDLE)=GetProcAddress(setup,"SetupCloseInfFile");
    LONG (W32API *runsetup)(HANDLE,const char *,const char *,const char *,const char *,HANDLE *,DWORD,void *)=
        GetProcAddress(adv,"RunSetupCommandA");
    int (W32API *launch)(HANDLE,HANDLE,char *,int)=GetProcAddress(adv,"LaunchINFSectionA");
    LONG (W32API *launchex)(HANDLE,HANDLE,char *,int)=GetProcAddress(adv,"LaunchINFSectionExA");
    if(!open || !install || !close || !runsetup || !launch || !launchex ||
       !queue.init || !queue.callback || !queue.term)return 6;
    HANDLE api=LoadLibraryExA("advapi32.dll",0,0);if(!api)return 17;
    LONG (W32API *regopen)(DWORD,const char *,DWORD *)=GetProcAddress(api,"RegCreateKeyA");
    LONG (W32API *regset)(DWORD,const char *,DWORD,DWORD,const void *,DWORD)=GetProcAddress(api,"RegSetValueExA");
    LONG (W32API *regquery)(DWORD,const char *,DWORD *,DWORD *,void *,DWORD *)=GetProcAddress(api,"RegQueryValueExA");
    LONG (W32API *regclose)(DWORD)=GetProcAddress(api,"RegCloseKey");
    if(!regopen || !regset || !regquery || !regclose)return 18;
    DWORD key=0,obsolete=9;
    if(regopen(0x80000002u,"Software\\CiukiOS\\InstallerFixture",&key) ||
       regset(key,"Obsolete",0,4,&obsolete,4) || regclose(key))return 19;
    DWORD line=0;HANDLE h=open(inf,0,2,&line);if(h==K32_INVALID)return 7;
    /* Exercise the three SPINST subsets separately against the same tree. */
    int section_error=install_sections(h,tree,install,&queue);
    if(section_error){close(h);return section_error;}
    close(h);
    DWORD type=0,bytes=4,installed=0;
    if(regopen(0x80000002u,"Software\\CiukiOS\\InstallerFixture",&key))return 20;
    int registry_ok=!regquery(key,"Installed",0,&type,&installed,&bytes) && type==4 && bytes==4 && installed==1;
    bytes=4;if(regquery(key,"Obsolete",0,&type,&obsolete,&bytes)!=2)registry_ok=0;
    if(regclose(key))registry_ok=0;FreeLibrary(api);if(!registry_ok)return 21;
    char ini[32];if(GetPrivateProfileStringA("Fixture","Installed","missing",ini,sizeof(ini),"fixture.ini")!=1 || ini[0]!='1')return 22;
    const char expected[]="CiukiOS authored installer fixture. No commercial application data.\n";
    char payload[sizeof(expected)];DWORD got=0;
    HANDLE payload_file=CreateFileA("C:\\Program Files\\Ciuki INF Fixture\\payload.txt",0x80000000u,1,0,3,0,0);
    if(payload_file==K32_INVALID)return 23;
    int copied=GetFileSize(payload_file,0)==sizeof(expected)-1 && ReadFile(payload_file,payload,sizeof(payload),&got,0) && got==sizeof(expected)-1;
    CloseHandle(payload_file);if(!copied)return 24;
    for(unsigned i=0;i<got;i++)if(payload[i]!=expected[i])return 24;
    if(runsetup(0,inf,"DefaultInstall",tree,"Ciuki INF fixture",0,1|4,0)<0)return 11;
    const char *suffix=",DefaultInstall,1";unsigned at=0;
    for(unsigned i=0;inf[i];i++)command[at++]=inf[i];for(unsigned i=0;;i++){command[at++]=suffix[i];if(!suffix[i])break;}
    if(launch(0,0,command,0)!=0)return 12;
    /* Ex extracts the INF from a cabinet in a writable temporary folder. */
    char tempinf[260],tempcab[260];
    if(!join(tempinf,tree,"chain.inf") || !join(tempcab,tree,"fixture.cab") || !CopyFileA(cab,tempcab,1))return 25;
    if(!DeleteFileA(tempinf))return 26;
    at=0;const char *infname="chain.inf";for(unsigned i=0;infname[i];i++)command[at++]=infname[i];
    const char *section=",DefaultInstall,";for(unsigned i=0;section[i];i++)command[at++]=section[i];
    for(unsigned i=0;tempcab[i];i++)command[at++]=tempcab[i];command[at++]=',';command[at++]='4';command[at]=0;
    if(launchex(0,0,command,0)!=0)return 13;
    /* RunSetupCommand's executable mode returns a waitable process handle. */
    char child[260];if(!join(child,tree,"child.exe"))return 14;
    at=0;command[at++]='"';for(unsigned i=0;child[i];i++)command[at++]=child[i];command[at++]='"';command[at]=0;
    HANDLE process=0;DWORD code=~0u;
    if(runsetup(0,command,0,tree,0,&process,0,0)<0 || !process)return 15;
    int ok=WaitForSingleObject(process,30000)==0 && GetExitCodeProcess(process,&code) && !code;
    CloseHandle(process);FreeLibrary(adv);FreeLibrary(setup);
    char file[260];const char *names[]={"child.done","child.exe","payload.txt","chain.inf","fixture.cab"};
    for(unsigned i=0;i<5;i++)if(join(file,tree,names[i]) && GetFileAttributesA(file)!=~0u && !DeleteFileA(file))ok=0;
    if(!RemoveDirectoryA(tree))ok=0;return ok?0:16;
}
int W32API entry(void){ExitProcess((DWORD)run());return 1;}
