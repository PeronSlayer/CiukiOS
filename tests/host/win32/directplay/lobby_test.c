/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define COBJMACROS
#define INITGUID
#include "lobby-under-test.c"

struct key {int used;WCHAR name[256];WCHAR values[5][512];};
static struct key keys[16];
static int open_handles, write_error;
static const WCHAR *value_names[]={L"Guid",L"File",L"CommandLine",L"Path",L"CurrentDirectory"};
static int equal(const WCHAR *a,const WCHAR *b) {while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void copy(WCHAR *a,const WCHAR *b) {do {*a++=*b;}while(*b++);}
INT WINAPI lstrcmpiW(const WCHAR *a,const WCHAR *b) {while(*a||*b){unsigned x=*a++,y=*b++;if(x>='a'&&x<='z')x-=32;if(y>='a'&&y<='z')y-=32;if(x!=y)return x<y?-1:1;}return 0;}
int WINAPI MultiByteToWideChar(UINT cp,DWORD flags,const char *s,int n,WCHAR *out,int cap) {(void)cp;(void)flags;if(n<0)n=strlen(s)+1;if(out){assert(cap>=n);for(int i=0;i<n;i++)out[i]=(unsigned char)s[i];}return n;}
int WINAPI WideCharToMultiByte(UINT cp,DWORD flags,const WCHAR *s,int n,char *out,int cap,const char *def,BOOL *used) {(void)cp;(void)flags;(void)def;(void)used;if(n<0)n=lstrlenW(s)+1;if(out){assert(cap>=n);for(int i=0;i<n;i++)out[i]=s[i];}return n;}
int WINAPI StringFromGUID2(REFGUID g,WCHAR *out,int cap) {char text[40];snprintf(text,sizeof(text),"{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",g->Data1,g->Data2,g->Data3,g->Data4[0],g->Data4[1],g->Data4[2],g->Data4[3],g->Data4[4],g->Data4[5],g->Data4[6],g->Data4[7]);assert(cap>=39);for(int i=0;i<39;i++)out[i]=text[i];return 39;}
HRESULT WINAPI CLSIDFromString(const WCHAR *text,CLSID *guid) {char s[40];if(lstrlenW(text)!=38)return E_INVALIDARG;for(int i=0;i<39;i++)s[i]=text[i];unsigned a,b,c,d[8];int n=sscanf(s,"{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}",&a,&b,&c,&d[0],&d[1],&d[2],&d[3],&d[4],&d[5],&d[6],&d[7]);if(n!=11)return E_INVALIDARG;guid->Data1=a;guid->Data2=b;guid->Data3=c;for(int i=0;i<8;i++)guid->Data4[i]=d[i];return S_OK;}
static struct key *key(HKEY handle) {unsigned n=(ULONG_PTR)handle;assert(n>=2&&n<16&&keys[n].used);return &keys[n];}
LSTATUS WINAPI RegCreateKeyExW(HKEY parent,const WCHAR *name,DWORD reserved,WCHAR *klass,DWORD options,REGSAM access,SECURITY_ATTRIBUTES *security,HKEY *out,DWORD *disposition) {(void)reserved;(void)klass;(void)options;(void)access;(void)security;(void)disposition;
 if(parent==HKEY_LOCAL_MACHINE){*out=(HKEY)1;open_handles++;return 0;}
 assert(parent==(HKEY)1);
 for(unsigned i=2;i<16;i++)if(keys[i].used&&equal(keys[i].name,name)){*out=(HKEY)(ULONG_PTR)i;open_handles++;return 0;}
 for(unsigned i=2;i<16;i++)if(!keys[i].used){memset(&keys[i],0,sizeof(keys[i]));keys[i].used=1;copy(keys[i].name,name);*out=(HKEY)(ULONG_PTR)i;open_handles++;return 0;}return ERROR_NOT_ENOUGH_MEMORY;
}
LSTATUS WINAPI RegOpenKeyExW(HKEY parent,const WCHAR *name,DWORD options,REGSAM access,HKEY *out) {(void)options;(void)access;if(parent==HKEY_LOCAL_MACHINE){*out=(HKEY)1;open_handles++;return 0;}assert(parent==(HKEY)1);for(unsigned i=2;i<16;i++)if(keys[i].used&&equal(keys[i].name,name)){*out=(HKEY)(ULONG_PTR)i;open_handles++;return 0;}return ERROR_FILE_NOT_FOUND;}
LSTATUS WINAPI RegCloseKey(HKEY h) {(void)h;assert(open_handles>0);open_handles--;return 0;}
LSTATUS WINAPI RegSetValueExW(HKEY h,const WCHAR *name,DWORD reserved,DWORD type,const BYTE *data,DWORD size) {(void)reserved;assert(type==REG_SZ&&size<=sizeof(keys[0].values[0]));if(write_error)return ERROR_ACCESS_DENIED;for(int i=0;i<5;i++)if(equal(name,value_names[i])){memcpy(key(h)->values[i],data,size);return 0;}abort();}
LSTATUS WINAPI RegQueryValueExW(HKEY h,const WCHAR *name,DWORD *reserved,DWORD *type,BYTE *data,DWORD *size) {(void)reserved;for(int i=0;i<5;i++)if(equal(name,value_names[i])){WCHAR *text=key(h)->values[i];if(!*text&&i==0)return ERROR_FILE_NOT_FOUND;unsigned n=(lstrlenW(text)+1)*2;if(type)*type=REG_SZ;if(data){if(*size<n){*size=n;return ERROR_MORE_DATA;}memcpy(data,text,n);}*size=n;return 0;}return ERROR_FILE_NOT_FOUND;}
LSTATUS WINAPI RegEnumKeyExW(HKEY h,DWORD index,WCHAR *name,DWORD *length,DWORD *reserved,WCHAR *klass,DWORD *klass_length,FILETIME *time) {(void)reserved;(void)klass;(void)klass_length;(void)time;assert(h==(HKEY)1);unsigned n=0;for(unsigned i=2;i<16;i++)if(keys[i].used&&n++==index){assert(*length>(unsigned)lstrlenW(keys[i].name));copy(name,keys[i].name);*length=lstrlenW(name);return 0;}return ERROR_NO_MORE_ITEMS;}
LSTATUS WINAPI RegDeleteKeyW(HKEY h,const WCHAR *name) {assert(h==(HKEY)1);for(int i=2;i<16;i++)if(keys[i].used&&equal(name,keys[i].name)){keys[i].used=0;return 0;}return ERROR_FILE_NOT_FOUND;}
void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs) {(void)cs;}
void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs) {(void)cs;}
static int seen;
static GUID expected={0x12345678,0x1234,0x5678,{1,2,3,4,5,6,7,8}};
static BOOL CALLBACK enumerate(const DPLAPPINFO *info,void *context,DWORD flags) {assert(!flags&&info->dwSize==sizeof(*info));assert(IsEqualGUID(&info->guidApplication,&expected));if(context)assert(!strcmp(info->lpszAppNameA,"LAN test"));else assert(equal(info->lpszAppName,L"LAN test"));seen++;return FALSE;}
int main(void) {
 DPAPPLICATIONDESC app={0};app.dwSize=sizeof(app);app.guidApplication=expected;app.lpszApplicationNameA="LAN test";app.lpszFilenameA="test.exe";app.lpszPathA="C:\\Games\\Test";app.lpszCommandLineA="--lobby";
 assert(DPL_Register(1,&app,TRUE)==DPERR_INVALIDPARAMS);assert(DPL_Register(0,&app,TRUE)==DP_OK);assert(!open_handles);
 assert(equal(keys[2].values[1],L"test.exe")&&equal(keys[2].values[3],L"C:\\Games\\Test"));
 IDirectPlayLobbyImpl lobby={0};assert(DPL_EnumApplications(&lobby,enumerate,(void*)1,0,TRUE)==DP_OK);assert(seen==1&&!open_handles);
 assert(DPL_EnumApplications(&lobby,enumerate,NULL,0,FALSE)==DP_OK);assert(seen==2&&!open_handles);
 assert(DPL_Unregister(0,&expected)==DP_OK);assert(!open_handles);assert(DPL_Unregister(0,&expected)==DPERR_UNKNOWNAPPLICATION);assert(!open_handles);
 app.lpszApplicationName=L"LAN test";app.lpszFilename=L"test.exe";app.lpszPath=L"C:\\Games\\Test";app.lpszCommandLine=L"--wide";
 assert(DPL_Register(0,&app,FALSE)==DP_OK);assert(!open_handles);
 write_error=1;assert(DPL_Register(0,&app,FALSE)==DPERR_ACCESSDENIED);write_error=0;assert(!open_handles);
 assert(DPL_Unregister(0,&expected)==DP_OK);assert(!open_handles);
 app.lpszApplicationName=L"invalid\\child";assert(DPL_Register(0,&app,FALSE)==DPERR_INVALIDPARAMS);
 assert(DPL_Register(0,NULL,FALSE)==DPERR_INVALIDPARAMS);assert(DPL_Unregister(1,&expected)==DPERR_INVALIDPARAMS);
 puts("PASS patched Wine lobby: A/W registration, fields/GUIDs, enumeration/callback stop, unregistration, failures and key lifetime");fflush(stdout);return 0;
}
