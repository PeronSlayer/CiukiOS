/* One process stream and interactive worker, owned by winmm.dll.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "audio.h"
#include <ciuki/abi.h>
#include <w32/kernel32.h>
extern void W32API Sleep(DWORD);
extern HANDLE W32API CreateThread(SECURITY_ATTRIBUTES *, DWORD,
                                  DWORD(W32API *)(void *), void *, DWORD,
                                  DWORD *);
extern BOOL W32API SetThreadPriority(HANDLE, int);
extern DWORD W32API ResumeThread(HANDLE);
extern BOOL W32API SetEvent(HANDLE);
extern BOOL W32API PulseEvent(HANDLE);
extern HANDLE W32API GetModuleHandleA(const char *);
extern HANDLE W32API LoadLibraryA(const char *);
extern void *W32API GetProcAddress(HANDLE, const char *);
extern void W32API InitializeCriticalSection(RTL_CRITICAL_SECTION *);
extern void W32API EnterCriticalSection(RTL_CRITICAL_SECTION *);
extern void W32API LeaveCriticalSection(RTL_CRITICAL_SECTION *);
static struct wa_engine engine;
static RTL_CRITICAL_SECTION guard;
static volatile uint32_t initialized;
/* Account the worker stack and bounded static/heap bookkeeping too. */
#define WA_RUNTIME_RESERVE (128u * 1024u)
static uint32_t private_bytes;
static int audio_fd = -1;
static HANDLE worker;
static volatile uint32_t quitting;
static volatile uint32_t module_pinned;
extern void wa_timers_tick(uint32_t);
static int native(uint32_t n, uintptr_t a, uintptr_t b, uintptr_t c,
                  uintptr_t d) {
#ifdef W32_HOST_MODEL
  extern int wa_native_model(uint32_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t);
  return wa_native_model(n, a, b, c, d);
#else
  int r;
  __asm__ volatile("int $0x80"
                   : "=a"(r)
                   : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
                   : "memory", "cc");
  return r;
#endif
}
void *wa_alloc(uint32_t n) {
  if (n > WA_PRIVATE_LIMIT - sizeof(engine) - sizeof(guard) -
              WA_RUNTIME_RESERVE - private_bytes ||
      n > UINT32_MAX - 16)
    return 0;
  uint32_t *p = HeapAlloc(GetProcessHeap(), 8, n + 16);
  if (!p)
    return 0;
  p[0] = n;
  private_bytes += n;
  return p + 4;
}
void wa_free(void *v) {
  if (v) {
    uint32_t *p = (uint32_t *)v - 4;
    private_bytes -= p[0];
    HeapFree(GetProcessHeap(), 0, p);
  }
}
uint32_t wa_private_bytes(void) {
  return private_bytes + sizeof(engine) + sizeof(guard) + WA_RUNTIME_RESERVE;
}
static int dev_open(void *o, uint32_t rate) {
  (void)o;
  audio_fd = native(CIUKI_SYS_OPEN, (uintptr_t)"/dev/audio",
                    O_WRONLY | O_NONBLOCK, 0, 0);
  if (audio_fd < 0)
    return audio_fd;
  struct ciuki_audio_format f = {.version = 1,
                                 .size = sizeof(f),
                                 .rate = rate,
                                 .format = CIUKI_AUDIO_S16LE,
                                 .channels = 2};
  int r = native(CIUKI_SYS_DEVCTL, audio_fd, AUDIO_SET_FORMAT, (uintptr_t)&f,
                 sizeof(f));
  if (r < 0) {
    native(CIUKI_SYS_CLOSE, audio_fd, 0, 0, 0);
    audio_fd = -1;
  }
  return r;
}
static int dev_status(void *o, uint64_t *n, uint32_t *g) {
  (void)o;
  struct ciuki_audio_status s = {.version = 1, .size = sizeof(s)};
  int r = native(CIUKI_SYS_DEVCTL, audio_fd, AUDIO_GET_STATUS, (uintptr_t)&s,
                 sizeof(s));
  if (r < 0)
    return r;
  *n = s.consumed_frames;
  *g = s.stream_generation;
  return 0;
}
static int dev_write(void *o, const int16_t *p, uint32_t frames) {
  (void)o;
  int n = native(CIUKI_SYS_WRITE, audio_fd, (uintptr_t)p, frames * 4, 0);
  return n < 0 ? n : n / 4;
}
static void dev_close(void *o) {
  (void)o;
  if (audio_fd >= 0)
    native(CIUKI_SYS_CLOSE, audio_fd, 0, 0, 0);
  audio_fd = -1;
}
void wa_deliver(const struct wa_event *e) {
  uint32_t kind = e->callback.kind & 0x70000;
  uintptr_t t = e->callback.target;
  if (!t)
    return;
  if (kind == WA_CALLBACK_EVENT) {
    if (e->p2)
      PulseEvent((HANDLE)t);
    else
      SetEvent((HANDLE)t);
  } else if (kind == WA_CALLBACK_WINDOW) {
    typedef BOOL(W32API * post)(HANDLE, DWORD, uintptr_t, intptr_t);
    post fn =
        (post)GetProcAddress(GetModuleHandleA("user32.dll"), "PostMessageA");
    if (fn)
      fn((HANDLE)t, e->message, e->handle, (intptr_t)e->p1);
  } else if (kind == WA_CALLBACK_THREAD) {
    struct ciuki_w32_message_args a = {.action = W32_MSG_POST, .tid = (DWORD)t};
    a.header = K32_HEADER(W32_MESSAGE, a);
    a.message.message = e->message;
    a.message.wparam = (DWORD)e->handle;
    a.message.lparam = (DWORD)e->p1;
    w32_ciuki_call(&a, sizeof(a));
  } else if (kind == WA_CALLBACK_FUNCTION) {
    typedef void(W32API * fn)(uintptr_t, uint32_t, uintptr_t, uintptr_t,
                              uintptr_t);
    ((fn)t)(e->handle, e->message, e->callback.instance, e->p1, e->p2);
  }
}
static void dispatch(void *o, const struct wa_event *e) {
  (void)o; /* release around foreign code; callbacks may reenter */
  LeaveCriticalSection(&guard);
  wa_deliver(e);
  EnterCriticalSection(&guard);
}
static void init(void) {
  uint32_t expected = 0;
  if (__atomic_compare_exchange_n(&initialized, &expected, 1, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    InitializeCriticalSection(&guard);
    struct wa_transport t = {0,         dev_open,  dev_status,
                             dev_write, dev_close, dispatch};
    wa_init(&engine, &t);
    __atomic_store_n(&initialized, 2, __ATOMIC_RELEASE);
  } else
    while (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) != 2)
      Sleep(0);
}
void W32API CiukiAudioLock(void) {
  init();
  EnterCriticalSection(&guard);
}
void W32API CiukiAudioUnlock(void) { LeaveCriticalSection(&guard); }
static DWORD W32API mixer_thread(void *p) {
  (void)p;
  while (!quitting) {
    CiukiAudioLock();
    uint32_t r = wa_pump(&engine);
    wa_dispatch(&engine);
    CiukiAudioUnlock();
    wa_timers_tick(GetTickCount());
    if (r)
      w32_diag("audio", "winmm.dll", "transport-error");
    Sleep(1);
  }
  return 0;
}
/* The worker's code/data must outlive application FreeLibrary calls. The
 * retained loader reference is released by process teardown, with the stream.
 * Acquire it outside the audio lock to preserve loader/audio lock ordering. */
