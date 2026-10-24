/* Production facade, worker creation and native transport over copied mocks. */
#include "../../../../sdk/win32/audio/runtime.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t now, opened, closed, thread_created, priority, callback_count;
static uint64_t native_consumed, native_accepted;
static int unavailable;
static unsigned capture_available=1, capture_running, capture_frames, capture_reads;
static int capture_failure, reservation_failure;
HANDLE W32API GetProcessHeap(void) { return (HANDLE)1; }
void *W32API HeapAlloc(HANDLE h, DWORD flags, DWORD n) {
  (void)h;
  (void)flags;
  return calloc(1, n);
}
BOOL W32API HeapFree(HANDLE h, DWORD flags, void *p) {
  (void)h;
  (void)flags;
  free(p);
  return 1;
}
void W32API InitializeCriticalSection(RTL_CRITICAL_SECTION *s) {
  memset(s, 0, sizeof(*s));
}
void W32API EnterCriticalSection(RTL_CRITICAL_SECTION *s) {
  s->RecursionCount++;
}
void W32API LeaveCriticalSection(RTL_CRITICAL_SECTION *s) {
  assert(s->RecursionCount);
  s->RecursionCount--;
}
void W32API Sleep(DWORD n) { now += n; }
DWORD W32API GetTickCount(void) { return now; }
HANDLE W32API CreateThread(SECURITY_ATTRIBUTES *s, DWORD stack,
                           DWORD(W32API *fn)(void *), void *arg, DWORD flags,
                           DWORD *id) {
  (void)s;
  (void)arg;
  (void)id;
  assert(stack == 65536 && fn && flags == 4);
  thread_created++;
  return (HANDLE)9;
}
BOOL W32API SetThreadPriority(HANDLE h, int p) {
  assert(h == (HANDLE)9);
  priority = (uint32_t)p;
  return 1;
}
DWORD W32API ResumeThread(HANDLE h) {
  assert(h == (HANDLE)9);
  return 1;
}
BOOL W32API TerminateThread(HANDLE h, DWORD code) {
  (void)h;
  (void)code;
  return 1;
}
BOOL W32API CloseHandle(HANDLE h) {
  (void)h;
  return 1;
}
BOOL W32API SetEvent(HANDLE h) {
  assert(h);
  callback_count++;
  return 1;
}
BOOL W32API PulseEvent(HANDLE h) { return SetEvent(h); }
HANDLE W32API LoadLibraryA(const char *n) {
  assert(!strcmp(n, "winmm.dll"));
  return (HANDLE)11;
}
HANDLE W32API GetModuleHandleA(const char *n) {
  (void)n;
  return 0;
}
void *W32API GetProcAddress(HANDLE h, const char *n) {
  (void)h;
  (void)n;
  return 0;
}
void w32_diag(const char *a, const char *b, const char *c) {
  (void)a;
  (void)b;
  (void)c;
}
int32_t w32_ciuki_call(void *p, uint32_t n) {
  (void)p;
  (void)n;
  return 0;
}
int wa_native_model(uint32_t n, uintptr_t a, uintptr_t b, uintptr_t c,
                    uintptr_t d) {
  if (n == CIUKI_SYS_OPEN) {
    assert(!strcmp((const char *)a, "/dev/audio"));
    if (b == (O_RDONLY | O_NONBLOCK)) return capture_available ? 18 : -19;
    assert(b == (O_WRONLY | O_NONBLOCK));
    if (unavailable)
      return -12;
    opened++;
    return 17;
  }
  if (n == CIUKI_SYS_CLOSE) {
    if (a == 18) return 0;
    assert(a == 17);
    closed++;
    return 0;
  }
  if (n == CIUKI_SYS_DEVCTL) {
    if (a == 18) {
      if (b == AUDIO_CAPTURE_READ) {
        struct ciuki_audio_capture_data *p=(void *)c;
        assert(d==sizeof(*p) && p->frames==128 && capture_running);
        if (capture_failure) return capture_failure;
        if (!capture_frames) return -EAGAIN;
        p->captured=capture_frames<128 ? capture_frames : 128;
        capture_frames-=p->captured; capture_reads++;
        for (unsigned i=0;i<p->captured;i++) { p->pcm[i*2]=8192; p->pcm[i*2+1]=-4096; }
        return 0;
      }
      struct ciuki_audio_capture *p=(void *)c;
      assert(d==sizeof(*p));
      if (b==AUDIO_CAPTURE_ACQUIRE) return reservation_failure;
      if (b==AUDIO_CAPTURE_CAPS) p->available=capture_available;
      else if (b==AUDIO_CAPTURE_START) { assert(p->gain==100 && p->rate==48000); capture_running=1; }
      else if (b==AUDIO_CAPTURE_STOP) capture_running=0;
      else assert(0);
      return 0;
    }
    assert(a == 17);
    if (b == AUDIO_SET_FORMAT) {
      struct ciuki_audio_format *f = (void *)c;
      assert(d == sizeof(*f) && f->version == 1 && f->channels == 2 &&
             f->format == 2);
      return 0;
    }
    if (b==AUDIO_SET_SOURCE) {
      struct ciuki_audio_route *p=(void *)c;
      assert(d==sizeof(*p) && p->source==CIUKI_AUDIO_MIXED); return 0;
    }
    assert(b == AUDIO_GET_STATUS);
    struct ciuki_audio_status *s = (void *)c;
    assert(d == sizeof(*s));
    s->consumed_frames = native_consumed;
    s->stream_generation = 1;
    return 0;
  }
  assert(n == CIUKI_SYS_WRITE && a == 17 && c % 4 == 0);
  native_accepted += c / 4;
  assert(native_accepted - native_consumed <= 512);
  return (int)c;
}
extern uint32_t W32API waveOutOpen(uintptr_t *, uint32_t,
                                   const struct wa_format *, uintptr_t,
                                   uintptr_t, uint32_t);
