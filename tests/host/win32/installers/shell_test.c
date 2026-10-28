/* Round-trip with the independent production desktop decoder.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "host.h"
#include "../../../../apps/desktop/shell_link.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct link;
struct interface {const void *const *vt;struct link *owner;};
struct factory {const void *const *vt;ULONG refs;};
#define METHOD(i,n,type) ((type)(i)->vt[n])
static SHELL_GUID guid(DWORD a){return (SHELL_GUID){.a=a,.d={0xc0,0,0,0,0,0,0,0x46}};}
extern BOOL SHGetSpecialFolderPathW(HANDLE,WCHAR *,int,BOOL);
extern HRESULT SHGetFolderPathW(HANDLE,int,HANDLE,DWORD,WCHAR *);
extern LONG RegOpenKeyA(DWORD,const char *,DWORD *);
extern LONG RegQueryValueExA(DWORD,const char *,DWORD *,DWORD *,void *,DWORD *);
extern LONG RegCloseKey(DWORD);
static void recent_sendto(void){
  const char *names[]={"Recent","SendTo"};
  const char *paths[]={"C:\\WINDOWS\\Recent","C:\\WINDOWS\\SendTo"};
  for(unsigned i=0;i<2;i++){
    char a[260];WCHAR w[260];
    assert(SHGetSpecialFolderPathA(0,a,8+(int)i,0) && !strcmp(a,paths[i]));
    assert(GetFileAttributesA(a)==~0u); /* Retrieval alone never creates. */
    assert(!SHGetFolderPathW(0,0x8008+(int)i,0,0,w) && shell_ansi(w,a,260) && !strcmp(a,paths[i]));
    assert(GetFileAttributesA(a)&16);
    assert(SHGetSpecialFolderPathW(0,w,8+(int)i,1) && shell_ansi(w,a,260) && !strcmp(a,paths[i]));
    for(unsigned root=0;root<2;root++)for(unsigned user=0;user<2;user++){
      char key[160],value[260];DWORD h,type=0,n=sizeof(value);
      snprintf(key,sizeof(key),"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\%sShell Folders",user?"User ":"");
      assert(!RegOpenKeyA(0x80000002u-root,key,&h));
      assert(!RegQueryValueExA(h,names[i],0,&type,value,&n) && type==1 && !strcmp(value,paths[i]));
      assert(!RegCloseKey(h));
    }
  }
  puts("PASS Recent/SendTo A/W paths, create flag and matching initial registry defaults");
}
static void folder_identifiers(void){
  /* The exact 9x NSIS route: current-user CSIDL, with/without CREATE,
   * PIDL -> ANSI path, and task-allocator ownership. */
  const unsigned ids[]={0,2,8,9,16,23,25};
  for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++){
    char expected[260],actual[260];WCHAR wide[260];void *pidl=0;
    assert(SHGetSpecialFolderPathA(0,expected,(int)ids[i],0));
    assert(!SHGetSpecialFolderLocation(0,(int)ids[i]|0x8000,&pidl) && pidl);
    assert(SHGetPathFromIDListA(pidl,actual) && !strcmp(actual,expected));
    assert(SHGetPathFromIDListW(pidl,wide) && shell_ansi(wide,actual,260) && !strcmp(actual,expected));
    /* The list consists of counted SHITEMIDs followed by a zero WORD. */
    const uint8_t *p=pidl;unsigned total=0;
    while(w32_u16(p)){unsigned n=w32_u16(p);assert(n>=3 && n<=1024 && total+n<1024);p+=n;total+=n;}
    if(ids[i]==16){
      assert(total>45 && ((uint8_t *)pidl)[2]==0x1f && ((uint8_t *)pidl)[22]==0x23);
      /* Malformed filesystem item must not become a traversal path. */
      uint8_t *folder=(uint8_t *)pidl+45;folder[14]='\\';
      assert(!SHGetPathFromIDListA(pidl,actual) && !actual[0]);
    }
    shell_free(pidl);
  }
  void *bad=(void *)1;assert(SHGetSpecialFolderLocation(0,31,&bad)<0 && !bad);
  assert(SHGetSpecialFolderLocation(0,16,0)<0);
  char path[260]="sentinel";assert(!SHGetPathFromIDListA(0,path) && !path[0]);
  puts("PASS NSIS 9x desktop/programs PIDL paths, ownership and malformed identifiers");
}
static void links(void){
  SHELL_GUID cls=guid(0x21401),factory_id=guid(1),link_id=guid(0x214ee),persist_id=guid(0x10b);
  struct factory *factory=0;struct interface *link=0,*persist=0;
  assert(!DllGetClassObject(&cls,&factory_id,(void **)&factory));
  assert(!METHOD(factory,3,HRESULT(*)(struct factory *,void *,const SHELL_GUID *,void **))(factory,0,&link_id,(void **)&link));
  assert(!METHOD(link,20,HRESULT(*)(struct interface *,const char *))(link,"C:\\Program Files\\Ciuki Fixture\\fixture.exe"));
  assert(!METHOD(link,7,HRESULT(*)(struct interface *,const char *))(link,"Fixture \x80"));
  assert(!METHOD(link,9,HRESULT(*)(struct interface *,const char *))(link,"C:\\Program Files\\Ciuki Fixture"));
  assert(!METHOD(link,11,HRESULT(*)(struct interface *,const char *))(link,"--title \"two words\""));
  assert(!METHOD(link,17,HRESULT(*)(struct interface *,const char *,int))(link,"C:\\Program Files\\Ciuki Fixture\\fixture.exe",-3));
  assert(!METHOD(link,15,HRESULT(*)(struct interface *,int))(link,3));
  uint16_t hotkey=0xffff;
  assert(!METHOD(link,12,HRESULT(*)(struct interface *,uint16_t *))(link,&hotkey) && !hotkey);
  assert(METHOD(link,12,HRESULT(*)(struct interface *,uint16_t *))(link,0)<0);
  assert(!METHOD(link,13,HRESULT(*)(struct interface *,uint16_t))(link,0x0646)); /* Ctrl+Alt+F */
  assert(METHOD(link,13,HRESULT(*)(struct interface *,uint16_t))(link,0x1046)<0);
  assert(METHOD(link,13,HRESULT(*)(struct interface *,uint16_t))(link,0x0600)<0);
  assert(!METHOD(link,12,HRESULT(*)(struct interface *,uint16_t *))(link,&hotkey) && hotkey==0x0646);
  assert(!METHOD(link,0,HRESULT(*)(struct interface *,const SHELL_GUID *,void **))(link,&persist_id,(void **)&persist));
  WCHAR filename[260];assert(shell_wide("C:\\WINDOWS\\Desktop\\Fixture.lnk",filename,260));
  assert(!METHOD(persist,6,HRESULT(*)(struct interface *,const WCHAR *,BOOL))(persist,filename,1));
  HANDLE h=CreateFileA("C:\\WINDOWS\\Desktop\\Fixture.lnk",0x80000000,1,0,3,0,0);assert(h!=K32_INVALID);
  uint8_t bytes[4096];DWORD got;assert(ReadFile(h,bytes,sizeof(bytes),&got,0));CloseHandle(h);
  struct desk_launch_record expected={.label="Fixture \xe2\x82\xac",.target="C:\\Program Files\\Ciuki Fixture\\fixture.exe",.arguments="--title \"two words\"",.directory="C:\\Program Files\\Ciuki Fixture",.icon="C:\\Program Files\\Ciuki Fixture\\fixture.exe",.show=3,.icon_index=-3,.hotkey=0x0646},decoded;
  assert(desk_link_decode(bytes,got,&decoded));assert(!memcmp(&expected,&decoded,sizeof(expected)));
  for(DWORD n=0;n<got;n++)assert(!desk_link_decode(bytes,n,&decoded));
  assert(!METHOD(persist,5,HRESULT(*)(struct interface *,const WCHAR *,DWORD))(persist,filename,0));
  assert(!METHOD(link,12,HRESULT(*)(struct interface *,uint16_t *))(link,&hotkey) && hotkey==0x0646);
  uint8_t encoded[4096];assert(desk_link_encode(&expected,encoded,sizeof(encoded))==got && !memcmp(bytes,encoded,got));
  const DWORD keys[]={0,0x0846,0x10046,0x1046,0x0600};
  for(unsigned k=0;k<sizeof(keys)/sizeof(*keys);k++){
    w32_put32(bytes+36,keys[k]);
    int acceptable=k<2;assert(desk_link_decode(bytes,got,&decoded)==acceptable);
    h=CreateFileA("C:\\WINDOWS\\Desktop\\Fixture.lnk",0x40000000u,0,0,2,128,0);assert(h!=K32_INVALID);
    DWORD wrote;assert(WriteFile(h,bytes,got,&wrote,0) && wrote==got);assert(CloseHandle(h));
    assert((METHOD(persist,5,HRESULT(*)(struct interface *,const WCHAR *,DWORD))(persist,filename,0)==0)==acceptable);
    if(acceptable)hotkey=(uint16_t)keys[k];
    uint16_t actual;assert(!METHOD(link,12,HRESULT(*)(struct interface *,uint16_t *))(link,&actual) && actual==hotkey);
  }
  assert(!METHOD(link,13,HRESULT(*)(struct interface *,uint16_t))(link,0));
  assert(!METHOD(link,12,HRESULT(*)(struct interface *,uint16_t *))(link,&hotkey) && !hotkey);
  assert(!METHOD(link,13,HRESULT(*)(struct interface *,uint16_t))(link,0x0646));
  assert(!METHOD(persist,6,HRESULT(*)(struct interface *,const WCHAR *,BOOL))(persist,filename,1));
  char target[260];assert(!METHOD(link,3,HRESULT(*)(struct interface *,char *,int,void *,DWORD))(link,target,260,0,0));assert(!strcmp(target,expected.target));
  assert(METHOD(persist,2,ULONG(*)(struct interface *))(persist)==1);assert(METHOD(link,2,ULONG(*)(struct interface *))(link)==0);METHOD(factory,2,ULONG(*)(struct factory *))(factory);
  puts("PASS IShellLink / IPersistFile / desktop reader UTF-8 exact round-trip");
}
extern HANDLE ShellExecuteA(HANDLE,const char *,const char *,const char *,const char *,int);
static void operations(void){
  assert(shell_mkdirs("C:\\Source Tree\\Child Folder"));assert(shell_mkdirs("C:\\Destination"));
  host_put("C:\\Source Tree\\Child Folder\\data.txt","authored installer data\n");
  host_put("C:\\other.txt","second file\n");
  SHELL_FILEOP op={.operation=2,.from="C:\\Source Tree\0C:\\other.txt\0",.to="C:\\Destination\0",.flags=0x14};
  assert(!SHFileOperationA(&op) && !op.aborted);
  assert(GetFileAttributesA("C:\\Destination\\Source Tree\\Child Folder\\data.txt")!=~0u);
  op=(SHELL_FILEOP){.operation=2,.from="C:\\other.txt\0",.to="D:\\read-only.txt\0",.flags=0x14};assert(SHFileOperationA(&op)==19);
  host_full=1;op.to="C:\\full.txt\0";assert(SHFileOperationA(&op)==112);host_full=0;
  host_cancel=1;op.flags=0;assert(!SHFileOperationA(&op) && op.aborted);host_cancel=0;
  op=(SHELL_FILEOP){.operation=2,.from="C:\\Source Tree\0",.to="C:\\Source Tree\\Child Folder\0",.flags=0x14};assert(SHFileOperationA(&op)==0x76);
  op=(SHELL_FILEOP){.operation=3,.from="C:\\Destination\\Source Tree\0",.flags=0x14};assert(!SHFileOperationA(&op));
  assert(CopyFileA("C:\\other.txt","C:\\temporary.txt",1));
  op=(SHELL_FILEOP){.operation=4,.from="C:\\temporary.txt\0",.to="C:\\Destination\\renamed.txt\0",.flags=0x14};assert(SHFileOperationA(&op)==0x73);
  op.to="C:\\renamed.txt\0";assert(!SHFileOperationA(&op));
  op=(SHELL_FILEOP){.operation=1,.from="C:\\renamed.txt\0",.to="C:\\Destination\0",.flags=0x14};assert(!SHFileOperationA(&op));
  op=(SHELL_FILEOP){.operation=3,.from="C:\\Destination\\renamed.txt\0",.flags=0x14};assert(!SHFileOperationA(&op));
  op=(SHELL_FILEOP){.operation=3,.from="C:\\\0",.flags=0x14};assert(SHFileOperationA(&op)==0x74);
  op.from="C:\\.\\\0";assert(SHFileOperationA(&op)==0x74);
  puts("PASS shell trees multi-source / spaces / subtree / cancel / disk-full / CD errors");
}
/* G2: FOF_ALLOWUNDO deletes go to the Recycle Bin with Windows 9x INFO2 records. */
static DWORD read_all(const char *path,uint8_t *out,DWORD cap){
  HANDLE h=CreateFileA(path,0x80000000u,1,0,3,0,0);if(h==K32_INVALID)return ~0u;
  DWORD got=0;assert(ReadFile(h,out,cap,&got,0));CloseHandle(h);return got;
}
static void recycle(void){
  uint8_t info[4096];
  assert(shell_mkdirs("C:\\Work\\Tree\\Deep"));
  host_put("C:\\Work\\a.txt","12345");host_put("C:\\Work\\Tree\\Deep\\b.dat","0123456789");host_put("C:\\Work\\Tree\\c.dat","abc");
  SHELL_FILEOP op={.operation=3,.from="C:\\Work\\a.txt\0C:\\Work\\Tree\\Deep\\..\\..\\Tree\0",.flags=0x54};   /* ALLOWUNDO | SILENT | NOCONFIRMATION */
  assert(!SHFileOperationA(&op) && !op.aborted);
  assert(GetFileAttributesA("C:\\Work\\a.txt")==~0u && GetFileAttributesA("C:\\Work\\Tree")==~0u);
  assert(GetFileAttributesA("C:\\RECYCLED\\Dc1.txt")==128 && (GetFileAttributesA("C:\\RECYCLED\\Dc2")&16));
  assert(GetFileAttributesA("C:\\RECYCLED\\Dc2\\Deep\\b.dat")==128 && GetFileAttributesA("C:\\RECYCLED\\INFO2.lck")==~0u);
  DWORD n=read_all("C:\\RECYCLED\\INFO2",info,sizeof(info));
  assert(n==20+2*280 && w32_u32(info)==4 && w32_u32(info+4)==3 && w32_u32(info+8)==2 && w32_u32(info+12)==280);
  assert(!strcmp((char *)info+20,"C:\\Work\\a.txt") && w32_u32(info+20+260)==1 && w32_u32(info+20+264)==2 && w32_u32(info+20+276)==5);
  uint64_t ft=(uint64_t)(1792000000ull+11644473600ull)*10000000ull;
  assert(w32_u32(info+20+268)==(DWORD)ft && w32_u32(info+20+272)==(DWORD)(ft>>32));
  assert(!strcmp((char *)info+20+280,"C:\\Work\\Tree") && w32_u32(info+20+280+260)==2 && w32_u32(info+20+280+276)==13);
  /* A second delete continues the numbering. */
  host_put("C:\\Work\\again.txt","x");op.from="C:\\Work\\again.txt\0";assert(!SHFileOperationA(&op) && GetFileAttributesA("C:\\RECYCLED\\Dc3.txt")==128);
  assert(read_all("C:\\RECYCLED\\INFO2",info,sizeof(info))==20+3*280 && w32_u32(info+4)==4);
  /* The confirmation says Recycle Bin when the item will be recycled. */
  host_put("C:\\Work\\ask.txt","x");op.flags=0x44;op.from="C:\\Work\\ask.txt\0";assert(!SHFileOperationA(&op) && GetFileAttributesA("C:\\RECYCLED\\Dc4.txt")==128);
  /* CD-ROM and removable drives, relative names and the bin's own items delete at once. */
  host_put("E:\\fixed.txt","y");op.from="E:\\fixed.txt\0";op.flags=0x54;assert(!SHFileOperationA(&op) && GetFileAttributesA("E:\\RECYCLED\\De1.txt")==128);
  host_put("F:\\usb.txt","y");op.from="F:\\usb.txt\0";assert(!SHFileOperationA(&op) && GetFileAttributesA("F:\\usb.txt")==~0u && GetFileAttributesA("F:\\RECYCLED")==~0u);
  op.from="C:\\RECYCLED\\Dc1.txt\0";assert(!SHFileOperationA(&op) && GetFileAttributesA("C:\\RECYCLED\\Dc1.txt")==~0u);
  /* No ALLOWUNDO: the old permanent delete. "Do not move files" and a too-large item are honoured. */
  host_put("C:\\Work\\perm.txt","z");op.from="C:\\Work\\perm.txt\0";op.flags=0x14;assert(!SHFileOperationA(&op) && GetFileAttributesA("C:\\RECYCLED\\Dc5.txt")==~0u);
  assert(shell_mkdirs("C:\\system\\settings"));host_put("C:\\system\\settings\\recycle.cfg","nuke=1\n");
  host_put("C:\\Work\\nuke.txt","z");op.from="C:\\Work\\nuke.txt\0";op.flags=0x54;assert(!SHFileOperationA(&op) && GetFileAttributesA("C:\\Work\\nuke.txt")==~0u && GetFileAttributesA("C:\\RECYCLED\\Dc5.txt")==~0u);
  host_put("C:\\system\\settings\\recycle.cfg","independent=1\ndrive.c.percent=1\ndrive.c.nuke=0\nnuke=1\n");
  {size_t big=6u<<20;char *data=malloc(big+1);assert(data);memset(data,'q',big);data[big]=0;host_put("C:\\Work\\big.bin",data);free(data);}
  op.from="C:\\Work\\big.bin\0";assert(SHFileOperationA(&op)==112 && GetFileAttributesA("C:\\Work\\big.bin")==128);   /* 1 % of 512 MiB is 5 MiB */
  host_put("C:\\system\\settings\\recycle.cfg","percent=10\n");
  op.flags=0x54;assert(!SHFileOperationA(&op) && GetFileAttributesA("C:\\Work\\big.bin")==~0u && GetFileAttributesA("C:\\RECYCLED\\Dc5.bin")==128);
  op.flags=0x74;assert(SHFileOperationA(&op)==120);                                        /* name mappings: still not provided */
  /* Leave the fixture tree as it was. */
  const char *junk[]={"C:\\RECYCLED\\Dc1.txt","C:\\RECYCLED\\Dc3.txt","C:\\RECYCLED\\Dc4.txt","C:\\RECYCLED\\Dc5.bin","C:\\RECYCLED\\Dc2\\Deep\\b.dat","C:\\RECYCLED\\Dc2\\c.dat",
    "C:\\RECYCLED\\INFO2","E:\\RECYCLED\\De1.txt","E:\\RECYCLED\\INFO2","C:\\system\\settings\\recycle.cfg"};
  for(unsigned i=0;i<sizeof(junk)/sizeof(*junk);i++)DeleteFileA(junk[i]);
  const char *dirs[]={"C:\\RECYCLED\\Dc2\\Deep","C:\\RECYCLED\\Dc2","C:\\RECYCLED","E:\\RECYCLED","C:\\Work","C:\\system\\settings","C:\\system"};
  for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++)RemoveDirectoryA(dirs[i]);
  puts("PASS shell FOF_ALLOWUNDO: Recycle Bin move with INFO2 records, numbering, fixed second drive, removable/relative/in-bin deletes, policy, too-large refusal");
}
static void execute(void){
  host_registry("Software\\Classes\\.ciuki","","CiukiFixture");
  host_registry("Software\\Classes\\CiukiFixture\\shell\\open\\command","","\"C:\\Program Files\\Ciuki Fixture\\fixture.exe\" \"%1\" %*");
  host_registry("Software\\Classes\\CiukiFixture\\shell\\print\\command","","\"C:\\Program Files\\Ciuki Fixture\\fixture.exe\" /print \"%1\"");
  SHELL_EXECUTE a={.size=sizeof(a),.mask=0x40,.file="C:\\Document.ciuki",.parameters="/extra",.show=1};
  assert(ShellExecuteExA(&a) && a.process && (uintptr_t)a.instance>32);assert(CloseHandle(a.process));
  assert((uintptr_t)ShellExecuteA(0,"print",a.file,0,0,1)>32);
  assert((uintptr_t)ShellExecuteA(0,"explore",a.file,0,0,1)<=32);
  a.file="C:\\setup.msi";assert(!ShellExecuteExA(&a) && host_refusals==1);
  a.file="https://example.com/";assert(!ShellExecuteExA(&a));
  a.file="C:\\msiexec.exe";assert(!ShellExecuteExA(&a) && host_refusals==2);
  puts("PASS ShellExecute association verbs / exact command tail / retained process / MSI / URL refusal");
  /* fx-q3vise: a folder (the VISE engine's call: SEE_MASK 0, no verb,
   * SW_SHOW) opens in Files, with Explorer's hidden stand-in window:
   * CabinetWClass titled with the folder's name, as Windows 98 names it. */
  assert(shell_mkdirs("C:\\WINDOWS\\Start Menu\\Programs\\Ciuki Game\\Extras"));
  SHELL_EXECUTE f={.size=sizeof(f),.mask=0x40,.file="C:\\WINDOWS\\Start Menu\\Programs\\Ciuki Game",.show=5};
  assert(ShellExecuteExA(&f) && (uintptr_t)f.instance>32 && !f.process);
  assert(host_folder_launches==1 && !strcmp(host_folder_launch[0],"/WINDOWS/Start Menu/Programs/Ciuki Game"));
  assert(host_window_count==1 && host_windows[0].live && !strcmp(host_windows[0].klass,"CabinetWClass") &&
         !strcmp(host_windows[0].title,"Ciuki Game") && host_windows[0].style==0x80000000u);
  /* The same folder again (a trailing backslash): Explorer only activates its window. */
  f.file="C:\\WINDOWS\\Start Menu\\Programs\\Ciuki Game\\";
  assert(ShellExecuteExA(&f) && (uintptr_t)f.instance>32 && host_folder_launches==1 && host_window_count==1);
  /* "explore" is Explorer's two-pane window class. */
  assert((uintptr_t)ShellExecuteA(0,"explore","C:\\WINDOWS\\Start Menu\\Programs\\Ciuki Game\\Extras",0,0,1)>32);
  assert(host_folder_launches==2 && !strcmp(host_folder_launch[1],"/WINDOWS/Start Menu/Programs/Ciuki Game/Extras"));
  assert(!strcmp(host_windows[1].klass,"ExploreWClass") && !strcmp(host_windows[1].title,"Extras"));
  /* Another drive is /mnt/<letter> in Files; the A form and the default verb. */
  assert(shell_mkdirs("E:\\Games"));
  assert((uintptr_t)ShellExecuteA(0,0,"E:\\Games",0,0,1)>32 && host_folder_launches==3 && !strcmp(host_folder_launch[2],"/mnt/e/Games"));
  /* A refusing desktop fails the open (ERROR_ACCESS_DENIED) and leaves no window behind. */
  assert(shell_mkdirs("C:\\Refused"));
  host_folder_refuse=1;f.file="C:\\Refused";
  assert(!ShellExecuteExA(&f) && GetLastError()==5 && (uintptr_t)f.instance==5 && host_window_count==4 && !host_windows[3].live);
  host_folder_refuse=0;
  /* A folder has no "print" verb; a drive root keeps the refusal; a missing folder is not a folder. */
  assert((uintptr_t)ShellExecuteA(0,"print","C:\\Refused",0,0,1)<=32);
  assert((uintptr_t)ShellExecuteA(0,0,"C:\\",0,0,1)<=32);
  assert((uintptr_t)ShellExecuteA(0,0,"C:\\Missing Folder",0,0,1)<=32 && host_folder_launches==3);
  const char *folders[]={"C:\\Refused","E:\\Games","C:\\WINDOWS\\Start Menu\\Programs\\Ciuki Game\\Extras","C:\\WINDOWS\\Start Menu\\Programs\\Ciuki Game"};
  for(unsigned i=0;i<sizeof(folders)/sizeof(*folders);i++)assert(RemoveDirectoryA(folders[i]));
  puts("PASS ShellExecute folders: Files launch, Explorer-class window and title, reuse, explore, other drive, refusal");
}
static void versions(void){
  extern DWORD VerInstallFileA(DWORD,const char *,const char *,const char *,const char *,const char *,char *,DWORD *);
  extern DWORD GetFileVersionInfoSizeA(const char *,DWORD *);
  extern BOOL GetFileVersionInfoA(const char *,DWORD,DWORD,void *);
  extern BOOL VerQueryValueA(const void *,const char *,void **,DWORD *);
  assert(shell_mkdirs("C:\\Program Files\\Ciuki Fixture"));assert(shell_mkdirs("C:\\Source DLLs"));
  host_put("C:\\Source DLLs\\gdi32.dll","authored application DLL copy\n");
  char tmp[260];DWORD cap=sizeof(tmp);
  assert(!VerInstallFileA(1,"gdi32.dll","gdi32.dll","C:\\Source DLLs","C:\\System\\Win32",0,tmp,&cap));
  assert(!tmp[0] && cap==1);
  cap=sizeof(tmp);assert(VerInstallFileA(0,"missing.dll","missing.dll","C:\\Source DLLs","C:\\Source DLLs",0,tmp,&cap)==0x10000u);
  assert(GetFileAttributesA("C:\\Program Files\\Ciuki Fixture\\gdi32.dll")!=~0u);
  assert(GetFileAttributesA("C:\\System\\Win32\\gdi32.dll")==~0u);
  DWORD ignored,n=GetFileVersionInfoSizeA("C:\\WINDOWS\\SYSTEM\\gdi32.dll",&ignored);assert(n==192);
  uint8_t data[192];void *fixed;DWORD bytes;
  assert(GetFileVersionInfoA("gdi32.dll",0,sizeof(data),data) && VerQueryValueA(data,"\\",&fixed,&bytes));
  assert(bytes==52 && w32_u32((uint8_t *)fixed+8)==0x4000a && w32_u32((uint8_t *)fixed+12)==(2222u<<16));
  puts("PASS manifest version / force-install redirect preserves builtin store");
}
static void profile_folders(void){
  const unsigned ids[]={0,16,2,5,6,7,8,9,11,21,26};
  const char *suffix[]={"Documents\\Desktop","Documents\\Desktop","Start Menu\\Programs","Documents","Favorites", "Start Menu\\Programs\\StartUp","Recent","SendTo","Start Menu","Templates","Application Data"};
  for(unsigned profile=1;profile<3;profile++){
    char root[128];snprintf(root,sizeof(root),"/Windows/Profiles/u%08x",profile);
    host_profile(profile==1 ? "Alice" : "Bob",root);
    for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++){
      char a[260],expected[260];WCHAR w[260];void *pidl=0;
      snprintf(expected,sizeof(expected),"C:\\Windows\\Profiles\\u%08x\\%s",profile,suffix[i]);
      assert(SHGetSpecialFolderPathA(0,a,(int)ids[i],0) && !strcmp(a,expected));
      if(i==0)assert(GetFileAttributesA(a)==~0u);
      assert(SHGetSpecialFolderPathW(0,w,(int)ids[i],1) && shell_ansi(w,a,260) && !strcmp(a,expected));
      assert(GetFileAttributesA(a)&16);
      assert(!SHGetSpecialFolderLocation(0,(int)ids[i],&pidl));
      assert(SHGetPathFromIDListA(pidl,a) && !strcmp(a,expected));shell_free(pidl);
    }
    char common[260];assert(SHGetSpecialFolderPathA(0,common,25,0) && !strcmp(common,"C:\\WINDOWS\\Desktop"));
    /* sec-user-05: CSIDL_PROFILE and CSIDL_LOCAL_APPDATA of the session. */
    char expected[260],a[260];snprintf(expected,sizeof(expected),"C:\\Windows\\Profiles\\u%08x",profile);
    assert(SHGetSpecialFolderPathA(0,a,40,0) && !strcmp(a,expected));
    snprintf(expected,sizeof(expected),"C:\\Windows\\Profiles\\u%08x\\Local Settings\\Application Data",profile);
    assert(SHGetSpecialFolderPathA(0,a,28,0) && !strcmp(a,expected));
  }
  host_profile("","");char path[260];assert(SHGetSpecialFolderPathA(0,path,5,0) && !strcmp(path,"C:\\Documents"));
  assert(!SHGetSpecialFolderPathA(0,path,40,0)); /* no profile: no CSIDL_PROFILE, as Windows 98 */
  puts("PASS g32b Shell A/W/PIDL: Alice/Bob personal folders, CREATE, common folder and default profile; sec-user-05 profile/local application data");
}
int main(int argc,char **argv){
  setvbuf(stdout,0,_IONBF,0);assert(argc==2);host_init(argv[1]);
  char path[260];const unsigned ids[]={2,5,7,11,16,20,22,23,24,25,26,35,36,37,38,43,46};
  for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++)assert(SHGetSpecialFolderPathA(0,path,(int)ids[i],1));
  assert(!SHGetSpecialFolderPathA(0,path,3,0) && !path[0]);
  assert(!SHGetSpecialFolderPathA(0,path,31,0) && !path[0]);
  recent_sendto();folder_identifiers();links();operations();recycle();execute();versions();profile_folders();host_finish();puts("PASS installer shell host adapter");return 0;
}
