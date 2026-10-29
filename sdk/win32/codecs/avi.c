/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Read-only bounded RIFF AVI access. Playback and MCI belong to f5-12B.
 * f5-12B adds AVIStreamSampleToTime/TimeToSample (strh dwScale/dwRate).
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
  uint32_t offset, size, id, sample;
};
struct stream {
  uint8_t header[56], format[2048];
  uint32_t format_len, count, capacity, samples, header_seen;
  struct frame *frames;
};
struct file {
  void *handle;
  uint32_t refs, length, streams, movi, movi_end, index, index_bytes;
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
static int append(struct stream *s, uint32_t p, uint32_t n, uint32_t id) {
  uint32_t unit = le32(s->header + 44), samples = unit ? n / unit : 1;
  if ((unit && n % unit) || samples > 0x7fffffffu - s->samples)
    return 0;
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
  s->frames[s->count++] = (struct frame){p, n, id, s->samples};
  s->samples += samples;
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
      if (type == CC_FOUR('m', 'o', 'v', 'i')) {
        if (f->movi || depth || index >= 0)
          return 0;
        f->movi = p + 8; /* idx1 offsets normally start at the movi FOURCC */
        f->movi_end = p + 8 + n;
      }
      if (type == CC_FOUR('s', 't', 'r', 'l')) {
        if (movi || f->streams == 4)
          return 0;
        child = (int)f->streams++;
      }
      if (!scan(f, p + 12, p + 8 + n, child,
                movi || type == CC_FOUR('m', 'o', 'v', 'i'), depth + 1, budget))
        return 0;
    } else if (index >= 0 && id == CC_FOUR('s', 't', 'r', 'h')) {
      if (movi || f->stream[index].header_seen || n < 56 ||
          !readat(f, p + 8, f->stream[index].header, 56))
        return 0;
      f->stream[index].header_seen = 1;
    } else if (index >= 0 && id == CC_FOUR('s', 't', 'r', 'f')) {
      if (movi || f->stream[index].format_len || !n || n > 2048 ||
          !readat(f, p + 8, f->stream[index].format, n))
        return 0;
      f->stream[index].format_len = n;
    } else if (!depth && id == CC_FOUR('i', 'd', 'x', '1')) {
      if (f->index || !f->movi || n % 16 || n / 16 > 1000000)
        return 0;
      f->index = p + 8;
      f->index_bytes = n;
    } else if (movi && header[0] >= '0' && header[0] <= '9' &&
               header[1] >= '0' && header[1] <= '9') {
      uint32_t si = (header[0] - '0') * 10 + header[1] - '0';
      if (si >= f->streams)
        return 0;
      if ((header[2] == 'd' && (header[3] == 'c' || header[3] == 'b')) ||
          (header[2] == 'w' && header[3] == 'b'))
        if (!append(&f->stream[si], p + 8, n, id))
          return 0;
    }
    p = next;
  }
  return p == end;
}
/* idx1 is optional. Validate against the physical movi scan, never use an
 * untrusted index to introduce a read. AVIOLDINDEX permits historical
 * file-relative offsets as well as offsets relative to the movi FOURCC. */
