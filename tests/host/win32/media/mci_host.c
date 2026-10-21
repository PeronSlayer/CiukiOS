/* Shared kernel32/time/device model of the f5-12B MCI host harnesses.
 * Production code under test is linked unchanged; see mci_test.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "mci_host.h"
FILE *trace;
static uint64_t now_frames; /* consumed device frames, simulated */
uint32_t now_ms;
long live_allocations;
int key_down;
unsigned mci_notifications, mci_last_notify;
unsigned mci_identity_fail;
static _Thread_local int depth, in_sleep;
void hf_lock(void);
void hf_unlock(void);
void (*mci_wait_hook)(void);
_Thread_local void (*mci_unlock_hook)(void);
#ifdef MCI_TEST_CD
#include "cd_model.h"
int mci_test_lock_depth(void) { return depth; }
#endif

void out(const char *format, ...) {
  va_list a;
  va_start(a, format);
  vfprintf(trace, format, a);
  va_end(a);
}

/* ---- device transport and mixer singleton ---- */
struct wa_engine engine;
static uint64_t device_consumed;
uint64_t written;
uint32_t digest = 2166136261u;
static int t_open(void *o, uint32_t rate) {
  (void)o;
  out("TRANSPORT open rate=%u\n", rate);
  return 0;
}
static int t_status(void *o, uint64_t *n, uint32_t *g) {
  (void)o;
  *n = device_consumed;
  *g = 1;
  return 0;
}
int16_t *capture;
size_t capture_frames, capture_cap;
static int t_write(void *o, const int16_t *p, uint32_t frames) {
  (void)o;
  if (capture) {
    if (capture_frames + frames > capture_cap) {
      capture_cap = (capture_frames + frames) * 2;
      capture = realloc(capture, capture_cap * 4);
      assert(capture);
    }
    memcpy(capture + capture_frames * 2, p, frames * 4);
    capture_frames += frames;
  }
  for (uint32_t i = 0; i < frames * 2; i++) {
    digest ^= (uint16_t)p[i];
    digest *= 16777619u;
  }
  written += frames;
  return (int)frames;
}
static void t_close(void *o) { (void)o; }
static void t_dispatch(void *o, const struct wa_event *e) {
  (void)o;
  (void)e;
}
void *wa_alloc(uint32_t n) {
  void *p = calloc(1, n);
  if (p)
    live_allocations++;
  return p;
}
void wa_free(void *p) {
  if (p) {
    live_allocations--;
    free(p);
  }
}
uint32_t wa_private_bytes(void) { return 0; }
struct wa_engine *CiukiAudioAcquire(uint32_t rate, uint32_t *error) {
  if (!engine.transport.write) {
    struct wa_transport t = {0, t_open, t_status, t_write, t_close, t_dispatch};
    wa_init(&engine, &t);
  }
  uint32_t r = wa_open(&engine, rate);
  if (error)
    *error = r;
  return r ? 0 : &engine;
}
void CiukiAudioLock(void) {}
void CiukiAudioUnlock(void) {}
void CiukiAudioFocus(uint32_t f) { (void)f; }
struct wa_engine *wa_process_engine(void) { return &engine; }
/* Advance simulated time: the device consumes what the mixer accepted. */
void advance(uint32_t ms) {
  uint32_t rate = engine.opened ? engine.rate : 44100;
  uint64_t target = now_frames + (uint64_t)ms * rate / 1000;
  while (now_frames < target) {
#ifdef MCI_TEST_CD
    cd_tick();
#endif
    uint64_t step = target - now_frames > 256 ? 256 : target - now_frames;
    if (engine.opened) {
      for (int i = 0; i < 3; i++)
        wa_pump(&engine); /* the mixer keeps its two-quantum lead */
      if (device_consumed + step > engine.accepted)
        step = engine.accepted - device_consumed;
      device_consumed += step;
      wa_poll(&engine);
      wa_dispatch(&engine);
      if (!step)
        step = 1;
    }
    now_frames += step;
  }
  now_ms += ms;
}

