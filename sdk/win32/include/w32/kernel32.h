/* Original Win32 facade types. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_KERNEL32_H
#define CIUKI_KERNEL32_H
#include <ciuki/w32_core.h>
#include <w32/ldr.h>
typedef void *HANDLE;
typedef int32_t BOOL;
typedef uint32_t DWORD;
typedef struct {
  DWORD low, high;
} FILETIME;
typedef struct {
  uint16_t year, month, weekday, day, hour, minute, second, milliseconds;
} SYSTEMTIME;
typedef struct {
  DWORD size, major, minor, build, platform;
  char csd[128];
} OSVERSIONINFOA;
typedef struct {
  DWORD size, major, minor, build, platform;
  WCHAR csd[128];
} OSVERSIONINFOW;
typedef struct {OSVERSIONINFOA base;uint16_t service_major,service_minor,suite;uint8_t product,reserved;} OSVERSIONINFOEXA;
typedef struct {OSVERSIONINFOW base;uint16_t service_major,service_minor,suite;uint8_t product,reserved;} OSVERSIONINFOEXW;
typedef struct {
  DWORD length, load, total_phys, avail_phys, total_pagefile, avail_pagefile,
      total_virtual, avail_virtual;
} MEMORYSTATUS;
typedef struct {
  DWORD length, load;
  uint64_t total_phys, avail_phys, total_pagefile, avail_pagefile,
      total_virtual, avail_virtual, avail_extended_virtual;
} MEMORYSTATUSEX;
typedef struct {
  uint16_t architecture, reserved;
  DWORD page_size;
  void *minimum, *maximum;
  uintptr_t active_mask;
  DWORD processors, type, granularity;
  uint16_t level, revision;
} SYSTEM_INFO;
typedef struct {
  DWORD length;
  void *descriptor;
  BOOL inherit;
} SECURITY_ATTRIBUTES;
typedef struct {
  uintptr_t internal, internal_high;
  DWORD offset, offset_high;
  HANDLE event;
} OVERLAPPED;
typedef struct {
  DWORD attributes;
  FILETIME creation, access, write;
  DWORD size_high, size_low, reserved[2];
  char name[260], alternate[14];
} WIN32_FIND_DATAA;
typedef struct {
  LONG bias;
  WCHAR standard_name[32];
  SYSTEMTIME standard_date;
  LONG standard_bias;
  WCHAR daylight_name[32];
  SYSTEMTIME daylight_date;
  LONG daylight_bias;
} TIME_ZONE_INFORMATION;
#define K32_INVALID ((HANDLE)(intptr_t)-1)
#define K32_PTR(p) ((DWORD)(uintptr_t)(p))
#define K32_HEADER(op, a) ((struct ciuki_w32_header){1, sizeof(a), op, 0})
DWORD W32API GetLastError(void);
void W32API SetLastError(DWORD);
int k32_status(int32_t);
int k32_profile_begin(void);
void k32_profile_end(void);
int k32_file(struct ciuki_w32_file_args *);
int k32_path(const char *, char[778]);
/* Private core uses 3-byte UCS2 UTF-8; this facade supports ACP 1252. */
#ifndef FS_PATH_BYTES
#define FS_PATH_BYTES 778u
#endif
void *k32_alloc(DWORD);
void k32_free(void *, DWORD);
HANDLE W32API GetProcessHeap(void);
HANDLE W32API HeapCreate(DWORD, DWORD, DWORD);
BOOL W32API HeapDestroy(HANDLE);
void *W32API HeapAlloc(HANDLE, DWORD, DWORD);
void *W32API HeapReAlloc(HANDLE, DWORD, void *, DWORD);
BOOL W32API HeapFree(HANDLE, DWORD, void *);
DWORD W32API HeapSize(HANDLE, DWORD, const void *);
void *W32API VirtualAlloc(void *, DWORD, DWORD, DWORD);
BOOL W32API VirtualFree(void *, DWORD, DWORD);
BOOL W32API VirtualProtect(void *, DWORD, DWORD, DWORD *);
DWORD W32API VirtualQuery(const void *, struct ciuki_w32_memory_info *, DWORD);
DWORD W32API WaitForMultipleObjectsEx(DWORD, const HANDLE *, BOOL, DWORD, BOOL);
DWORD W32API WaitForSingleObject(HANDLE, DWORD);
BOOL W32API CloseHandle(HANDLE);
HANDLE W32API CreateFileA(const char *, DWORD, DWORD, SECURITY_ATTRIBUTES *,
                          DWORD, DWORD, HANDLE);
BOOL W32API ReadFile(HANDLE, void *, DWORD, DWORD *, OVERLAPPED *);
BOOL W32API WriteFile(HANDLE, const void *, DWORD, DWORD *, OVERLAPPED *);
DWORD W32API SetFilePointer(HANDLE, LONG, LONG *, DWORD);
BOOL W32API SetEndOfFile(HANDLE);
BOOL W32API FlushFileBuffers(HANDLE);
BOOL W32API LockFile(HANDLE, DWORD, DWORD, DWORD, DWORD);
BOOL W32API UnlockFile(HANDLE, DWORD, DWORD, DWORD, DWORD);
BOOL W32API LockFileEx(HANDLE, DWORD, DWORD, DWORD, DWORD, OVERLAPPED *);
BOOL W32API UnlockFileEx(HANDLE, DWORD, DWORD, DWORD, OVERLAPPED *);
BOOL W32API GlobalMemoryStatusEx(MEMORYSTATUSEX *);
BOOL W32API ReadProcessMemory(HANDLE, const void *, void *, DWORD, DWORD *);
BOOL W32API WriteProcessMemory(HANDLE, void *, const void *, DWORD, DWORD *);
BOOL W32API DeleteFileA(const char *);
DWORD W32API GetFileSize(HANDLE, DWORD *);
DWORD W32API GetFileAttributesA(const char *);
BOOL W32API SetFileAttributesA(const char *, DWORD);
DWORD W32API GetTempPathA(DWORD, char *);
DWORD W32API GetEnvironmentVariableA(const char *, char *, DWORD);
DWORD W32API GetCurrentDirectoryA(DWORD, char *);
DWORD W32API GetFullPathNameA(const char *, DWORD, char *, char **);
DWORD W32API GetTickCount(void);
void W32API Sleep(DWORD);
int W32API MultiByteToWideChar(DWORD, DWORD, const char *, int, WCHAR *, int);
int W32API WideCharToMultiByte(DWORD, DWORD, const WCHAR *, int, char *, int,
                               const char *, BOOL *);
DWORD W32API GetPrivateProfileStringA(const char *, const char *, const char *,
                                      char *, DWORD, const char *);
DWORD W32API GetPrivateProfileIntA(const char *, const char *, int,
                                   const char *);
BOOL W32API WritePrivateProfileStringA(const char *, const char *, const char *,
                                       const char *);
DWORD W32API GetPrivateProfileSectionA(const char *, char *, DWORD,
                                       const char *);
DWORD W32API GetPrivateProfileSectionNamesA(char *, DWORD, const char *);
BOOL W32API SystemTimeToFileTime(const SYSTEMTIME *, FILETIME *);
BOOL W32API FileTimeToSystemTime(const FILETIME *, SYSTEMTIME *);
void W32API GetSystemTime(SYSTEMTIME *);
DWORD W32API TlsAlloc(void);
BOOL W32API TlsFree(DWORD);
void *W32API TlsGetValue(DWORD);
BOOL W32API TlsSetValue(DWORD, void *);
void W32API ExitThread(DWORD);
#endif