extern uint32_t W32API waveOutPrepareHeader(uintptr_t, struct wa_header *,
                                            uint32_t);
extern uint32_t W32API waveOutWrite(uintptr_t, struct wa_header *, uint32_t);
extern uint32_t W32API waveOutReset(uintptr_t);
extern uint32_t W32API waveOutClose(uintptr_t);
extern uint32_t W32API timeSetEvent(uint32_t, uint32_t, uintptr_t, uintptr_t,
                                    uint32_t);
extern uint32_t W32API timeKillEvent(uint32_t);
void wa_midi_tick(void) {}
extern uint32_t W32API waveInGetNumDevs(void);
extern uint32_t W32API waveInOpen(uintptr_t *,uint32_t,const struct wa_format *,uintptr_t,uintptr_t,uint32_t);
extern uint32_t W32API waveInPrepareHeader(uintptr_t,struct wa_header *,uint32_t);
extern uint32_t W32API waveInUnprepareHeader(uintptr_t,struct wa_header *,uint32_t);
extern uint32_t W32API waveInAddBuffer(uintptr_t,struct wa_header *,uint32_t);
extern uint32_t W32API waveInStart(uintptr_t);
extern uint32_t W32API waveInStop(uintptr_t);
extern uint32_t W32API waveInReset(uintptr_t);
extern uint32_t W32API waveInClose(uintptr_t);
extern uint32_t W32API waveInGetID(uintptr_t,uint32_t *);
extern uint32_t W32API waveInGetErrorTextA(uint32_t,char *,uint32_t);
static void test_input(void) {
  struct wa_format f={1,1,24000,24000,1,8,0};
  uintptr_t h=0,other=0;
  assert(waveInGetNumDevs()==1);
  assert(!waveInOpen(0,UINT32_MAX,&f,0,0,1) && !capture_running);
  capture_available=0; assert(!waveInGetNumDevs()); capture_available=1;
  reservation_failure=-EBUSY;
  assert(waveInOpen(&h,0,&f,0,0,0)==4 && !h);
  reservation_failure=0;
  assert(!waveInOpen(&h,UINT32_MAX,&f,123,0,WA_CALLBACK_EVENT) && h);
  assert(waveInOpen(&other,0,&f,0,0,0)==4 && !other);
  uint32_t id=99; char error_text[16];
  assert(!waveInGetID(h,&id) && !id && waveInGetID(h,0)==11);
  assert(!waveInGetErrorTextA(4,error_text,sizeof(error_text)) && !error_text[15]);
  assert(waveInGetErrorTextA(12345,error_text,sizeof(error_text))==9);
  char pcm[100]={0};
  struct wa_header a={.data=pcm,.bytes=sizeof(pcm)}, b={.data=pcm,.bytes=20};
  assert(waveInAddBuffer(h,&a,sizeof(a))==34);
  assert(!waveInPrepareHeader(h,&a,sizeof(a)) && !waveInPrepareHeader(h,&b,sizeof(b)));
  assert(!waveInAddBuffer(h,&a,sizeof(a)));
  assert(waveInAddBuffer(h,&a,sizeof(a))==33 && waveInClose(h)==33);
  assert(waveInUnprepareHeader(h,&a,sizeof(a))==33);
  assert(!waveInStart(h) && capture_running);
  capture_frames=200; CiukiAudioLock(); wa_input_tick(); wa_dispatch(&engine); CiukiAudioUnlock();
  assert(a.recorded==100 && (a.flags&WA_DONE) && !(a.flags&WA_QUEUED));
  for (unsigned i=0;i<100;i++) assert((unsigned char)pcm[i]==136); /* stereo average, 8-bit */
  assert(capture_reads==2);
  assert(!waveInAddBuffer(h,&a,sizeof(a)) && !waveInAddBuffer(h,&b,sizeof(b)));
  capture_frames=20; assert(!waveInStop(h));
  assert(a.recorded==10 && (a.flags&WA_DONE) && (b.flags&WA_QUEUED));
  assert(!capture_running && !waveInReset(h) && (b.flags&WA_DONE) && !b.recorded);
  CiukiAudioLock(); wa_dispatch(&engine); CiukiAudioUnlock();
  assert(!waveInUnprepareHeader(h,&a,sizeof(a)) && !waveInUnprepareHeader(h,&b,sizeof(b)));
  assert(!waveInStart(h)); capture_failure=-EIO;
  CiukiAudioLock(); wa_input_tick(); CiukiAudioUnlock();
  assert(!capture_running && waveInStart(h)==6);
  capture_failure=0; assert(!waveInClose(h) && waveInClose(h)==5);
  CiukiAudioLock(); wa_dispatch(&engine); CiukiAudioUnlock();
  assert(!private_bytes);
  puts("PASS waveIn query/caps/ownership/PCM/queue/stop/reset/error/callbacks/cleanup");
}
int main(void) {
  struct wa_format f = {1, 1, 44100, 88200, 2, 16, 0};
  uintptr_t wave;
  assert(!waveOutOpen(0, 0, &f, 0, 0, 1) && !opened);
  unavailable = 1;
  assert(waveOutOpen(&wave, 0, &f, 0, 0, 0) == 4 && !wave && !thread_created);
  unavailable = 0;
  assert(!waveOutOpen(&wave, 0, &f, 123, 0, WA_CALLBACK_EVENT));
  assert(opened == 1 && thread_created == 1 && priority == 15);
  uintptr_t other;
  assert(!waveOutOpen(&other, 0, &f, 0, 0, 0) && opened == 1);
  int16_t pcm[20] = {1, 2, 3};
  struct wa_header h = {.data = (void *)pcm, .bytes = sizeof(pcm)};
  assert(!waveOutPrepareHeader(wave, &h, sizeof(h)));
  assert(!waveOutWrite(wave, &h, sizeof(h)));
  assert(waveOutClose(wave) == 33);
  CiukiAudioLock();
  wa_pump(&engine);
  wa_dispatch(&engine);
  CiukiAudioUnlock();
  assert(callback_count == 1 && !(h.flags & WA_DONE));
  native_consumed = 20;
  CiukiAudioLock();
  wa_pump(&engine);
  wa_dispatch(&engine);
  CiukiAudioUnlock();
  assert((h.flags & WA_DONE) && callback_count == 2);
  assert(!waveOutClose(wave) && !waveOutClose(other));
  uint32_t timers[16];
  now = UINT32_MAX - 3;
  for (unsigned i = 0; i < 16; i++) {
    timers[i] = timeSetEvent(5, 0, 456, 0, 0x11);
    assert(timers[i]);
  }
  assert(!timeSetEvent(5, 0, 456, 0, 0x11));
  wa_timers_tick(0);
  assert(callback_count == 2);
  wa_timers_tick(2);
  assert(callback_count == 18);
  wa_timers_tick(52);
  assert(callback_count == 34);
  for (unsigned i = 0; i < 16; i++)
    assert(!timeKillEvent(timers[i]));
  wa_timers_tick(60);
  assert(callback_count == 34);
  assert(opened == 1 && thread_created == 1);
  wa_close(&engine);
  assert(closed == 1);
  test_input();
  printf("PASS native audio "
         "singleton/format/ENOMEM/priority/queue/callbacks/wave/timers/wrap; "
         "private=%u\n",
         wa_private_bytes());
  return 0;
}
