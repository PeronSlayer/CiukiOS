/* Windows 9x named-Unicode refusals. Census-required environment W APIs stay
 * implemented; these functions never inspect or mutate caller outputs.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#define REFUSE(name, result) do { static volatile ULONG once; \
  if (!__atomic_exchange_n(&once,1,__ATOMIC_RELAXED)) w32_diag("stub","kernel32.dll",name); \
  SetLastError(120); return result; } while (0)
HANDLE W32API CreateFileW(const WCHAR *p, DWORD a, DWORD s, SECURITY_ATTRIBUTES *sa, DWORD d, DWORD f, HANDLE t) {
  (void)p;(void)a;(void)s;(void)sa;(void)d;(void)f;(void)t; REFUSE("CreateFileW",K32_INVALID);
}
BOOL W32API CreateProcessW(const WCHAR *app, WCHAR *cmd, SECURITY_ATTRIBUTES *p, SECURITY_ATTRIBUTES *t,
                           BOOL inherit, DWORD flags, void *env, const WCHAR *dir, void *startup, void *out) {
  (void)app;(void)cmd;(void)p;(void)t;(void)inherit;(void)flags;(void)env;(void)dir;(void)startup;(void)out;
  REFUSE("CreateProcessW",0);
}
HANDLE W32API CreateEventW(SECURITY_ATTRIBUTES *s, BOOL m, BOOL i, const WCHAR *n) {
  (void)s;(void)m;(void)i;(void)n;REFUSE("CreateEventW",0);
}
HANDLE W32API CreateMutexW(SECURITY_ATTRIBUTES *s, BOOL o, const WCHAR *n) {
  (void)s;(void)o;(void)n;REFUSE("CreateMutexW",0);
}
HANDLE W32API LoadLibraryExW(const WCHAR *n, HANDLE f, DWORD flags) {
  (void)n;(void)f;(void)flags;REFUSE("LoadLibraryExW",0);
}
HANDLE W32API LoadLibraryW(const WCHAR *n) { (void)n;REFUSE("LoadLibraryW",0); }
HANDLE W32API GetModuleHandleW(const WCHAR *n) { (void)n;REFUSE("GetModuleHandleW",0); }
DWORD W32API GetModuleFileNameW(HANDLE h, WCHAR *out, DWORD n) {
  (void)h;(void)out;(void)n;REFUSE("GetModuleFileNameW",0);
}
BOOL W32API GetVersionExW(OSVERSIONINFOW *out) { (void)out;REFUSE("GetVersionExW",0); }
DWORD W32API GetEnvironmentVariableW(const WCHAR *n, WCHAR *out, DWORD cap) {
  (void)n;(void)out;(void)cap;REFUSE("GetEnvironmentVariableW",0);
}
WCHAR *W32API GetCommandLineW(void) { REFUSE("GetCommandLineW",0); }
int W32API CompareStringW(DWORD l,DWORD f,const WCHAR *a,int n,const WCHAR *b,int m) {
  (void)l;(void)f;(void)a;(void)n;(void)b;(void)m;REFUSE("CompareStringW",0);
}
int W32API GetLocaleInfoW(DWORD l,DWORD t,WCHAR *out,int n) {
  (void)l;(void)t;(void)out;(void)n;REFUSE("GetLocaleInfoW",0);
}
BOOL W32API GetStringTypeW(DWORD t,const WCHAR *src,int n,uint16_t *out) {
  (void)t;(void)src;(void)n;(void)out;REFUSE("GetStringTypeW",0);
}
int W32API LCMapStringW(DWORD l,DWORD f,const WCHAR *src,int n,WCHAR *out,int cap) {
  (void)l;(void)f;(void)src;(void)n;(void)out;(void)cap;REFUSE("LCMapStringW",0);
}