/* ---- synthesizer boundary (messages traced with their mixer frame) ---- */
uint32_t wa_synth_load(const void *p, uint32_t n) {
  (void)p;
  (void)n;
  return 0;
}
void wa_synth_render(void *p, int32_t *sum, uint32_t n, uint32_t rate) {
  (void)p;
  (void)sum;
  (void)n;
  (void)rate;
}
uint32_t wa_synth_short(uint32_t msg) {
  out("MIDI frame=%llu msg=%06x\n", (unsigned long long)engine.rendered, msg);
  return 0;
}
uint32_t wa_synth_long(const uint8_t *p, uint32_t n) {
  out("MIDI frame=%llu sysex=%u first=%02x last=%02x\n",
      (unsigned long long)engine.rendered, n, p[0], p[n - 1]);
  return 0;
}
void wa_synth_reset(void) {
  out("MIDI frame=%llu reset\n", (unsigned long long)engine.rendered);
}
void wa_synth_volume(uint32_t v) { (void)v; }
uint32_t wa_bank_full(uint32_t a, uint32_t b) {
  (void)a;
  (void)b;
  return 0;
}
void w32_diag(const char *a, const char *b, const char *c) {
  (void)a;
  (void)b;
  (void)c;
}

/* ---- kernel32 model: real files relative to the fixture directory ---- */
#define BANK ((HANDLE)0x7700)
HANDLE CreateFileA(const char *name, DWORD access, DWORD share,
                   SECURITY_ATTRIBUTES *sa, DWORD create, DWORD attr,
                   HANDLE t) {
  (void)access;
  (void)share;
  (void)sa;
  (void)create;
  (void)attr;
  (void)t;
  if (!strncmp(name, "C:\\WINDOWS\\SYSTEM\\Fluid", 23))
    return BANK;
  int fd = hf_open(name);
  return fd < 0 ? K32_INVALID : (HANDLE)(uintptr_t)(fd + 0x1000);
}
BOOL ReadFile(HANDLE h, void *p, DWORD n, DWORD *done, OVERLAPPED *o) {
  (void)o;
  long r = hf_read((int)((uintptr_t)h - 0x1000), p, n);
  *done = r < 0 ? 0 : (DWORD)r;
  return r >= 0;
}
DWORD SetFilePointer(HANDLE h, LONG at, LONG *high, DWORD how) {
  (void)high;
  long r = hf_seek((int)((uintptr_t)h - 0x1000), at, (int)how);
  return r < 0 ? 0xffffffffu : (DWORD)r;
}
DWORD GetFileSize(HANDLE h, DWORD *high) {
  if (high)
    *high = 0;
  if (h == BANK)
    return 4096;
  long n = hf_size((int)((uintptr_t)h - 0x1000));
  return n < 0 ? 0xffffffffu : (DWORD)n;
}
BOOL GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *info) {
  if (mci_identity_fail == 1) return 0;
  memset(info, 0, sizeof(*info));
  if (mci_identity_fail == 2) return 1; /* Successful query, no identity. */
  return !hf_identity((int)((uintptr_t)h - 0x1000), &info->serial,
                      &info->index_high, &info->index_low);
}
BOOL CloseHandle(HANDLE h) {
  if (h != BANK && (uintptr_t)h >= 0x1000 && (uintptr_t)h < 0x2000)
    hf_close((int)((uintptr_t)h - 0x1000));
  return 1;
}
void GlobalMemoryStatus(MEMORYSTATUS *m) { memset(m, 0, sizeof(*m)); }
HANDLE CreateFileMappingA(HANDLE f, SECURITY_ATTRIBUTES *s, DWORD a, DWORD b,
                          DWORD c, const char *n) {
  (void)f;
  (void)s;
  (void)a;
  (void)b;
  (void)c;
  (void)n;
  return (HANDLE)0x7701;
}
#ifndef MCI_TEST_AVI /* the AVI harness links gdi32 test support */
void *MapViewOfFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) {
  (void)h;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  static uint8_t bank[16];
  return bank;
}
#endif
void InitializeCriticalSection(RTL_CRITICAL_SECTION *s) {
  memset(s, 0, sizeof(*s));
}
void EnterCriticalSection(RTL_CRITICAL_SECTION *s) {
  hf_lock();
  s->RecursionCount++;
  depth++;
}
void LeaveCriticalSection(RTL_CRITICAL_SECTION *s) {
  assert(s->RecursionCount > 0);
  s->RecursionCount--;
  depth--;
  hf_unlock();
  if (mci_unlock_hook) mci_unlock_hook();
}
/* Sleep is the only clock: the worker's service pass runs when no MCI lock
 * is held by the sleeping thread (the production worker is a thread). */
