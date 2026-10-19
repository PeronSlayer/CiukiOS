/* Multimedia timer quota and wrap-safe deadlines. LGPL-2.1-or-later. */
#include "audio.h"
extern uint32_t W32API GetTickCount(void);
extern struct wa_engine *wa_process_engine(void);
struct timer {
  uint32_t id, deadline, period, flags;
  uintptr_t callback, user;
};
static struct timer timers[WA_TIMERS];
static uint32_t serial;
struct caps {
  uint32_t minimum, maximum;
};
uint32_t W32API timeGetTime(void) { return GetTickCount(); }
uint32_t W32API timeGetDevCaps(struct caps *c, uint32_t n) {
  if (!c || n < sizeof(*c))
    return 97;
  c->minimum = 1;
  c->maximum = 1000000;
  return 0;
}
uint32_t W32API timeBeginPeriod(uint32_t n) {
  return n >= 1 && n <= 1000000 ? 0 : 97;
}
uint32_t W32API timeEndPeriod(uint32_t n) { return timeBeginPeriod(n); }
uint32_t W32API timeGetSystemTime(uint32_t *p, uint32_t n) {
  if (!p || n < 12)
    return 97;
  p[0] = 1;
  p[1] = timeGetTime();
  p[2] = 0;
  return 0;
}
uint32_t W32API timeSetEvent(uint32_t delay, uint32_t resolution, uintptr_t cb,
                             uintptr_t user, uint32_t flags) {
  (void)resolution;
  if (!delay || delay > 1000000 || !cb || (flags & ~0x31u) ||
      (flags & 0x30u) == 0x30u)
    return 0;
  uint32_t error; /* share the same interactive thread, including timer-only
                     clients */
  if (!CiukiAudioAcquire(0, &error))
    return 0;
  CiukiAudioLock();
  unsigned i;
  for (i = 0; i < WA_TIMERS && timers[i].id; i++) {
  }
  if (i == WA_TIMERS) {
    CiukiAudioUnlock();
    return 0;
  }
  if (!++serial)
    ++serial;
  timers[i] =
      (struct timer){serial, timeGetTime() + delay, delay, flags, cb, user};
  uint32_t id = serial;
  CiukiAudioUnlock();
  return id;
}
uint32_t W32API timeKillEvent(uint32_t id) {
  CiukiAudioLock();
  for (unsigned i = 0; i < WA_TIMERS; i++)
    if (timers[i].id == id) {
      timers[i].id = 0;
      CiukiAudioUnlock();
      return 0;
    }
  CiukiAudioUnlock();
  return 97;
}
void wa_timers_tick(uint32_t now) {
  for (unsigned i = 0; i < WA_TIMERS; i++) {
    CiukiAudioLock();
    struct timer t = timers[i];
    if (!t.id || (int32_t)(now - t.deadline) < 0) {
      CiukiAudioUnlock();
      continue;
    }
    if (t.flags &
        1) { /* Coalesce missed ticks, preserving phase. No callback storm. */
      timers[i].deadline += ((now - t.deadline) / t.period + 1) * t.period;
    } else
      timers[i].id = 0;
    struct wa_event v = {
        .callback = {t.callback, t.user,
                     t.flags & 0x30 ? WA_CALLBACK_EVENT : WA_CALLBACK_FUNCTION},
        .handle = t.id,
        .p2 = !!(t.flags & 0x20)};
    extern void wa_deliver(const struct wa_event *);
    CiukiAudioUnlock();
    wa_deliver(&v);
  }
}
