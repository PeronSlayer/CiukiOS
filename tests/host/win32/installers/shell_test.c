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
  assert(!METHOD(link,0,HRESULT(*)(struct interface *,const SHELL_GUID *,void **))(link,&persist_id,(void **)&persist));
  WCHAR filename[260];assert(shell_wide("C:\\WINDOWS\\Desktop\\Fixture.lnk",filename,260));
  assert(!METHOD(persist,6,HRESULT(*)(struct interface *,const WCHAR *,BOOL))(persist,filename,1));
  HANDLE h=CreateFileA("C:\\WINDOWS\\Desktop\\Fixture.lnk",0x80000000,1,0,3,0,0);assert(h!=K32_INVALID);
  uint8_t bytes[4096];DWORD got;assert(ReadFile(h,bytes,sizeof(bytes),&got,0));CloseHandle(h);
  struct desk_launch_record expected={.label="Fixture \xe2\x82\xac",.target="C:\\Program Files\\Ciuki Fixture\\fixture.exe",.arguments="--title \"two words\"",.directory="C:\\Program Files\\Ciuki Fixture",.icon="C:\\Program Files\\Ciuki Fixture\\fixture.exe",.show=3,.icon_index=-3},decoded;
  assert(desk_link_decode(bytes,got,&decoded));assert(!memcmp(&expected,&decoded,sizeof(expected)));
  for(DWORD n=0;n<got;n++)assert(!desk_link_decode(bytes,n,&decoded));
  assert(!METHOD(persist,5,HRESULT(*)(struct interface *,const WCHAR *,DWORD))(persist,filename,0));
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
int main(int argc,char **argv){
  setvbuf(stdout,0,_IONBF,0);assert(argc==2);host_init(argv[1]);
  char path[260];const unsigned ids[]={2,5,7,11,16,20,22,23,24,25,26,35,36,37,38,43,46};
  for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++)assert(SHGetSpecialFolderPathA(0,path,(int)ids[i],1));
  assert(!SHGetSpecialFolderPathA(0,path,3,0) && !path[0]);
  assert(!SHGetSpecialFolderPathA(0,path,31,0) && !path[0]);
  links();operations();execute();versions();host_finish();puts("PASS installer shell host adapter");return 0;
}