void Sleep(DWORD ms) {
  if (in_sleep)
    return;
  in_sleep = 1;
  advance(ms ? ms : 1);
  if (!depth)
    mci_service_pass();
  in_sleep = 0;
}
DWORD GetTickCount(void) { return now_ms; }
HANDLE CreateThread(SECURITY_ATTRIBUTES *s, DWORD stack,
                    DWORD (*fn)(void *), void *p, DWORD f, DWORD *id) {
  (void)s;
  (void)stack;
  (void)fn;
  (void)p;
  (void)f;
  (void)id;
#ifdef MCI_TEST_CD
  if (p) cd_reader_add(p);
#endif
  return (HANDLE)0x4000;
}
BOOL SetThreadPriority(HANDLE h, int p) {
  (void)h;
  (void)p;
  return 1;
}
DWORD GetCurrentThreadId(void) { return 4242; }
HANDLE GetModuleHandleA(const char *n) {
  (void)n;
  return 0;
}
HANDLE LoadLibraryA(const char *n) {
  (void)n;
  return 0;
}
void *GetProcAddress(HANDLE h, const char *n) {
  (void)h;
  (void)n;
  return 0;
}
#ifdef MCI_TEST_AVI
void harness_post(uintptr_t window, uint32_t message, uintptr_t wp,
                  intptr_t lp);
#endif
void mci_host_post(uintptr_t window, uint32_t status, uint32_t id) {
  mci_notifications++; mci_last_notify = status;
#ifdef MCI_TEST_CD
  cd_model.last_notify=status; cd_model.notifications++;
#endif
  static const char *names[] = {"?", "successful", "superseded", "?",
                                "aborted", "?", "?", "?", "failure"};
  out("NOTIFY t=%u hwnd=%lx %s id=%u\n", now_ms, (unsigned long)window,
      status < 9 ? names[status] : "?", id);
#ifdef MCI_TEST_AVI
  harness_post(window, MM_MCINOTIFY, status, id);
#endif
}
int mci_host_key(int key) {
  (void)key;
  if (mci_wait_hook) mci_wait_hook();
  return key_down;
}
struct acm_api {
  void *open, *close, *prepare, *unprepare, *convert;
};
const struct acm_api *mci_host_acm(void) {
  static struct acm_api api;
  api.open = (void *)acmStreamOpen;
  api.close = (void *)acmStreamClose;
  api.prepare = (void *)acmStreamPrepareHeader;
  api.unprepare = (void *)acmStreamUnprepareHeader;
  api.convert = (void *)acmStreamConvert;
  return &api;
}
#ifndef MCI_TEST_AVI
const struct mci_driver *mci_host_avi(const struct mci_host *host) {
  (void)host;
  return 0;
}
#endif

DWORD GetFullPathNameA(const char *name, DWORD size, char *out, char **part) {
  char path[1024];
  size_t n = strlen(name);
  if (n >= sizeof(path)) return 0;
  memcpy(path, name, n + 1);
  for (size_t i = 0; i < n; i++) if (path[i] == '\\') path[i] = '/';
  char result[1024]; size_t used = 0;
  for (char *at = path; *at;) {
    while (*at == '/') at++;
    char *end = at; while (*end && *end != '/') end++;
    size_t len = end - at;
    if (!len) break;
    if (len == 1 && at[0] == '.') {}
    else if (len == 2 && at[0] == '.' && at[1] == '.') {
      while (used && result[--used] != '/') {}
    } else {
      if (used) result[used++] = '/';
      memcpy(result + used, at, len); used += len;
    }
    at = end;
  }
  if (used >= size) return (DWORD)used + 1;
  memcpy(out, result, used); out[used] = 0;
  if (part) *part = 0;
  return (DWORD)used;
}
