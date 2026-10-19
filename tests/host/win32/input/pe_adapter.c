/* H1 adapter boundaries only. Never shipped in a builtin store or image.
 * Production DirectInput executes as PE32 through the f5-02 Linux loader.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/input_bridge.h>
#include <w32/ntdll.h>
static W32_INPUT_SINK sink;
static void *sink_context;
static struct w32_input_snapshot snapshot;
static UINT lock_depth, pointer_flags, signals;
HANDLE W32API GetProcessHeap(void) {
  return NtCurrentTeb()->ProcessEnvironmentBlock->ProcessHeap;
}
void *W32API HeapAlloc(HANDLE h, DWORD flags, DWORD n) {
  return RtlAllocateHeap(h, flags, n);
}
BOOL W32API HeapFree(HANDLE h, DWORD flags, void *p) {
  return RtlFreeHeap(h, flags, p);
}
BOOL W32API SetEvent(HANDLE h) {
  if (h != (HANDLE)77)
    return 0;
  signals++;
  return 1;
}
DWORD W32API GetCurrentProcessId(void) { return 1; }
void W32API SetLastError(DWORD n) { NtCurrentTeb()->LastErrorValue = n; }
BOOL W32API IsWindow(HWND h) { return h == (HWND)1; }
HWND W32API GetParent(HWND h) {
  (void)h;
  return 0;
}
DWORD W32API GetWindowThreadProcessId(HWND h, DWORD *pid) {
  if (pid)
    *pid = 1;
  return h == (HWND)1 ? 1 : 0;
}
int W32API GetKeyNameTextA(LONG lp, char *name, int count) {
  (void)lp;
  if (count > 0)
    name[0] = 0;
  return 0;
}
void W32API CiukiUserInputLock(void) { lock_depth++; }
void W32API CiukiUserInputUnlock(void) { lock_depth--; }
void W32API CiukiUserInputPump(void) {}
BOOL W32API CiukiUserInputSubscribe(W32_INPUT_SINK fn, void *ctx, BOOL add) {
  sink = add ? fn : 0;
  sink_context = add ? ctx : 0;
  return 1;
}
void W32API CiukiUserInputSnapshot(struct w32_input_snapshot *out) {
  *out = snapshot;
}
BOOL W32API CiukiUserInputPointer(HWND h, UINT flags) {
  (void)h;
  pointer_flags = flags;
  return 1;
}
DWORD W32API AdapterInput(DWORD kind, LONG x, LONG y, DWORD stamp) {
  if (kind == 10) {
    snapshot.foreground = x ? (HWND)1 : 0;
    if (sink)
      sink(sink_context, snapshot.foreground, 0, 3);
  }
  if (kind == 11) {
    snapshot.cursor = (POINT){x, y};
  }
  if (kind == 1 || kind == 2 || kind == 3) {
    struct ciuki_input_event e = {.type = kind == 1   ? CIUKI_INPUT_MOTION
                                          : kind == 2 ? CIUKI_INPUT_KEY
                                                      : CIUKI_INPUT_WHEEL,
                                  .code = kind == 2 ? 0x1e : 0,
                                  .value = x,
                                  .value2 = y,
                                  .monotonic_ns = (uint64_t)stamp * 1000000};
    if (kind == 2)
      snapshot.keys[0x1e] = x ? 128 : 0;
    if (sink)
      sink(sink_context, snapshot.foreground, &e, kind == 1 ? 1 : 0);
  }
  return kind == 12   ? pointer_flags
         : kind == 13 ? signals
         : kind == 14 ? lock_depth
                      : 0;
}

LONG W32API GetWindowLongA(HWND window, int index) {
  (void)window;
  (void)index;
  return WS_VISIBLE;
}
