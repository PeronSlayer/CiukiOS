/* WAV convenience playback over the same source registry. LGPL-2.1-or-later. */
#include "audio.h"
#include <w32/kernel32.h>
extern HANDLE wa_file_open(const void *, int);
extern void W32API Sleep(DWORD);
extern void *W32API FindResourceA(void *, const char *, const char *);
extern void *wa_resource_w(void *, const WCHAR *);
extern void *W32API LoadResource(void *, void *);
extern void *W32API LockResource(void *);
extern uint32_t W32API SizeofResource(void *, void *);
static struct wa_source *sound;
static void *owned;
static void purge(struct wa_engine *e) {
  if (sound) {
    wa_stop(e, sound);
    wa_unregister(e, sound);
    wa_free(sound);
    sound = 0;
  }
  wa_free(owned);
  owned = 0;
}
static int parse(const uint8_t *p, uint32_t bytes, struct wa_format *f,
                 const uint8_t **data, uint32_t *size) {
  if (bytes < 12 || w32_u32(p) != 0x46464952 || w32_u32(p + 8) != 0x45564157)
    return 0;
  uint32_t limit = w32_u32(p + 4);
  if (limit > bytes - 8)
    return 0;
  limit += 8;
  int format = 0;
  *data = 0;
  for (uint32_t at = 12; at <= limit && limit - at >= 8;) {
    uint32_t n = w32_u32(p + at + 4), id = w32_u32(p + at);
    if (n > limit - at - 8)
      return 0;
    if (id == 0x20746d66 && n >= 16) {
      w32_fill(f, 0, sizeof(*f));
      w32_copy(f, p + at + 8, 16);
      format = wa_valid_format(f);
    }
    if (id == 0x61746164) {
      *data = p + at + 8;
      *size = n;
    }
    uint32_t step = 8 + n + (n & 1);
    if (step > limit - at)
      break;
    at += step;
  }
  return format && *data && *size && *size % f->align == 0;
}
static int play(const void *name, void *module, uint32_t flags, int wide) {
  if ((flags & 8) && !(flags & 1))
    return 0;
  uint32_t error;
  struct wa_engine *e = CiukiAudioAcquire(44100, &error);
  if (!e)
    return 0;
  CiukiAudioLock();
  if (sound && (sound->playing || sound->end_frame > e->consumed) &&
      (flags & 0x10)) {
    CiukiAudioUnlock();
    return 0;
  }
  purge(e);
  if (!name || (flags & 0x40)) {
    CiukiAudioUnlock();
    return 1;
  }
  const uint8_t *bytes = 0;
  uint32_t length = 0;
  void *allocation = 0;
  if ((flags & 0x40004) == 0x40004) {
    void *r = wide ? wa_resource_w(module, name)
                   : FindResourceA(module, name, "WAVE");
    if (r) {
      length = SizeofResource(module, r);
      bytes = LockResource(LoadResource(module, r));
    }
  } else if (flags & 4) {
    bytes = name;
    length = w32_u32(bytes + 4);
    if (length > 2 * 1024 * 1024 - 8) {
      CiukiAudioUnlock();
      return 0;
    }
    length += 8;
  } else {
    HANDLE file = wa_file_open(name, wide);
    if (file != K32_INVALID) {
      uint32_t hi = 0, got = 0;
      length = GetFileSize(file, &hi);
      if (!hi && length <= 2 * 1024 * 1024) {
        allocation = wa_alloc(length);
        if (allocation && ReadFile(file, allocation, length, &got, 0) &&
            got == length)
          bytes = allocation;
      }
      CloseHandle(file);
    }
  }
  struct wa_format f;
  const uint8_t *data;
  uint32_t n;
  if (!bytes || !parse(bytes, length, &f, &data, &n)) {
    wa_free(allocation);
    CiukiAudioUnlock();
    return 0;
  }
  struct wa_source *s = wa_alloc(sizeof(*s));
  struct wa_samples *m = wa_alloc(sizeof(*m) + n);
  if (!s || !m) {
    wa_free(s);
    wa_free(m);
    wa_free(allocation);
    CiukiAudioUnlock();
    return 0;
  }
  wa_source_init(s, &f);
  m->bytes = n;
  m->refs = 1;
  w32_copy(m->data, data, n);
  wa_free(allocation);
  s->samples = m;
  s->playing = 1;
  s->looping = !!(flags & 8);
  s->flags = WA_GLOBAL;
  if (wa_register(e, s)) {
    wa_free(s);
    wa_free(m);
    CiukiAudioUnlock();
    return 0;
  }
  sound = s;
  owned = m;
  CiukiAudioUnlock();
  if (!(flags & 1)) {
    while (1) {
      CiukiAudioLock();
      int active = sound == s && (s->playing || s->end_frame > e->consumed);
      CiukiAudioUnlock();
      if (!active)
        break;
      Sleep(1);
    }
  }
  return 1;
}
int W32API PlaySoundA(const char *name, void *module, uint32_t flags) {
  return play(name, module, flags, 0);
}
int W32API PlaySoundW(const WCHAR *name, void *module, uint32_t flags) {
  return play(name, module, flags, 1);
}
int W32API sndPlaySoundA(const char *name, uint32_t flags) {
  return play(name, 0, flags, 0);
}
int W32API sndPlaySoundW(const WCHAR *name, uint32_t flags) {
  return play(name, 0, flags, 1);
}
