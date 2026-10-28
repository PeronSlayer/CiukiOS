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
LSTATUS WINAPI RegQueryValueExA(HKEY h,const char *name,DWORD *reserved,DWORD *type,BYTE *data,DWORD *size) {
 WCHAR wide[64];unsigned n=0;do{wide[n]=(unsigned char)name[n];}while(name[n++]);
 WCHAR text[512];DWORD bytes=sizeof(text);LSTATUS e=RegQueryValueExW(h,wide,reserved,type,(BYTE*)text,&bytes);if(e)return e;
 DWORD need=(DWORD)lstrlenW(text)+1;if(data){if(*size<need){*size=need;return ERROR_MORE_DATA;}for(unsigned i=0;i<need;i++)data[i]=(BYTE)text[i];}*size=need;return 0;
}
LSTATUS WINAPI RegEnumKeyExW(HKEY h,DWORD index,WCHAR *name,DWORD *length,DWORD *reserved,WCHAR *klass,DWORD *klass_length,FILETIME *time) {(void)reserved;(void)klass;(void)klass_length;(void)time;assert(h==(HKEY)1);unsigned n=0;for(unsigned i=2;i<16;i++)if(keys[i].used&&n++==index){assert(*length>(unsigned)lstrlenW(keys[i].name));copy(name,keys[i].name);*length=lstrlenW(name);return 0;}return ERROR_NO_MORE_ITEMS;}
LSTATUS WINAPI RegDeleteKeyW(HKEY h,const WCHAR *name) {assert(h==(HKEY)1);for(int i=2;i<16;i++)if(keys[i].used&&equal(name,keys[i].name)){keys[i].used=0;return 0;}return ERROR_FILE_NOT_FOUND;}
void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs) {(void)cs;}
void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs) {(void)cs;}
static int seen;
static GUID expected={0x12345678,0x1234,0x5678,{1,2,3,4,5,6,7,8}};
static BOOL CALLBACK enumerate(const DPLAPPINFO *info,void *context,DWORD flags) {assert(!flags&&info->dwSize==sizeof(*info));assert(IsEqualGUID(&info->guidApplication,&expected));if(context)assert(!strcmp(info->lpszAppNameA,"LAN test"));else assert(equal(info->lpszAppName,L"LAN test"));seen++;return FALSE;}
/* Execute the real launch/notification helpers against deterministic OS seams. */
struct os_object { int refs,signal,kind; };
struct os_handle { int live,object,remote; };
static struct os_object objects[256];
static struct os_handle os[256];
static int os_count,failure,reserved,terminated;
static DWORD launched_id;
static LPTHREAD_START_ROUTINE worker_callback;
static void *worker_argument;
static DPLCONNECTION saved_connection;
static DPSESSIONDESC2 saved_session;
static char saved_short[64],saved_address[128];
static HANDLE os_new(int kind) {
 int object=0,handle=0;while(object<256&&objects[object].refs)object++;while(handle<256&&os[handle].live)handle++;assert(object<256&&handle<256);
 objects[object]=(struct os_object){1,0,kind};os[handle]=(struct os_handle){1,object,0};os_count++;return (HANDLE)(ULONG_PTR)(handle+4096);
}
static struct os_handle *os_get(HANDLE h) {unsigned i=(ULONG_PTR)h-4096;assert(i<256&&os[i].live);return &os[i];}
BOOL WINAPI CloseHandle(HANDLE h) {struct os_handle *p=os_get(h);assert(objects[p->object].refs>0);objects[p->object].refs--;p->live=0;os_count--;return TRUE;}
BOOL WINAPI SetEvent(HANDLE h) {objects[os_get(h)->object].signal=1;return TRUE;}
HANDLE WINAPI CreateEventW(SECURITY_ATTRIBUTES *a,BOOL manual,BOOL initial,const WCHAR *name) {(void)a;(void)manual;(void)initial;(void)name;if(failure==4)return NULL;return os_new(4);}
BOOL WINAPI DuplicateHandle(HANDLE source,HANDLE h,HANDLE target,HANDLE *out,DWORD access,BOOL inherit,DWORD options) {
 (void)access;(void)inherit;if(failure==5)return FALSE;struct os_handle *p=os_get(h);int i=0;while(i<256&&os[i].live)i++;assert(i<256);
 os[i]=(struct os_handle){1,p->object,target!=GetCurrentProcess()};objects[p->object].refs++;os_count++;*out=(HANDLE)(ULONG_PTR)(i+4096);
 if(options&DUPLICATE_CLOSE_SOURCE){assert(source!=NULL);CloseHandle(h);}return TRUE;
}
BOOL WINAPI CreateProcessA(const char *name,char *command,SECURITY_ATTRIBUTES *pa,SECURITY_ATTRIBUTES *ta,BOOL inherit,DWORD flags,void *env,const char *dir,STARTUPINFOA *startup,PROCESS_INFORMATION *out) {
 (void)pa;(void)ta;(void)env;assert(name&&command&&dir&&startup->cb==sizeof(*startup)&&!inherit&&(flags&CREATE_SUSPENDED));
 assert(!strcmp(name,"C:\\Games\\Test\\test.exe")&&strstr(command,"\"C:\\Games\\Test\\test.exe\""));if(failure==1)return FALSE;
 memset(out,0,sizeof(*out));out->hProcess=os_new(1);out->hThread=os_new(2);out->dwProcessId=0x1234;launched_id=out->dwProcessId;return TRUE;
}
BOOL WINAPI TerminateProcess(HANDLE process,UINT code) {(void)code;objects[os_get(process)->object].signal=1;terminated++;for(unsigned i=0;i<256;i++)if(os[i].live&&os[i].remote)CloseHandle((HANDLE)(ULONG_PTR)(i+4096));return TRUE;}
DWORD WINAPI ResumeThread(HANDLE thread) {os_get(thread);assert(reserved&&saved_connection.lpSessionDesc==&saved_session);return failure==9 ? (DWORD)-1 : 1;}
HANDLE WINAPI CreateThread(SECURITY_ATTRIBUTES *a,SIZE_T stack,LPTHREAD_START_ROUTINE callback,void *arg,DWORD flags,DWORD *id) {
 (void)a;(void)stack;(void)flags;if(failure==7)return NULL;assert(!worker_callback);worker_callback=callback;worker_argument=arg;*id=99;return os_new(3);
}
DWORD WINAPI WaitForMultipleObjects(DWORD count,const HANDLE *handles,BOOL all,DWORD timeout) {
 assert(!all&&timeout==INFINITE&&(count==2||count==3));for(unsigned i=0;i<count;i++){struct os_handle *h=os_get(handles[i]);if(objects[h->object].signal)return WAIT_OBJECT_0+i;}
 return WAIT_OBJECT_0+count-1; /* Child reads settings, then exits in the model. */
}
BOOL DPLAYX_AnyLobbiesWaitingForConnSettings(void) {return FALSE;}
BOOL DPLAYX_CreateLobbyApplication(DWORD id) {assert(id==launched_id);if(failure==2)return FALSE;assert(!reserved);reserved=1;return TRUE;}
void DPLAYX_ReleaseLobbyApplication(DWORD id) {assert(id==launched_id);reserved=0;memset(&saved_connection,0,sizeof(saved_connection));}
BOOL DPLAYX_SetLobbyHandles(DWORD id,HANDLE start,HANDLE death,HANDLE read) {assert(reserved&&id==launched_id);os_get(start);os_get(death);os_get(read);return failure!=6;}
BOOL DPLAYX_SetLobbyMsgThreadId(DWORD id,DWORD thread) {assert(reserved&&id==launched_id&&thread==99);return failure!=8;}
HRESULT DPLAYX_SetConnectionSettingsA(DWORD flags,DWORD id,const DPLCONNECTION *connection) {
 assert(!flags&&id==launched_id&&reserved);if(failure==3)return DPERR_OUTOFMEMORY;saved_connection=*connection;saved_session=*connection->lpSessionDesc;saved_connection.lpSessionDesc=&saved_session;
 if(connection->lpPlayerName&&connection->lpPlayerName->lpszShortNameA)snprintf(saved_short,sizeof(saved_short),"%s",connection->lpPlayerName->lpszShortNameA);
 assert(connection->dwAddressSize<=sizeof(saved_address));if(connection->dwAddressSize)memcpy(saved_address,connection->lpAddress,connection->dwAddressSize);return DP_OK;
}
DPSESSIONDESC2 *DP_DuplicateSessionDesc(const DPSESSIONDESC2 *session,BOOL from,BOOL to) {(void)from;(void)to;DPSESSIONDESC2 *copy=malloc(sizeof(*copy));if(copy)*copy=*session;return copy;}
static void finish_worker(void) {
 if(worker_callback){LPTHREAD_START_ROUTINE callback=worker_callback;worker_callback=NULL;callback(worker_argument);worker_argument=NULL;}
 for(unsigned i=0;i<256;i++)if(os[i].live&&os[i].remote)CloseHandle((HANDLE)(ULONG_PTR)(i+4096));
 assert(!reserved&&!os_count);
}
static void launches(IDirectPlayLobbyImpl *lobby) {
 DPSESSIONDESC2 session={.dwSize=sizeof(session),.guidApplication=expected};
 DPNAME player={.dwSize=sizeof(player),.lpszShortNameA="player"};
 char address[]="connection bytes";DPLCONNECTION connection={.dwSize=sizeof(connection),.lpSessionDesc=&session,.lpPlayerName=&player,.lpAddress=address,.dwAddressSize=sizeof(address)};
 DWORD id=0;assert(IDirectPlayLobby3AImpl_RunApplication(&lobby->IDirectPlayLobby3A_iface,0,&id,&connection,NULL)==DP_OK);
 assert(id==launched_id&&reserved&&!strcmp(saved_short,"player")&&!memcmp(address,saved_address,sizeof(address)));finish_worker();
 for(failure=1;failure<=9;failure++) {
  id=77;assert(FAILED(IDirectPlayLobby3AImpl_RunApplication(&lobby->IDirectPlayLobby3A_iface,0,&id,&connection,NULL)));assert(!id);finish_worker();assert(!open_handles);
 }
 failure=0;
 HANDLE notification=CreateEventW(NULL,TRUE,FALSE,NULL);
 assert(IDirectPlayLobby3AImpl_RunApplication(&lobby->IDirectPlayLobby3A_iface,0,&id,&connection,notification)==DP_OK);
 LPTHREAD_START_ROUTINE notify_worker=worker_callback;worker_callback=NULL;notify_worker(worker_argument);worker_argument=NULL;
 assert(objects[os_get(notification)->object].signal);CloseHandle(notification);finish_worker();
 player.lpszShortName=L"wide player";
 assert(IDirectPlayLobby3Impl_RunApplication(&lobby->IDirectPlayLobby3_iface,0,&id,&connection,NULL)==DP_OK);assert(!strcmp(saved_short,"wide player"));finish_worker();
 assert(IDirectPlayLobby3AImpl_RunApplication(&lobby->IDirectPlayLobby3A_iface,1,&id,&connection,NULL)==DPERR_INVALIDPARAMS);
 assert(terminated>=7);
}

