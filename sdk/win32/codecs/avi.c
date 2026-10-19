/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Read-only bounded RIFF AVI access. Playback and MCI belong to f5-12B.
 */
#include "codec.h"
#ifndef W32_HOST_MODEL
#define IMPORT __declspec(dllimport)
#else
#define IMPORT
#endif
IMPORT void *W32API GetProcessHeap(void);
IMPORT void *W32API HeapAlloc(void *, uint32_t, uint32_t);
IMPORT int W32API HeapFree(void *, uint32_t, void *);
IMPORT void *W32API CreateFileA(const char *, uint32_t, uint32_t, void *,
                                uint32_t, uint32_t, void *);
IMPORT int W32API WideCharToMultiByte(uint32_t, uint32_t, const WCHAR *, int,
                                      char *, int, const char *, int *);
IMPORT uint32_t W32API GetFileSize(void *, uint32_t *);
IMPORT uint32_t W32API SetFilePointer(void *, int32_t, int32_t *, uint32_t);
IMPORT int W32API ReadFile(void *, void *, uint32_t, uint32_t *, void *);
IMPORT int W32API CloseHandle(void *);
IMPORT uintptr_t W32API ICLocate(uint32_t, uint32_t, struct cc_bitmap *,
                                 struct cc_bitmap *, uint32_t);
IMPORT intptr_t W32API ICSendMessage(uintptr_t, uint32_t, uintptr_t, uintptr_t);
IMPORT int32_t W32API ICClose(uintptr_t);
IMPORT int32_t ICDecompress(uintptr_t, uint32_t, struct cc_bitmap *,
                            const void *, struct cc_bitmap *, void *);
