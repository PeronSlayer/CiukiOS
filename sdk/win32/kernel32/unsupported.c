/* Typed W21 failures for NT 5.1+ services the reused Wine 10.0 DLLs probe
 * (f5-20A, decision table rules w21-actctx, w21-wow64, w21-thread-
 * description). Each records one diagnostic per export through the shared
 * ntdll helper, sets ERROR_CALL_NOT_IMPLEMENTED and returns the documented
 * failure value; outputs are never written. The callers treat the failure
 * as "no activation context", "not WOW64" and "no description".
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#include <w32/shims.h>
#define FAIL(name, convention)                                                 \
  static volatile uint32_t once;                                               \
  return w32_shim_stub(&once, "kernel32.dll", name, convention)
BOOL W32API ActivateActCtx(HANDLE context, uintptr_t *cookie) {
  (void)context;
  (void)cookie;
  FAIL("ActivateActCtx", 0);
}
HANDLE W32API CreateActCtxW(const void *description) {
  (void)description;
  static volatile uint32_t once;
  w32_shim_stub(&once, "kernel32.dll", "CreateActCtxW", 4);
  return K32_INVALID;
}
BOOL W32API DeactivateActCtx(DWORD flags, uintptr_t cookie) {
  (void)flags;
  (void)cookie;
  FAIL("DeactivateActCtx", 0);
}
BOOL W32API FindActCtxSectionGuid(DWORD flags, const void *extension, ULONG section,
                                  const void *guid, void *data) {
  (void)flags;
  (void)extension;
  (void)section;
  (void)guid;
  (void)data;
  FAIL("FindActCtxSectionGuid", 0);
}
BOOL W32API FindActCtxSectionStringW(DWORD flags, const void *extension, ULONG section,
                                     const WCHAR *text, void *data) {
  (void)flags;
  (void)extension;
  (void)section;
  (void)text;
  (void)data;
  FAIL("FindActCtxSectionStringW", 0);
}
BOOL W32API GetCurrentActCtx(HANDLE *context) {
  (void)context;
  FAIL("GetCurrentActCtx", 0);
}
void W32API ReleaseActCtx(HANDLE context) {
  (void)context;
  static volatile uint32_t once;
  w32_shim_stub(&once, "kernel32.dll", "ReleaseActCtx", 0);
}
DWORD W32API GetSystemWow64DirectoryW(WCHAR *out, DWORD cap) {
  (void)out;
  (void)cap;
  FAIL("GetSystemWow64DirectoryW", 0);
}
BOOL W32API Wow64DisableWow64FsRedirection(void **old) {
  (void)old;
  FAIL("Wow64DisableWow64FsRedirection", 0);
}
BOOL W32API Wow64RevertWow64FsRedirection(void *old) {
  (void)old;
  FAIL("Wow64RevertWow64FsRedirection", 0);
}
LONG W32API SetThreadDescription(HANDLE thread, const WCHAR *description) {
  (void)thread;
  (void)description;
  static volatile uint32_t once;
  return (LONG)w32_shim_stub(&once, "kernel32.dll", "SetThreadDescription", 1);
}