static int index_entry(struct file *f, const uint8_t *entry, uint32_t base,
                       uint32_t *next) {
  uint32_t id = le32(entry), flags = le32(entry + 4);
  uint32_t offset = le32(entry + 8), size = le32(entry + 12);
  if (offset > f->length - base)
    return 0;
  offset += base;
  if (offset < f->movi + 4 || offset > f->movi_end ||
      f->movi_end - offset < 8 || size > f->movi_end - offset - 8)
    return 0;
  if (flags & 1) { /* AVIIF_LIST: index entry names a rec list. */
    uint8_t header[12];
    return id == CC_FOUR('r', 'e', 'c', ' ') && size >= 4 &&
           readat(f, offset, header, sizeof(header)) &&
           le32(header) == CC_FOUR('L', 'I', 'S', 'T') &&
           le32(header + 4) == size && le32(header + 8) == id;
  }
  if (entry[0] < '0' || entry[0] > '9' || entry[1] < '0' || entry[1] > '9')
    return 0;
  uint32_t si = (entry[0] - '0') * 10 + entry[1] - '0';
  if (si >= f->streams)
    return 0;
  struct stream *s = &f->stream[si];
  uint32_t lo = next[si], hi = s->count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (s->frames[mid].offset < offset + 8)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == s->count || s->frames[lo].offset != offset + 8 ||
      s->frames[lo].size != size || s->frames[lo].id != id)
    return 0;
  next[si] = lo + 1;
  return 1;
}
static int validate_index(struct file *f) {
  uint32_t base = f->movi, next[4] = {0};
  for (uint32_t at = 0; at < f->index_bytes; at += 16) {
    uint8_t entry[16];
    if (!readat(f, f->index + at, entry, sizeof(entry)))
      return 0;
    if (!index_entry(f, entry, base, next)) {
      if (at || !index_entry(f, entry, 0, next))
        return 0;
      base = 0;
    }
  }
  return 1;
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
      !scan(f, 12, f->length, -1, 0, 0, &budget) || !f->streams ||
      !f->movi || !validate_index(f)) {
    AVIFileRelease(f);
    return AVIERR + 102;
  }
  for (uint32_t i = 0; i < f->streams; i++) {
    struct stream *s = &f->stream[i];
    if (!s->header_seen || !s->format_len || !le32(s->header + 20) || !le32(s->header + 24) ||
        le32(s->header + 28) > 0x7fffffffu - s->samples) {
      AVIFileRelease(f);
      return AVIERR + 102;
    }
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
  return v ? (int32_t)v->file->stream[v->stream].samples : -1;
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
  if (at >= s->samples || !count)
    return 0;
  uint32_t unit = le32(s->header + 44);
  if (unit) {
    uint32_t lo = 0, hi = s->count;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (s->frames[mid].sample <= at)
        lo = mid + 1;
      else
        hi = mid;
    }
    i = lo - 1;
    uint32_t skip = (at - s->frames[i].sample) * unit;
    if (count == -1)
      count = (int32_t)((s->frames[i].size - skip) / unit);
    if ((uint32_t)count > s->samples - at)
      count = (int32_t)(s->samples - at);
    /* A fixed-size read fills as many complete samples as fit. Only a
     * buffer smaller than one sample is AVIERR_BUFFERTOOSMALL. */
    if (buffer) {
      if ((uint32_t)capacity < unit)
        return AVIERR + 116;
      if ((uint32_t)count > (uint32_t)capacity / unit)
        count = (int32_t)((uint32_t)capacity / unit);
    }
    if ((uint32_t)count > 0x7fffffffu / unit)
      return AVIERR + 102;
    uint32_t remaining = (uint32_t)count * unit;
    while (remaining) {
      struct frame *frame = &s->frames[i++];
      uint32_t n = frame->size - skip;
      if (n > remaining)
        n = remaining;
      if (buffer && n && !readat(v->file, frame->offset + skip,
                                 (uint8_t *)buffer + total, n))
        return AVIERR + 107;
      total += n;
      remaining -= n;
      skip = 0;
    }
    if (bytes)
      *bytes = (int32_t)total;
    if (samples)
      *samples = count;
    return 0;
  }
  if (count == -1)
    count = 1;
  if ((uint32_t)count > s->count - at)
    count = (int32_t)(s->count - at);
  for (i = 0; i < (uint32_t)count; i++) {
    if (s->frames[at + i].size > 0x7fffffffu - total)
      return AVIERR + 102;
    if (buffer && s->frames[at + i].size > (uint32_t)capacity - total) {
      if (!i)
        return AVIERR + 116;
      count = (int32_t)i;
      break;
    }
    total += s->frames[at + i].size;
  }
  if (bytes)
    *bytes = (int32_t)total;
  if (!buffer) {
    if (samples)
      *samples = count;
    return 0;
  }
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
  /* the decoder's format: a header and, for indexed output, a colour table */
  union {
    struct cc_bitmap header;
    uint8_t bytes[40 + 1024];
  } format;
  struct cc_bitmap *out = &format.header;
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
    cc_copy(out, desired, 40);
  else {
    /* GET_FORMAT reads the colour table after the header: give it the
     * stream format with a bounded, zero-filled table. */
    uint8_t source[40 + 1024];
    uint32_t header = le32(s->format), table = s->format_len > header ? s->format_len - header : 0;
    cc_zero(source, sizeof(source));
    cc_copy(source, s->format, 40);
    ((struct cc_bitmap *)source)->size = 40;
    if (header >= 40 && table)
      cc_copy(source + 40, s->format + header, table > 1024 ? 1024 : table);
    if (ICSendMessage(g->ic, 0x400a, (uintptr_t)source, (uintptr_t)out)) {
      ICClose(g->ic);
      release(g);
      return 0;
    }
  }
  if (ICSendMessage(g->ic, 0x400c, (uintptr_t)s->format, (uintptr_t)out)) {
    ICClose(g->ic);
    release(g);
    return 0;
  }
  /* biSizeImage may legally be zero for BI_RGB. Size the allocation from
   * the validated geometry rather than trusting that optional field. */
  out->bytes = (((uint32_t)out->width * out->bits + 31) / 32) * 4 *
              (uint32_t)(out->height < 0 ? -out->height : out->height);
  g->frame_bytes = out->bytes;
  if (out->bytes > 16 * 1024 * 1024u) {
    ICClose(g->ic);
    release(g);
    return 0;
  }
  g->image = alloc(40 + 1024 + out->bytes);
  if (!g->image) {
    ICClose(g->ic);
    release(g);
    return 0;
  }
  cc_copy(g->image, out, 40);
  if (out->bits <= 8 && s->format_len > 40)
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
  fields[8] = s->samples;
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
/* f5-12B: AVIStreamSampleToTime / AVIStreamTimeToSample. Samples are in
 * stream units (dwRate/dwScale per second), clamped to [dwStart,
 * dwStart+dwLength]; integer arithmetic rounding toward zero; -1 on an
 * invalid stream or a zero rate/scale (Video for Windows reference). */