#define AVIERR ((int32_t)0x80044000)
#define VIDE CC_FOUR('v', 'i', 'd', 's')
struct frame {
  uint32_t offset, size;
};
struct stream {
  uint8_t header[56], format[2048];
  uint32_t format_len, count, capacity;
  struct frame *frames;
};
struct file {
  void *handle;
  uint32_t refs, length, streams;
  struct stream stream[4];
};
struct view {
  struct file *file;
  uint32_t stream, refs;
};
struct getter {
  struct view *view;
  uintptr_t ic;
  uint32_t frame_bytes, last;
  struct cc_bitmap *image;
  uint8_t *input;
  uint32_t input_cap;
};
static struct file *files[8];
static struct view *views[32];
static struct getter *getters[16];
static void *alloc(uint32_t n) { return HeapAlloc(GetProcessHeap(), 8, n); }
static void release(void *p) {
  if (p)
    HeapFree(GetProcessHeap(), 0, p);
}
static uint32_t le32(const uint8_t *p) {
  return cc_le16(p) | ((uint32_t)cc_le16(p + 2) << 16);
}
static int readat(struct file *f, uint32_t p, void *out, uint32_t n) {
  uint32_t done = 0;
  if (p > f->length || n > f->length - p ||
      SetFilePointer(f->handle, (int32_t)p, 0, 0) != p)
    return 0;
  return ReadFile(f->handle, out, n, &done, 0) && done == n;
}
static int append(struct stream *s, uint32_t p, uint32_t n) {
  if (s->count == 65536)
    return 0;
  if (s->count == s->capacity) {
    uint32_t cap = s->capacity ? s->capacity * 2 : 64;
    struct frame *frames = alloc(cap * sizeof(*frames));
    if (!frames)
      return 0;
    if (s->frames)
      cc_copy(frames, s->frames, s->count * sizeof(*frames));
    release(s->frames);
    s->frames = frames;
    s->capacity = cap;
  }
  s->frames[s->count++] = (struct frame){p, n};
  return 1;
}
static int scan(struct file *f, uint32_t p, uint32_t end, int index, int movi,
                uint32_t depth, uint32_t *budget) {
  if (depth > 8)
    return 0;
  while (p < end) {
    uint8_t header[12];
    uint32_t id, n, next;
    if (!*budget || end - p < 8 || !readat(f, p, header, 8))
      return 0;
    (*budget)--;
    id = le32(header);
    n = le32(header + 4);
    if (n > end - p - 8)
      return 0;
    next = p + 8 + n;
    if ((n & 1) && next == end)
      return 0;
    next += n & 1;
    if (id == CC_FOUR('L', 'I', 'S', 'T')) {
      uint32_t type;
      int child = index;
      if (n < 4 || !readat(f, p + 8, header, 4))
        return 0;
      type = le32(header);
      if (type == CC_FOUR('s', 't', 'r', 'l')) {
        if (f->streams == 4)
          return 0;
        child = (int)f->streams++;
      }
      if (!scan(f, p + 12, p + 8 + n, child,
                movi || type == CC_FOUR('m', 'o', 'v', 'i'), depth + 1, budget))
        return 0;
    } else if (index >= 0 && id == CC_FOUR('s', 't', 'r', 'h')) {
      if (n < 56 || !readat(f, p + 8, f->stream[index].header, 56))
        return 0;
    } else if (index >= 0 && id == CC_FOUR('s', 't', 'r', 'f')) {
      if (n > 2048 || !readat(f, p + 8, f->stream[index].format, n))
        return 0;
      f->stream[index].format_len = n;
    } else if (movi && header[0] >= '0' && header[0] <= '9' &&
               header[1] >= '0' && header[1] <= '9') {
      uint32_t si = (header[0] - '0') * 10 + header[1] - '0';
      if (si >= f->streams)
        return 0;
      if ((header[2] == 'd' && (header[3] == 'c' || header[3] == 'b')) ||
          (header[2] == 'w' && header[3] == 'b'))
        if (!append(&f->stream[si], p + 8, n))
          return 0;
    }
    p = next;
  }
  return p == end;
}
static struct file *file_find(void *h) {
  uint32_t i;
  for (i = 0; i < 8; i++)
    if (files[i] && files[i] == h)
      return files[i];
  return 0;
}
static struct view *view_find(void *h) {
  uint32_t i;
  for (i = 0; i < 32; i++)
    if (views[i] && views[i] == h)
      return views[i];
  return 0;
}
static struct getter *getter_find(void *h) {
  uint32_t i;
  for (i = 0; i < 16; i++)
    if (getters[i] && getters[i] == h)
      return getters[i];
  return 0;
}
void W32API AVIFileInit(void) {}
void W32API AVIFileExit(void) {}
uint32_t W32API AVIFileAddRef(void *h) {
  struct file *f = file_find(h);
  return f ? ++f->refs : 0;
}
uint32_t W32API AVIFileRelease(void *h) {
  struct file *f = file_find(h);
  uint32_t i;
  if (!f)
    return 0;
  if (--f->refs)
    return f->refs;
  for (i = 0; i < 4; i++)
    release(f->stream[i].frames);
  CloseHandle(f->handle);
  for (i = 0; i < 8; i++)
    if (files[i] == f)
      files[i] = 0;
  release(f);
  return 0;
}
static int32_t open_file(void **out, const void *name, uint32_t mode, void *cls,
                         int unicode) {
  uint8_t header[12];
  struct file *f;
  uint32_t slot, n, high = 0, budget = 1000000;
  if (!out)
    return AVIERR + 104;
  *out = 0;
  if (!name || cls || (mode & ~0x70u))
    return AVIERR + 104;
  for (slot = 0; slot < 8 && files[slot]; slot++) {
  }
  if (slot == 8)
    return AVIERR + 103;
  f = alloc(sizeof(*f));
  if (!f)
    return AVIERR + 103;
  if (unicode) {
    const WCHAR *wide = name;
    char path[260];
    uint32_t length = 0;
    int substituted = 0;
    while (length < 260 && wide[length])
      length++;
    if (length == 260 ||
        !WideCharToMultiByte(0, 0x400, wide, (int)length + 1, path, 260, 0,
                             &substituted) ||
        substituted) {
      release(f);
      return AVIERR + 104;
    }
    f->handle = CreateFileA(path, 0x80000000, 1, 0, 3, 0, 0);
  } else
    f->handle = CreateFileA(name, 0x80000000, 1, 0, 3, 0, 0);
  if ((intptr_t)f->handle == -1) {
    release(f);
    return AVIERR + 110;
  }
  f->refs = 1;
  f->length = GetFileSize(f->handle, &high);
  files[slot] = f;
  if (high || f->length > 0x7fffffffu || !readat(f, 0, header, 12) ||
      le32(header) != CC_FOUR('R', 'I', 'F', 'F') ||
      le32(header + 8) != CC_FOUR('A', 'V', 'I', ' ')) {
    AVIFileRelease(f);
    return AVIERR + 102;
  }
  n = le32(header + 4);
  if (n < 4 || n != f->length - 8 ||
      !scan(f, 12, f->length, -1, 0, 0, &budget) || !f->streams) {
    AVIFileRelease(f);
    return AVIERR + 102;
  }
  *out = f;
  return 0;
}
int32_t W32API AVIFileOpenA(void **out, const char *name, uint32_t mode,
                            void *cls) {
  return open_file(out, name, mode, cls, 0);
}
int32_t W32API AVIFileOpenW(void **out, const WCHAR *name, uint32_t mode,
                            void *cls) {
  return open_file(out, name, mode, cls, 1);
}
int32_t W32API AVIFileGetStream(void *h, void **out, uint32_t type,
                                int32_t at) {
  struct file *f = file_find(h);
  uint32_t i, slot;
  if (!out)
    return AVIERR + 104;
  *out = 0;
  if (!f || at < 0)
    return AVIERR + 104;
  for (i = 0; i < f->streams; i++)
    if (!type || le32(f->stream[i].header) == type) {
      if (at-- == 0)
        break;
    }
  if (i == f->streams)
    return AVIERR + 109;
  for (slot = 0; slot < 32 && views[slot]; slot++) {
  }
  if (slot == 32)
    return AVIERR + 103;
  views[slot] = alloc(sizeof(*views[slot]));
  if (!views[slot])
    return AVIERR + 103;
  views[slot]->file = f;
  views[slot]->stream = i;
  views[slot]->refs = 1;
  f->refs++;
  *out = views[slot];
  return 0;
}
uint32_t W32API AVIStreamAddRef(void *h) {
  struct view *v = view_find(h);
  return v ? ++v->refs : 0;
}
uint32_t W32API AVIStreamRelease(void *h) {
  struct view *v = view_find(h);
  uint32_t i;
  if (!v)
    return 0;
  if (--v->refs)
    return v->refs;
  AVIFileRelease(v->file);
  for (i = 0; i < 32; i++)
    if (views[i] == v)
      views[i] = 0;
  release(v);
  return 0;
}
int32_t W32API AVIStreamStart(void *h) {
  struct view *v = view_find(h);
  return v ? (int32_t)le32(v->file->stream[v->stream].header + 28) : -1;
}
int32_t W32API AVIStreamLength(void *h) {
  struct view *v = view_find(h);
  return v ? (int32_t)v->file->stream[v->stream].count : -1;
}
int32_t W32API AVIStreamReadFormat(void *h, int32_t pos, void *format,
                                   int32_t *size) {
  struct view *v = view_find(h);
  struct stream *s;
  (void)pos;
  if (!v || !size)
    return AVIERR + 104;
  s = &v->file->stream[v->stream];
  if (!format) {
    *size = (int32_t)s->format_len;
    return 0;
  }
  if (*size < 0 || (uint32_t)*size < s->format_len) {
    *size = (int32_t)s->format_len;
    return AVIERR + 116;
  }
  cc_copy(format, s->format, s->format_len);
  *size = (int32_t)s->format_len;
  return 0;
}
int32_t W32API AVIStreamRead(void *h, int32_t start, int32_t count,
                             void *buffer, int32_t capacity, int32_t *bytes,
                             int32_t *samples) {
  struct view *v = view_find(h);
  struct stream *s;
  uint32_t at, total = 0, i;
  if (bytes)
    *bytes = 0;
  if (samples)
    *samples = 0;
  if (!v || start < 0 || count < -1 || capacity < 0)
    return AVIERR + 104;
  s = &v->file->stream[v->stream];
  at = (uint32_t)AVIStreamStart(h);
  if ((uint32_t)start < at)
    return AVIERR + 104;
  at = (uint32_t)start - at;
  if (at >= s->count)
    return 0;
  if (count == -1)
    count = 1;
  if ((uint32_t)count > s->count - at)
    count = (int32_t)(s->count - at);
  for (i = 0; i < (uint32_t)count; i++) {
    if (s->frames[at + i].size > 0x7fffffffu - total)
      return AVIERR + 102;
    total += s->frames[at + i].size;
  }
  if (bytes)
    *bytes = (int32_t)total;
  if (!buffer) {
    if (samples)
      *samples = count;
    return 0;
  }
  if ((uint32_t)capacity < total)
    return AVIERR + 116;
  total = 0;
  for (i = 0; i < (uint32_t)count; i++) {
    struct frame *f = &s->frames[at + i];
    if (!readat(v->file, f->offset, (uint8_t *)buffer + total, f->size))
      return AVIERR + 107;
    total += f->size;
  }
  if (samples)
    *samples = count;
  return 0;
}
int32_t W32API AVIStreamBeginStreaming(void *h, int32_t start, int32_t end,
                                       int32_t rate) {
  (void)start;
  (void)end;
  (void)rate;
  return view_find(h) ? 0 : AVIERR + 104;
}
int32_t W32API AVIStreamEndStreaming(void *h) {
  return view_find(h) ? 0 : AVIERR + 104;
}
int32_t W32API AVIStreamGetFrameClose(void *h) {
  struct getter *g = getter_find(h);
  uint32_t i;
  if (!g)
    return AVIERR + 104;
  ICClose(g->ic);
  AVIStreamRelease(g->view);
  release(g->input);
  release(g->image);
  for (i = 0; i < 16; i++)
    if (getters[i] == g)
      getters[i] = 0;
  release(g);
  return 0;
}
void *W32API AVIStreamGetFrameOpen(void *h, struct cc_bitmap *desired) {
  if ((uintptr_t)desired == 1)
    desired = 0; /* AVIGETFRAMEF_BESTDISPLAYFMT */
  struct view *v = view_find(h);
  struct stream *s;
  struct getter *g;
  struct cc_bitmap out;
  uint32_t i;
  if (!v)
    return 0;
  s = &v->file->stream[v->stream];
  if (le32(s->header) != VIDE || s->format_len < 40)
    return 0;
  for (i = 0; i < 16 && getters[i]; i++) {
  }
  if (i == 16)
    return 0;
  g = alloc(sizeof(*g));
  if (!g)
    return 0;
  g->view = v;
  g->last = 0xffffffffu;
  g->ic = ICLocate(CC_FOUR('v', 'i', 'd', 'c'), le32(s->header + 4),
                   (void *)s->format, desired, 2);
  if (!g->ic) {
    release(g);
    return 0;
  }
  if (desired)
    cc_copy(&out, desired, 40);
  else if (ICSendMessage(g->ic, 0x400a, (uintptr_t)s->format,
                         (uintptr_t)&out)) {
    ICClose(g->ic);
    release(g);
    return 0;
  }
  if (ICSendMessage(g->ic, 0x400c, (uintptr_t)s->format, (uintptr_t)&out)) {
    ICClose(g->ic);
    release(g);
    return 0;
  }
  /* biSizeImage may legally be zero for BI_RGB. Size the allocation from
   * the validated geometry rather than trusting that optional field. */
  out.bytes = (((uint32_t)out.width * out.bits + 31) / 32) * 4 *
              (uint32_t)(out.height < 0 ? -out.height : out.height);
  g->frame_bytes = out.bytes;
  if (out.bytes > 16 * 1024 * 1024u) {
    ICClose(g->ic);
    release(g);
    return 0;
  }
  g->image = alloc(40 + 1024 + out.bytes);
  if (!g->image) {
    ICClose(g->ic);
    release(g);
    return 0;
  }
  cc_copy(g->image, &out, 40);
  if (out.bits <= 8 && s->format_len > 40)
    cc_copy((uint8_t *)g->image + 40, s->format + 40,
            s->format_len - 40 > 1024 ? 1024 : s->format_len - 40);
  AVIStreamAddRef(v);
  getters[i] = g;
  return g;
}
void *W32API AVIStreamGetFrame(void *h, int32_t pos) {
  struct getter *g = getter_find(h);
  struct stream *s;
  uint32_t target, at, palette;
  if (!g || pos < 0)
    return 0;
  s = &g->view->file->stream[g->view->stream];
  target = le32(s->header + 28);
  if ((uint32_t)pos < target)
    return 0;
  target = (uint32_t)pos - target;
  if (target >= s->count)
    return 0;
  palette = g->image->bits <= 8 ? 1024 : 0;
  if (g->last != 0xffffffffu && target < g->last) {
    ICSendMessage(g->ic, 0x400e, 0, 0);
    if (ICSendMessage(g->ic, 0x400c, (uintptr_t)s->format, (uintptr_t)g->image))
      return 0;
    g->last = 0xffffffffu;
    cc_zero((uint8_t *)g->image + 40 + palette, g->frame_bytes);
  }
  at = g->last == 0xffffffffu ? 0 : g->last + 1;
  for (; at <= target; at++) {
    struct frame *f = &s->frames[at];
    struct cc_bitmap *in = (void *)s->format;
    if (f->size > 16 * 1024 * 1024u)
      return 0;
    if (f->size > g->input_cap) {
      uint8_t *p = alloc(f->size);
      if (!p)
        return 0;
      release(g->input);
      g->input = p;
      g->input_cap = f->size;
    }
    if (f->size && !readat(g->view->file, f->offset, g->input, f->size))
      return 0;
    in->bytes = f->size;
    if (f->size && ICDecompress(g->ic, at ? 0x08000000 : 0, in, g->input,
                                g->image, (uint8_t *)g->image + 40 + palette))
      return 0;
    g->last = at;
  }
  return g->image;
}
static int32_t stream_info(void *h, void *out, int32_t bytes, int unicode) {
  struct view *v = view_find(h);
  struct stream *s;
  uint8_t result[204];
  uint32_t fields[19], i, n = unicode ? 204 : 140;
  if (!v || !out || bytes < 0 || (uint32_t)bytes < n)
    return AVIERR + 104;
  s = &v->file->stream[v->stream];
  cc_zero(result, sizeof(result));
  cc_zero(fields, sizeof(fields));
  fields[0] = le32(s->header);
  fields[1] = le32(s->header + 4);
  fields[2] = le32(s->header + 8);
  fields[4] = le32(s->header + 12);
  fields[5] = le32(s->header + 20);
  fields[6] = le32(s->header + 24);
  fields[7] = le32(s->header + 28);
  fields[8] = s->count;
  fields[9] = le32(s->header + 16);
  fields[10] = le32(s->header + 36);
  fields[11] = le32(s->header + 40);
  fields[12] = le32(s->header + 44);
  for (i = 0; i < 4; i++)
    fields[13 + i] =
        (uint32_t)(int32_t)(int16_t)cc_le16(s->header + 48 + i * 2);
  cc_copy(result, fields, sizeof(fields));
  cc_copy(out, result, n);
  return 0;
}
int32_t W32API AVIStreamInfoA(void *h, void *out, int32_t n) {
  return stream_info(h, out, n, 0);
}
int32_t W32API AVIStreamInfoW(void *h, void *out, int32_t n) {
  return stream_info(h, out, n, 1);
}
static int32_t file_info(void *h, void *out, int32_t bytes, int unicode) {
  struct file *f = file_find(h);
  uint32_t fields[11], i, n = unicode ? 172 : 108;
  uint8_t result[172];
  if (!f || !out || bytes < 0 || (uint32_t)bytes < n)
    return AVIERR + 104;
  cc_zero(fields, sizeof(fields));
  cc_zero(result, sizeof(result));
  fields[2] = 0x30;
  fields[3] = f->streams;
  for (i = 0; i < f->streams; i++) {
    struct stream *s = &f->stream[i];
    if (le32(s->header) == VIDE && s->format_len >= 40) {
      struct cc_bitmap *b = (void *)s->format;
      if (b->width <= 0 || b->height == (-2147483647 - 1) || !b->height)
        return AVIERR + 102;
      fields[5] = (uint32_t)b->width;
      fields[6] = (uint32_t)(b->height < 0 ? -b->height : b->height);
      fields[7] = le32(s->header + 20);
      fields[8] = le32(s->header + 24);
      fields[9] = s->count;
      break;
    }
  }
  cc_copy(result, fields, sizeof(fields));
  cc_copy(out, result, n);
  return 0;
}
int32_t W32API AVIFileInfoA(void *h, void *out, int32_t n) {
  return file_info(h, out, n, 0);
}
int32_t W32API AVIFileInfoW(void *h, void *out, int32_t n) {
  return file_info(h, out, n, 1);
}
