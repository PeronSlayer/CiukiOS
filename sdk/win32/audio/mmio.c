/* Bounded read-only RIFF I/O used by WAV/MCI. LGPL-2.1-or-later. */
#include "audio.h"
#include <w32/kernel32.h>
extern HANDLE wa_file_open(const void *, int);
struct mmioinfo {
  uint32_t flags, fourcc;
  uintptr_t proc;
  uint32_t error;
  void *task;
  int32_t buffer_bytes;
  char *buffer, *next, *read_end, *write_end;
  int32_t buffer_offset, disk_offset;
  uint32_t info[3], reserved[2];
  uintptr_t handle;
};
struct chunk {
  uint32_t id, size, type, offset, flags;
};
struct io {
  struct mmioinfo info;
  uint8_t *data;
  uint32_t size, position, owned;
  HANDLE file;
  uint32_t capacity, buffer_start, buffer_valid;
};
static struct io *ios[16];
static struct io *get(uintptr_t h) {
  for (unsigned i = 0; i < 16; i++)
    if ((uintptr_t)ios[i] == h)
      return ios[i];
  return 0;
}
static int read_at(struct io *s, uint32_t offset, void *out, uint32_t n) {
  if (offset > s->size || n > s->size - offset)
    return 0;
  if (!s->owned) {
    w32_copy(out, s->data + offset, n);
    return 1;
  }
  if (SetFilePointer(s->file, (LONG)offset, 0, 0) != offset)
    return 0;
  DWORD done = 0;
  return ReadFile(s->file, out, n, &done, 0) && done == n;
}
static int sync_info(struct io *s) {
  if (!s->owned) {
    s->info.next = (void *)(s->data + s->position);
    return 1;
  }
  if (s->position < s->buffer_start ||
      s->position > s->buffer_start + s->buffer_valid ||
      (s->position == s->buffer_start + s->buffer_valid &&
       s->position < s->size)) {
    uint32_t count = s->size - s->position;
    if (count > s->capacity)
      count = s->capacity;
    if (!read_at(s, s->position, s->data, count))
      return 0;
    s->buffer_start = s->position;
    s->buffer_valid = count;
  }
  s->info.next = (void *)(s->data + s->position - s->buffer_start);
  s->info.read_end = (void *)(s->data + s->buffer_valid);
  s->info.buffer_offset = (int32_t)s->buffer_start;
  s->info.disk_offset = (int32_t)(s->buffer_start + s->buffer_valid);
  return 1;
}
static uintptr_t open_io(const void *name, struct mmioinfo *info,
                         uint32_t flags, int wide) {
  if (flags & 3)
    return 0;
  unsigned i;
  for (i = 0; i < 16 && ios[i]; i++) {
  }
  if (i == 16)
    return 0;
  struct io *s = wa_alloc(sizeof(*s));
  if (!s)
    return 0;
  if (info && info->fourcc == 0x204d454d) {
    if (!info->buffer || info->buffer_bytes < 0) {
      wa_free(s);
      return 0;
    }
    s->data = (void *)info->buffer;
    s->size = (uint32_t)info->buffer_bytes;
    s->info = *info;
    s->info.read_end = (void *)(s->data + s->size);
  } else {
    s->file = wa_file_open(name, wide);
    if (s->file == K32_INVALID) {
      wa_free(s);
      return 0;
    }
    DWORD high = 0;
    s->size = GetFileSize(s->file, &high);
    if (high || s->size > INT32_MAX) {
      CloseHandle(s->file);
      wa_free(s);
      return 0;
    }
    s->capacity = 8192;
    s->data = wa_alloc(s->capacity);
    if (!s->data) {
      CloseHandle(s->file);
      wa_free(s);
      return 0;
    }
    s->owned = 1;
    s->info.buffer_bytes = (int32_t)s->capacity;
  }
  s->info.handle = (uintptr_t)s;
  s->info.buffer = (void *)s->data;
  if (!sync_info(s)) {
    if (s->owned) {
      CloseHandle(s->file);
      wa_free(s->data);
    }
    wa_free(s);
    return 0;
  }
  ios[i] = s;
  if (info)
    *info = s->info;
  return (uintptr_t)s;
}
uintptr_t W32API mmioOpenA(char *name, struct mmioinfo *p, uint32_t f) {
  return open_io(name, p, f, 0);
}
uintptr_t W32API mmioOpenW(WCHAR *name, struct mmioinfo *p, uint32_t f) {
  return open_io(name, p, f, 1);
}
int32_t W32API mmioRead(uintptr_t h, char *out, int32_t n) {
  struct io *s = get(h);
  if (!s || n < 0 || (!out && n))
    return -1;
  uint32_t count = (uint32_t)n;
  if (count > s->size - s->position)
    count = s->size - s->position;
  if (!read_at(s, s->position, out, count))
    return -1;
  s->position += count;
  if (!sync_info(s))
    return -1;
  return (int32_t)count;
}
int32_t W32API mmioSeek(uintptr_t h, int32_t offset, int32_t origin) {
  struct io *s = get(h);
  if (!s || origin < 0 || origin > 2)
    return -1;
  int64_t at = offset + (int64_t)(origin == 1   ? s->position
                                  : origin == 2 ? s->size
                                                : 0);
  if (at < 0 || (uint64_t)at > s->size)
    return -1;
  s->position = (uint32_t)at;
  return sync_info(s) ? (int32_t)at : -1;
}
uint32_t W32API mmioClose(uintptr_t h, uint32_t f) {
  (void)f;
  struct io *s = get(h);
  if (!s)
    return 5;
  for (unsigned i = 0; i < 16; i++)
    if (ios[i] == s)
      ios[i] = 0;
  if (s->owned) {
    CloseHandle(s->file);
    wa_free(s->data);
  }
  wa_free(s);
  return 0;
}
uint32_t W32API mmioGetInfo(uintptr_t h, struct mmioinfo *p, uint32_t f) {
  (void)f;
  struct io *s = get(h);
  if (!s)
    return 5;
  if (!p)
    return 11;
  if (!sync_info(s))
    return 257;
  *p = s->info;
  return 0;
}
uint32_t W32API mmioSetInfo(uintptr_t h, const struct mmioinfo *p, uint32_t f) {
  (void)f;
  struct io *s = get(h);
  if (!s)
    return 5;
  uint32_t valid = s->owned ? s->buffer_valid : s->size;
  if (!p || p->buffer != (char *)s->data ||
      (uintptr_t)p->next < (uintptr_t)s->data ||
      (uintptr_t)p->next > (uintptr_t)s->data + valid)
    return 11;
  s->position =
      (s->owned ? s->buffer_start : 0) + (uint32_t)(p->next - (char *)s->data);
  s->info.next = p->next;
  return 0;
}
uint32_t W32API mmioAdvance(uintptr_t h, struct mmioinfo *p, uint32_t f) {
  if (f)
    return 8;
  struct io *s = get(h);
  if (!s)
    return 5;
  uint32_t r = p ? mmioSetInfo(h, p, 0) : 0;
  if (r)
    return r;
  if (s->owned) {
    s->position = s->buffer_start + s->buffer_valid;
    if (!sync_info(s))
      return 257;
  }
  if (p)
    *p = s->info;
  return 0;
}
uint32_t W32API mmioDescend(uintptr_t h, struct chunk *p,
                            const struct chunk *parent, uint32_t flags) {
  struct io *s = get(h);
  if (!s || !p)
    return 11;
  uint32_t limit = s->size;
  if (parent) {
    if (parent->offset > s->size || parent->size > s->size - parent->offset)
      return 256;
    limit = parent->offset + parent->size;
  }
  uint32_t target = p->id, type = p->type;
  while (s->position <= limit && limit - s->position >= 8) {
    uint8_t header[12];
    uint32_t at = s->position;
    if (!read_at(s, at, header, 8))
      return 257;
    uint32_t id = w32_u32(header), size = w32_u32(header + 4), form = 0;
    if (size > limit - at - 8)
      return 256;
    if (id == 0x46464952 || id == 0x5453494c) {
      if (size < 4)
        return 256;
      if (!read_at(s, at + 8, header + 8, 4))
        return 257;
      form = w32_u32(header + 8);
    }
    int found = !flags || ((flags & 0x10) && id == target) ||
                ((flags & 0x20) && id == 0x46464952 && form == type) ||
                ((flags & 0x40) && id == 0x5453494c && form == type);
    if (found) {
      *p = (struct chunk){id, size, form, at + 8, 0};
      s->position = at + 8 + ((id == 0x46464952 || id == 0x5453494c) ? 4 : 0);
      return sync_info(s) ? 0 : 257;
    }
    uint32_t step = 8 + size + (size & 1);
    if (step > limit - at)
      break;
    s->position += step;
  }
  return 256;
}
uint32_t W32API mmioAscend(uintptr_t h, struct chunk *p, uint32_t f) {
  (void)f;
  struct io *s = get(h);
  if (!s || !p || p->offset > s->size || p->size > s->size - p->offset)
    return 11;
  uint32_t at = p->offset + p->size;
  if ((p->size & 1) && at < s->size)
    at++;
  s->position = at;
  return sync_info(s) ? 0 : 257;
}
uint32_t W32API mmioStringToFOURCCA(const char *p, uint32_t f) {
  if (!p)
    return 0;
  uint32_t v = 0;
  for (unsigned i = 0; i < 4 && p[i]; i++) {
    uint8_t c = (uint8_t)p[i];
    if ((f & 0x10) && c >= 'a' && c <= 'z')
      c -= 32;
    v |= (uint32_t)c << (8 * i);
  }
  return v;
}
uint32_t W32API mmioStringToFOURCCW(const WCHAR *p, uint32_t f) {
  char b[5] = {0};
  if (p)
    for (unsigned i = 0; i < 4 && p[i]; i++)
      b[i] = (char)p[i];
  return mmioStringToFOURCCA(b, f);
}
uint32_t W32API mmioFlush(uintptr_t h, uint32_t f) {
  (void)f;
  return get(h) ? 0 : 5;
}