int main(void) {
 DPAPPLICATIONDESC app={0};app.dwSize=sizeof(app);app.guidApplication=expected;app.lpszApplicationNameA="LAN test";app.lpszFilenameA="test.exe";app.lpszPathA="C:\\Games\\Test";app.lpszCommandLineA="--lobby";
 assert(DPL_Register(1,&app,TRUE)==DPERR_INVALIDPARAMS);assert(DPL_Register(0,&app,TRUE)==DP_OK);assert(!open_handles);
 assert(equal(keys[2].values[1],L"test.exe")&&equal(keys[2].values[3],L"C:\\Games\\Test"));
 IDirectPlayLobbyImpl lobby={0};assert(DPL_EnumApplications(&lobby,enumerate,(void*)1,0,TRUE)==DP_OK);assert(seen==1&&!open_handles);
 assert(DPL_EnumApplications(&lobby,enumerate,NULL,0,FALSE)==DP_OK);assert(seen==2&&!open_handles);
 launches(&lobby);
 assert(DPL_Unregister(0,&expected)==DP_OK);assert(!open_handles);assert(DPL_Unregister(0,&expected)==DPERR_UNKNOWNAPPLICATION);assert(!open_handles);
 app.lpszApplicationName=L"LAN test";app.lpszFilename=L"test.exe";app.lpszPath=L"C:\\Games\\Test";app.lpszCommandLine=L"--wide";
 assert(DPL_Register(0,&app,FALSE)==DP_OK);assert(!open_handles);
 write_error=1;assert(DPL_Register(0,&app,FALSE)==DPERR_ACCESSDENIED);write_error=0;assert(!open_handles);
 assert(DPL_Unregister(0,&expected)==DP_OK);assert(!open_handles);
 app.lpszApplicationName=L"invalid\\child";assert(DPL_Register(0,&app,FALSE)==DPERR_INVALIDPARAMS);
 assert(DPL_Register(0,NULL,FALSE)==DPERR_INVALIDPARAMS);assert(DPL_Unregister(1,&expected)==DPERR_INVALIDPARAMS);
 puts("PASS patched Wine lobby: A/W registration, fields/GUIDs, enumeration/callback stop, unregistration, A/W RunApplication/settings handoff, worker/remote handle lifetime, nine launch failure stages, and key lifetime");fflush(stdout);return 0;
}