static int pin_runtime(void) {
  uint32_t expected = 0;
  if (__atomic_compare_exchange_n(&module_pinned, &expected, 1, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    int ok = LoadLibraryA("winmm.dll") != 0;
    __atomic_store_n(&module_pinned, ok ? 2 : 0, __ATOMIC_RELEASE);
    return ok;
  }
  while (__atomic_load_n(&module_pinned, __ATOMIC_ACQUIRE) == 1)
    Sleep(0);
  return __atomic_load_n(&module_pinned, __ATOMIC_ACQUIRE) == 2;
}
struct wa_engine *W32API CiukiAudioAcquire(uint32_t rate, uint32_t *error) {
  if (!pin_runtime()) {
    if (error)
      *error = WA_ALLOCATED;
    return 0;
  }
  CiukiAudioLock();
  uint32_t r = rate ? wa_open(&engine, rate) : 0;
  if (!r && !worker) {
    worker = CreateThread(0, 65536, mixer_thread, 0, 4, 0);
    if (!worker || !SetThreadPriority(worker, 15)) {
      if (worker) {
        extern BOOL W32API TerminateThread(HANDLE, DWORD);
        TerminateThread(worker, 0);
        CloseHandle(worker);
        worker = 0;
      }
      wa_close(&engine);
      r = WA_ALLOCATED;
    } else
      ResumeThread(worker);
  }
  CiukiAudioUnlock();
  if (error)
    *error = r;
  return r ? 0 : &engine;
}
void W32API CiukiAudioFocus(uint32_t focused) {
  CiukiAudioLock();
  engine.focused = !!focused;
  CiukiAudioUnlock();
}
struct wa_engine *wa_process_engine(void) {
  init();
  return &engine;
}
BOOL W32API AudioDllMain(void *module, DWORD why, void *reserved) {
  (void)module;
  (void)reserved;
  if (!why) {
    quitting = 1;
    if (initialized == 2)
      wa_close(&engine); /* OS teardown closes worker/native descriptors even on
                            forced exit. */
  }
  return 1;
}
void wa_master(uint32_t out[2]) {
  struct ciuki_mixer_master m = {.version = 1, .size = sizeof(m)};
  int fd = native(CIUKI_SYS_OPEN, (uintptr_t)"/dev/mixer", O_RDONLY, 0, 0);
  int r = fd < 0 ? fd
                 : native(CIUKI_SYS_DEVCTL, fd, MIXER_GET_MASTER, (uintptr_t)&m,
                          sizeof(m));
  if (fd >= 0)
    native(CIUKI_SYS_CLOSE, fd, 0, 0, 0);
  out[0] = r < 0 ? 0 : m.left * 65535 / 100;
  out[1] = r < 0 ? 0 : m.right * 65535 / 100;
}
