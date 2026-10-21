/* CiukiOS installer shell boundary. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_SHELL_H
#define CIUKI_W32_SHELL_H
#include <w32/kernel32.h>
typedef LONG HRESULT;
typedef struct { DWORD a; uint16_t b,c; uint8_t d[8]; } SHELL_GUID;
#pragma pack(push,1)
typedef struct {
  HANDLE window; DWORD operation; const char *from,*to; uint16_t flags;
  BOOL aborted; void *mappings; const char *title;
} SHELL_FILEOP;
typedef struct {
  DWORD size,mask; HANDLE window; const char *verb,*file,*parameters,*directory;
  int show; HANDLE instance; void *idlist; const char *class_name;
  HANDLE class_key; DWORD hotkey; HANDLE icon,process;
} SHELL_EXECUTE;
#pragma pack(pop)
struct shell_startup {
  DWORD size; char *reserved,*desktop,*title;
  DWORD x,y,width,height,chars_x,chars_y,fill,flags;
  uint16_t show,reserved_bytes; uint8_t *reserved2; HANDLE input,output,error;
};
struct shell_process { HANDLE process,thread; DWORD pid,tid; };
BOOL W32API CreateProcessA(const char *,char *,SECURITY_ATTRIBUTES *,SECURITY_ATTRIBUTES *,BOOL,DWORD,void *,const char *,struct shell_startup *,struct shell_process *);
BOOL W32API CreateDirectoryA(const char *,SECURITY_ATTRIBUTES *);
BOOL W32API RemoveDirectoryA(const char *);
BOOL W32API MoveFileA(const char *,const char *);
BOOL W32API CopyFileA(const char *,const char *,BOOL);
BOOL W32API FindClose(HANDLE);
HANDLE W32API LoadLibraryExA(const char *,HANDLE,DWORD);
BOOL W32API FreeLibrary(HANDLE);
void *W32API GetProcAddress(HANDLE,const char *);
HANDLE W32API FindResourceA(HANDLE,const void *,const void *);
HANDLE W32API LoadResource(HANDLE,HANDLE);
DWORD W32API SizeofResource(HANDLE,HANDLE);
int W32API MessageBoxA(HANDLE,const char *,const char *,DWORD);
void w32_diag(const char *,const char *,const char *);
int shell_copy(char *,DWORD,const char *);
int shell_join(char *,const char *,const char *);
int shell_mkdirs(const char *);
int shell_ansi(const WCHAR *,char *,DWORD);
int shell_wide(const char *,WCHAR *,DWORD);
int shell_utf8_encode(const WCHAR *,char *,DWORD);
int shell_utf8_decode(const char *,WCHAR *,DWORD);
int shell_guid(const SHELL_GUID *,DWORD);
void *shell_alloc(DWORD);
void shell_free(void *);
BOOL W32API SHGetSpecialFolderPathA(HANDLE,char *,int,BOOL);
HRESULT W32API SHGetSpecialFolderLocation(HANDLE,int,void **);
BOOL W32API SHGetPathFromIDListA(const void *,char *);
BOOL W32API SHGetPathFromIDListW(const void *,WCHAR *);
HRESULT W32API SHGetFolderPathA(HANDLE,int,HANDLE,DWORD,char *);
int W32API SHFileOperationA(SHELL_FILEOP *);
BOOL W32API ShellExecuteExA(SHELL_EXECUTE *);
HRESULT W32API DllGetClassObject(const SHELL_GUID *,const SHELL_GUID *,void **);
struct shell_progress;
struct shell_progress *shell_progress_begin(const SHELL_FILEOP *);
int shell_progress_tick(struct shell_progress *,const char *);
void shell_progress_end(struct shell_progress *);
#endif