static uint64_t div64(uint64_t n, uint64_t d) { /* no compiler runtime */
  uint64_t q = 0, r = 0;
  for (int bit = 63; bit >= 0; bit--) {
    r = (r << 1) | ((n >> bit) & 1);
    if (r >= d) {
      r -= d;
      q |= (uint64_t)1 << bit;
    }
  }
  return q;
}
static int32_t clamp_sample(const struct stream *s, int64_t sample) {
  int64_t start = le32(s->header + 28), end = start + s->samples;
  if (sample < start)
    sample = start;
  if (sample > end)
    sample = end;
  return (int32_t)(sample > 0x7fffffff ? 0x7fffffff : sample);
}
int32_t W32API AVIStreamSampleToTime(void *h, int32_t sample) {
  struct view *v = view_find(h);
  if (!v)
    return -1;
  const struct stream *s = &v->file->stream[v->stream];
  const uint8_t *header = s->header;
  uint32_t scale = le32(header + 20), rate = le32(header + 24);
  if (!scale || !rate)
    return -1;
  uint64_t ticks = (uint64_t)(uint32_t)clamp_sample(s, sample) * scale;
  uint64_t seconds = div64(ticks, rate);
  if (seconds > 0x7fffffffu / 1000)
    return -1;
  uint64_t ms = seconds * 1000 + div64((ticks - seconds * rate) * 1000, rate);
  return ms > 0x7fffffff ? -1 : (int32_t)ms;
}
int32_t W32API AVIStreamTimeToSample(void *h, int32_t ms) {
  struct view *v = view_find(h);
  if (!v || ms < 0)
    return -1;
  const struct stream *s = &v->file->stream[v->stream];
  const uint8_t *header = s->header;
  uint32_t scale = le32(header + 20), rate = le32(header + 24);
  if (!scale || !rate)
    return -1;
  return clamp_sample(
      s, (int64_t)div64((uint64_t)ms * rate, (uint64_t)scale * 1000));
}
