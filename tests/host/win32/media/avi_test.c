/* f5-12B AVI host evidence: production mciavi.c (avivideo), msvfw32
 * DrawDib and MCIWnd, avifil32, the ICM decoders, msacm32, winmm MCI and
 * waveOut/mixer, drawing through the production gdi32 DIB engine into
 * window surfaces. USER windows, messages and timers are modelled here;
 * kernel32 and time in mci_host.c. Frames are dumped for independent
 * pixel oracles (test_media.py). SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "mci_host.h"
const struct mci_driver *CiukiMciDriver(const struct mci_host *);
uintptr_t DrawDibOpen(void);
int DrawDibClose(uintptr_t);
int DrawDibBegin(uintptr_t, uintptr_t, int, int, const void *, int, int,
                 uint32_t);
int DrawDibDraw(uintptr_t, uintptr_t, int, int, int, int, const void *,
                const void *, int, int, int, int, uint32_t);
int DrawDibEnd(uintptr_t);
int DrawDibChangePalette(uintptr_t, int, int, const uint8_t *);
void *DrawDibGetBuffer(uintptr_t, void *, uint32_t, uint32_t);
uintptr_t MCIWndCreateA(uintptr_t, uintptr_t, uint32_t, const char *);
int MCIWndRegisterClass(void);
void *ag_create(int, int, uintptr_t *, uint32_t **);
void ag_destroy(void *);
void ag_exit(void);
long ag_live(void);

const struct mci_driver *mci_host_avi(const struct mci_host *host) {
  return CiukiMciDriver(host);
}

/* ---- kernel32 heap and text (codec DLLs) ---- */
long heap_live;
void *GetProcessHeap(void) { return (void *)1; }
void *HeapAlloc(void *h, uint32_t flags, uint32_t n) {
  (void)h;
  void *p = flags & 8 ? calloc(1, n) : malloc(n);
  if (p)
    heap_live++;
  return p;
}
int HeapFree(void *h, uint32_t flags, void *p) {
  (void)h;
  (void)flags;
  if (p)
    heap_live--;
  free(p);
  return 1;
}
int WideCharToMultiByte(uint32_t cp, uint32_t flags, const WCHAR *s, int n,
                        char *out, int cap, const char *def, int *used) {
  (void)cp;
  (void)flags;
  (void)def;
  if (n > cap)
    return 0;
  *used = 0;
  for (int i = 0; i < n; i++)
    out[i] = (char)s[i];
  return n;
}

/* ---- USER model ---- */
typedef intptr_t (*wndproc)(uintptr_t, uint32_t, uintptr_t, intptr_t);
struct wndclass {
  uint32_t style;
  uintptr_t proc;
  int32_t cls_extra, wnd_extra;
  uintptr_t instance, icon, cursor, background, menu, name;
};
struct rect {
  int32_t left, top, right, bottom;
};
struct paint {
  uintptr_t dc;
  int32_t erase;
  struct rect rect;
  int32_t restore, increment;
  uint8_t reserved[32];
};
struct create {
  uintptr_t params, instance, menu, parent;
  int32_t cy, cx, y, x, style;
  uintptr_t name, klass;
  uint32_t ex_style;
};
struct window {
  uintptr_t handle, parent, dc;
  wndproc proc;
  uint32_t style, visible, dirty;
  int x, y, w, h;
  char title[128], klass[32];
  void *surface;
  uint32_t *pixels;
};
#define WINDOWS 16
static struct window windows[WINDOWS];
static struct {
  char name[32];
  wndproc proc;
} classes[8];
static struct {
  uintptr_t window, id;
  uint32_t period, due;
} timers[8];
static struct {
  uintptr_t window;
  uint32_t message;
  uintptr_t wp;
  intptr_t lp;
} queue[64];
static unsigned queued;
static uintptr_t next_handle = 0x10001;
static struct window *win(uintptr_t h) {
  for (int i = 0; i < WINDOWS; i++)
    if (windows[i].handle && windows[i].handle == h)
      return &windows[i];
  return 0;
}
static void resize(struct window *w, int width, int height) {
  if (width < 1)
    width = 1;
  if (height < 1)
    height = 1;
  ag_destroy(w->surface);
  w->surface = ag_create(width, height, &w->dc, &w->pixels);
  assert(w->surface);
  w->w = width;
  w->h = height;
}
uint16_t RegisterClassA(const struct wndclass *c) {
  for (int i = 0; i < 8; i++)
    if (!classes[i].proc || !strcmp(classes[i].name, (const char *)c->name)) {
      snprintf(classes[i].name, sizeof(classes[i].name), "%s",
               (const char *)c->name);
      classes[i].proc = (wndproc)c->proc;
      out("USER RegisterClass %s\n", classes[i].name);
      return (uint16_t)(0xc000 + i);
    }
  return 0;
}
intptr_t SendMessageA(uintptr_t h, uint32_t m, uintptr_t wp, intptr_t lp);
uintptr_t CreateWindowExA(uint32_t ex, const char *klass, const char *title,
                          uint32_t style, int x, int y, int w, int h,
                          uintptr_t parent, uintptr_t menu, uintptr_t instance,
                          void *param) {
  wndproc proc = 0;
  for (int i = 0; i < 8; i++)
    if (classes[i].proc && !strcmp(classes[i].name, klass))
      proc = classes[i].proc;
  if (!proc)
    return 0;
  struct window *n = 0;
  for (int i = 0; i < WINDOWS && !n; i++)
    if (!windows[i].handle)
      n = &windows[i];
  assert(n);
  memset(n, 0, sizeof(*n));
  n->handle = next_handle++;
  n->parent = parent;
  n->proc = proc;
  n->style = style;
  n->visible = !!(style & 0x10000000u);
  /* CW_USEDEFAULT in x places the window (y is then ignored): at 0,0. */
  n->x = x == (int)0x80000000 ? 0 : x;
  n->y = x == (int)0x80000000 ? 0 : y;
  snprintf(n->title, sizeof(n->title), "%s", title ? title : "");
  snprintf(n->klass, sizeof(n->klass), "%s", klass);
  resize(n, w, h);
  out("USER CreateWindow %s hwnd=%lx style=%08x %dx%d parent=%lx\n", klass,
      (unsigned long)n->handle, style, n->w, n->h, (unsigned long)parent);
  struct create c = {(uintptr_t)param, instance, menu, parent, h, w, y, x,
                     (int32_t)style, (uintptr_t)title, (uintptr_t)klass, ex};
  uintptr_t handle = n->handle;
  if (proc(handle, 0x0001, 0, (intptr_t)&c) == -1) {
    struct window *d = win(handle);
    ag_destroy(d->surface);
    memset(d, 0, sizeof(*d));
    return 0;
  }
  return handle;
}
int DestroyWindow(uintptr_t h) {
  struct window *w = win(h);
  if (!w)
    return 0;
  out("USER DestroyWindow hwnd=%lx\n", (unsigned long)h);
  w->proc(h, 0x0002, 0, 0);
  w = win(h);
  ag_destroy(w->surface);
  for (int i = 0; i < 8; i++)
    if (timers[i].window == h)
      timers[i].window = 0;
  memset(w, 0, sizeof(*w));
  return 1;
}
int ShowWindow(uintptr_t h, int cmd) {
  struct window *w = win(h);
  if (!w)
    return 0;
  int was = (int)w->visible;
  w->visible = cmd != 0;
  if (was != (int)w->visible)
    out("USER ShowWindow hwnd=%lx %d\n", (unsigned long)h, cmd);
  return was;
}
uintptr_t GetDC(uintptr_t h) {
  struct window *w = win(h);
  return w ? w->dc : 0;
}
int ReleaseDC(uintptr_t h, uintptr_t dc) {
  (void)h;
  (void)dc;
  return 1;
}
uintptr_t BeginPaint(uintptr_t h, struct paint *ps) {
  struct window *w = win(h);
  memset(ps, 0, sizeof(*ps));
  if (!w)
    return 0;
  ps->dc = w->dc;
  w->dirty = 0;
  return w->dc;
}
int EndPaint(uintptr_t h, const struct paint *ps) {
  (void)h;
  (void)ps;
  return 1;
}
intptr_t DefWindowProcA(uintptr_t h, uint32_t m, uintptr_t wp, intptr_t lp) {
  (void)h;
  (void)m;
  (void)wp;
  (void)lp;
  return 0;
}
int GetClientRect(uintptr_t h, struct rect *r) {
  struct window *w = win(h);
  if (!w)
    return 0;
  *r = (struct rect){0, 0, w->w, w->h};
  return 1;
}
int GetWindowRect(uintptr_t h, struct rect *r) {
  struct window *w = win(h);
  if (!w)
    return 0;
  *r = (struct rect){w->x, w->y, w->x + w->w, w->y + w->h};
  return 1;
}
int SetWindowTextA(uintptr_t h, const char *t) {
  struct window *w = win(h);
  if (!w)
    return 0;
  snprintf(w->title, sizeof(w->title), "%s", t);
  out("USER SetWindowText hwnd=%lx '%s'\n", (unsigned long)h, t);
  return 1;
}
int SetWindowPos(uintptr_t h, uintptr_t after, int x, int y, int cx, int cy,
                 uint32_t flags) {
  (void)after;
  struct window *w = win(h);
  if (!w)
    return 0;
  if (!(flags & 2)) {
    w->x = x;
    w->y = y;
  }
  if (!(flags & 1) && (cx != w->w || cy != w->h)) {
    resize(w, cx, cy);
    out("USER SetWindowPos hwnd=%lx %dx%d\n", (unsigned long)h, w->w, w->h);
    w->proc(h, 0x0005, 0, (intptr_t)((uint32_t)w->w | (uint32_t)w->h << 16));
  }
  if (flags & 0x40)
    w->visible = 1;
  return 1;
}
int IsWindow(uintptr_t h) { return win(h) != 0; }
int IsWindowVisible(uintptr_t h) {
  struct window *w = win(h);
  return w && w->visible;
}
int GetSystemMetrics(int n) { return n == 0 ? 640 : n == 1 ? 480 : 0; }
int AdjustWindowRect(struct rect *r, uint32_t style, int menu) {
  (void)r;
  (void)style;
  (void)menu;
  return 1; /* the model has no non-client area */
}
int InvalidateRect(uintptr_t h, const struct rect *r, int erase) {
  (void)r;
  (void)erase;
  struct window *w = win(h);
  if (w)
    w->dirty = 1;
  return w != 0;
}
void harness_post(uintptr_t, uint32_t, uintptr_t, intptr_t);
int PostMessageA(uintptr_t h, uint32_t m, uintptr_t wp, intptr_t lp) {
  if (!win(h))
    return 0;
  harness_post(h, m, wp, lp);
  return 1;
}
intptr_t SendMessageA(uintptr_t h, uint32_t m, uintptr_t wp, intptr_t lp) {
  struct window *w = win(h);
  return w ? w->proc(h, m, wp, lp) : 0;
}
uintptr_t GetParent(uintptr_t h) {
  struct window *w = win(h);
  return w ? w->parent : 0;
}
uintptr_t SetTimer(uintptr_t h, uintptr_t id, uint32_t ms, uintptr_t proc) {
  (void)proc;
  for (int i = 0; i < 8; i++)
    if (!timers[i].window || (timers[i].window == h && timers[i].id == id)) {
      timers[i].window = h;
      timers[i].id = id;
      timers[i].period = ms;
      timers[i].due = now_ms + ms;
      return id;
    }
  return 0;
}
int KillTimer(uintptr_t h, uintptr_t id) {
  for (int i = 0; i < 8; i++)
    if (timers[i].window == h && timers[i].id == id) {
      timers[i].window = 0;
      return 1;
    }
  return 0;
}
uint32_t GetWindowThreadProcessId(uintptr_t h, uint32_t *pid) {
  if (pid)
    *pid = 1;
  return win(h) ? 4242 : 0; /* the harness has one thread (GetCurrentThreadId) */
}
int32_t GetWindowLongA(uintptr_t h, int index) {
  struct window *w = win(h);
  return w && index == -16 ? (int32_t)w->style : 0;
}
void harness_post(uintptr_t window, uint32_t message, uintptr_t wp,
                  intptr_t lp) {
  if (!win(window) || queued == 64)
    return;
  queue[queued].window = window;
  queue[queued].message = message;
  queue[queued].wp = wp;
  queue[queued].lp = lp;
  queued++;
}
/* Delivers posted messages, paints and due timers (the app's loop). */
static void pump(void) {
  while (queued) {
    unsigned n = queued;
    for (unsigned i = 0; i < n; i++) {
      struct window *w = win(queue[i].window);
      if (w)
        w->proc(queue[i].window, queue[i].message, queue[i].wp, queue[i].lp);
    }
    memmove(queue, queue + n, (queued - n) * sizeof(queue[0]));
    queued -= n;
  }
  for (int i = 0; i < WINDOWS; i++)
    if (windows[i].handle && windows[i].dirty)
      windows[i].proc(windows[i].handle, 0x000F, 0, 0);
  for (int i = 0; i < 8; i++)
    if (timers[i].window && now_ms >= timers[i].due) {
      timers[i].due = now_ms + timers[i].period;
      SendMessageA(timers[i].window, 0x0113, timers[i].id, 0);
    }
}
static void run(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i += 5) {
    Sleep(5);
    pump();
  }
}

/* ---- trace helpers ---- */
static uint32_t expect(const char *s, uint32_t code, const char *want) {
  char result[256];
  out("> %s\n", s);
  uint32_t r = mciSendStringA(s, result, sizeof(result), 0x4321);
  out("= %u '%s'\n", r, r ? "" : result);
  if (r != code || (want && !r && strcmp(result, want))) {
    fprintf(stderr, "FAIL %s: %u '%s' (want %u '%s')\n", s, r,
            r ? "" : result, code, want ? want : "");
    exit(1);
  }
  return r;
}
static void dump(uintptr_t h, const char *name) {
  struct window *w = win(h);
  assert(w);
  char path[64];
  snprintf(path, sizeof(path), "%s.frame", name);
  FILE *f = fopen(path, "wb");
  assert(f);
  uint32_t size[2] = {(uint32_t)w->w, (uint32_t)w->h};
  fwrite(size, 4, 2, f);
  fwrite(w->pixels, 4, (size_t)w->w * (size_t)w->h, f);
  fclose(f);
  uint32_t hash = 2166136261u;
  for (int i = 0; i < w->w * w->h; i++) {
    hash ^= w->pixels[i];
    hash *= 16777619u;
  }
  out("FRAME %s %dx%d fnv=%08x\n", name, w->w, w->h, hash);
}
static void oracle_begin(void) {
  capture = malloc(4096 * 4);
  assert(capture);
  capture_frames = 0;
  capture_cap = 4096;
}
static void oracle_end(const char *file) {
  int fd = hf_open(file);
  assert(fd >= 0);
  long size = hf_size(fd);
  int16_t *want = malloc((size_t)size);
  assert(want && hf_read(fd, want, (unsigned long)size) == size);
  hf_close(fd);
  uint32_t count = (uint32_t)(size / 2);
  size_t found = (size_t)-1;
  for (size_t at = 0; at + count <= capture_frames && found == (size_t)-1;
       at++) {
    uint32_t i = 0;
    for (; i < count; i++)
      if (capture[(at + i) * 2] != want[i] ||
          capture[(at + i) * 2 + 1] != want[i])
        break;
    if (i == count)
      found = at;
  }
  out("ORACLE %s samples=%u match=%d\n", file, count, found != (size_t)-1);
  if (found == (size_t)-1) {
    fprintf(stderr, "FAIL oracle %s\n", file);
    exit(1);
  }
  free(want);
  free(capture);
  capture = 0;
}

/* Owner window of the MCIWnd test: logs its notifications. */
static intptr_t owner_proc(uintptr_t h, uint32_t m, uintptr_t wp,
                           intptr_t lp) {
  if (m == 0x0400 + 203) /* MCIWNDM_NOTIFYMEDIA: lParam is the name */
    out("OWNER t=%u msg=WM_USER+%u from=%lx media='%s'\n", now_ms,
        m - 0x0400, (unsigned long)wp, (const char *)lp);
  else if (m >= 0x0400 + 200 && m <= 0x0400 + 205)
    out("OWNER t=%u msg=WM_USER+%u from=%lx value=%ld\n", now_ms,
        m - 0x0400, (unsigned long)wp, (long)lp);
  (void)h;
  return 0;
}

static void avivideo(void) {
  out("== avivideo\n");
  expect("open clip-pcm.avi alias v", 0, "1");
  expect("status v length", 0, "6");
  expect("status v time format", 0, "frames");
  expect("status v frame rate", 0, "15000");
  expect("status v window handle", 0, "65537");
  expect("status v window visible", 0, "false");
  expect("capability v device type", 0, "digitalvideo");
  expect("capability v has audio", 0, "true");
  expect("capability v can stretch", 0, "true");
  expect("where v destination", 0, "0 0 32 16");
  expect("where v source", 0, "0 0 32 16");
  expect("info v product", 0, "CiukiOS AVI video");
  expect("status v audio", 0, "true");
  oracle_begin();
  expect("play v wait", 0, "");
  oracle_end("clip-pcm.pcm");
  expect("status v window visible", 0, "true");
  expect("status v position", 0, "5");
  dump(0x10001, "pcm-last");
  expect("seek v to 2", 0, "");
  dump(0x10001, "pcm-seek2");
  expect("step v by 2", 0, "");
  expect("status v position", 0, "4");
  dump(0x10001, "pcm-step4");
  expect("step v reverse", 0, "");
  expect("status v position", 0, "3");
  expect("set v time format ms", 0, "");
  expect("status v length", 0, "400");
  expect("status v position", 0, "200");
  expect("set v time format frames", 0, "");
  /* a caller's window, destination scaled 2x */
  uintptr_t app = CreateWindowExA(0, "Owner", "app", 0x10000000u, 0, 0, 64,
                                  32, 0, 0, 0, 0);
  char command[96];
  snprintf(command, sizeof(command), "window v handle %lu",
           (unsigned long)app);
  expect(command, 0, "");
  expect("status v window handle", 0, "65538");
  expect("put v destination at 0 0 64 32", 0, "");
  expect("where v destination", 0, "0 0 64 32");
  expect("seek v to 1", 0, "");
  dump(app, "app-2x-frame1");
  expect("play v from 0 to 3 notify", 0, "");
  run(150);
  expect("status v mode", 0, "playing");
  expect("pause v", 0, "");
  run(50);
  expect("resume v", 0, "");
  run(400);
  expect("status v mode", 0, "stopped");
  expect("status v position", 0, "3");
  dump(app, "app-2x-frame3");
  expect("window v handle default", 0, "");
  expect("put v destination", 0, "");
  expect("play v from 4 fullscreen wait", 0, "");
  /* 32x16 scaled by 20 into the 640x480 screen, centred */
  expect("where v destination", 0, "0 80 640 320");
  pump(); /* the movie window's thread restores its size */
  expect("where v destination", 0, "0 0 32 16");
  expect("where v window", 0, "0 0 32 16");
  expect("play v from 0 repeat notify", 0, "");
  run(700);
  expect("status v mode", 0, "playing");
  expect("stop v", 0, "");
  expect("window v state hide", 0, "");
  expect("status v window visible", 0, "false");
  expect("window v text \"Intro\"", 0, "");
  expect("info v window text", 0, "Intro");
  expect("set v audio all off", 0, "");
  expect("status v audio", 0, "false");
  expect("set v speed 500", MCIERR_UNSUPPORTED_FUNCTION, 0);
  expect("play v reverse", MCIERR_UNSUPPORTED_FUNCTION, 0);
  expect("seek v to 6", MCIERR_OUTOFRANGE, 0);
  expect("update v hdc 0", 0, "");
  expect("close v", 0, "");
  DestroyWindow(app);
  out("-- IMA ADPCM sound track\n");
  expect("open clip-ima.avi alias w", 0, "1");
  oracle_begin();
  expect("play w wait", 0, "");
  oracle_end("clip-ima.pcm");
  uintptr_t movie = 0;
  for (int i = 0; i < WINDOWS; i++)
    if (windows[i].handle && !strcmp(windows[i].klass, "CiukiAVIWindow"))
      movie = windows[i].handle;
  dump(movie, "ima-last");
  expect("close w", 0, "");
  expect("open missing.avi", MCIERR_FILE_NOT_FOUND, 0);
  expect("open broken.wav type avivideo", MCIERR_INVALID_FILE, 0);
}

static void drawdib(void) {
  out("== DrawDib\n");
  int fd = hf_open("clip-pcm.avi");
  assert(fd >= 0);
  long size = hf_size(fd);
  uint8_t *file = malloc((size_t)size);
  assert(file && hf_read(fd, file, (unsigned long)size) == size);
  hf_close(fd);
  /* strf of the video stream and frame 4's MS Video 1 chunk */
  uint8_t *format = 0, *chunk = 0;
  uint32_t chunk_size = 0, seen = 0;
  for (long i = 0; i + 8 < size; i++) {
    if (!format && !memcmp(file + i, "strf", 4))
      format = file + i + 8;
    if (!memcmp(file + i, "00dc", 4) && seen++ == 4) {
      chunk = file + i + 8;
      memcpy(&chunk_size, file + i + 4, 4);
      break;
    }
  }
  assert(format && chunk);
  uint8_t info[40 + 1024];
  memcpy(info, format, sizeof(info));
  memcpy(info + 20, &chunk_size, 4); /* biSizeImage of this frame */
  uintptr_t surface = CreateWindowExA(0, "Owner", "dd", 0x10000000u, 0, 0, 32,
                                      16, 0, 0, 0, 0);
  uintptr_t dd = DrawDibOpen();
  assert(dd);
  const uint8_t entry[4] = {1,2,3,0};
  assert(!DrawDibChangePalette(dd, 2147483647, 1, entry));
  assert(!DrawDibChangePalette(dd, 1, 2147483647, entry));
  assert(!DrawDibChangePalette(dd, 2147483647, 2147483647, entry));
  assert(!DrawDibChangePalette(dd, -1, 1, entry));
  assert(!DrawDibChangePalette(dd, 256, 1, entry));
  assert(DrawDibChangePalette(dd, 256, 0, 0));
  assert(DrawDibChangePalette(dd, 255, 1, entry));
  assert(DrawDibBegin(dd, GetDC(surface), -1, -1, info, -1, -1, 0));
  assert(DrawDibDraw(dd, GetDC(surface), 0, 0, -1, -1, info, chunk, 0, 0, -1,
                     -1, 0));
  dump(surface, "drawdib-frame4");
  uint8_t decoded[40 + 1024];
  assert(DrawDibGetBuffer(dd, decoded, sizeof(decoded), 0));
  out("DRAWDIB buffer bits=%u compression=%u\n", decoded[14],
      decoded[16] | decoded[17] << 8);
  memset(win(surface)->pixels, 0, 32 * 16 * 4);
  assert(DrawDibDraw(dd, GetDC(surface), 0, 0, -1, -1, 0, 0, 0, 0, -1, -1,
                     0x0002)); /* DDF_UPDATE redraws the last frame */
  dump(surface, "drawdib-update");
  assert(!DrawDibDraw(dd + 7, GetDC(surface), 0, 0, -1, -1, info, chunk, 0, 0,
                      -1, -1, 0));
  info[20] = 1; /* truncated compressed frame: refused, nothing drawn */
  info[21] = info[22] = info[23] = 0;
  assert(!DrawDibDraw(dd, GetDC(surface), 0, 0, -1, -1, info, chunk, 0, 0, -1,
                      -1, 0));
  assert(DrawDibEnd(dd) && DrawDibClose(dd) && !DrawDibClose(dd));
  DestroyWindow(surface);
  free(file);
}

static void mciwnd(void) {
  out("== MCIWnd\n");
  assert(MCIWndRegisterClass());
  uintptr_t owner = CreateWindowExA(0, "Owner", "owner", 0x10000000u, 0, 0,
                                    200, 100, 0, 0, 0, 0);
  /* WS_CHILD | WS_VISIBLE | MCIWNDF_NOPLAYBAR | MCIWNDF_NOMENU |
   * MCIWNDF_NOTIFYMODE | MCIWNDF_NOTIFYMEDIA */
  uintptr_t h = MCIWndCreateA(owner, 0, 0x50000000u | 0x0002 | 0x0008 |
                                            0x0100 | 0x0880,
                              "clip-pcm.avi");
  assert(h);
  struct window *w = win(h);
  out("MCIWND hwnd=%lx client=%dx%d\n", (unsigned long)h, w->w, w->h);
  intptr_t id = SendMessageA(h, 0x0400 + 100, 0, 0);
  out("MCIWND device=%ld length=%ld\n", (long)id,
      (long)SendMessageA(h, 0x0400 + 104, 0, 0));
  char text[64];
  SendMessageA(h, 0x0400 + 125, sizeof(text), (intptr_t)text);
  out("MCIWND device type '%s'\n", text);
  assert(!SendMessageA(h, 0x0806, 0, 0)); /* MCIWndPlay */
  run(600);
  intptr_t mode = SendMessageA(h, 0x0400 + 106, sizeof(text), (intptr_t)text);
  out("MCIWND mode=%ld '%s' position=%ld\n", (long)mode, text,
      (long)SendMessageA(h, 0x0400 + 102, 0, 0));
  dump(h, "mciwnd-last");
  assert(!SendMessageA(h, 0x0807, 0, 2)); /* MCIWndSeek(hwnd, 2) */
  SendMessageA(h, 0x0400 + 102, sizeof(text), (intptr_t)text);
  out("MCIWND position '%s'\n", text);
  assert(!SendMessageA(h, 0x0400 + 119, 0, (intptr_t) "ms"));
  out("MCIWND length ms=%ld\n", (long)SendMessageA(h, 0x0400 + 104, 0, 0));
  assert(!SendMessageA(h, 0x0400 + 101, 0, (intptr_t) "status mode"));
  SendMessageA(h, 0x0400 + 138, sizeof(text), (intptr_t)text);
  out("MCIWND sendstring returned '%s'\n", text);
  struct rect r;
  assert(!SendMessageA(h, 0x0400 + 142, 0, (intptr_t)&r));
  out("MCIWND dest %d %d %d %d\n", r.left, r.top, r.right, r.bottom);
  intptr_t e = SendMessageA(h, 0x0400 + 153, 0, (intptr_t) "missing.avi");
  SendMessageA(h, 0x0400 + 128, sizeof(text), (intptr_t)text);
  out("MCIWND open missing=%ld error '%s'\n", (long)e, text);
  SendMessageA(h, 0x0010, 0, 0); /* MCIWndDestroy */
  assert(!win(h));
  DestroyWindow(owner);
}

/* Fuzz: mutated authored AVI files through open/play/seek/step/where/close
 * (avifil32 parser, ICM decoders, ACM, DrawDib, gdi32) under ASan/UBSan. */
static uint32_t seed = 0xa71f00d5u;
static uint32_t rnd(uint32_t n) {
  seed = seed * 1664525u + 1013904223u;
  return n ? (seed >> 8) % n : 0;
}
static int fuzz(uint32_t iterations) {
  const char *seeds[2] = {"clip-pcm.avi", "clip-ima.avi"};
  uint8_t *data[2];
  long sizes[2];
  for (int i = 0; i < 2; i++) {
    int fd = hf_open(seeds[i]);
    assert(fd >= 0);
    sizes[i] = hf_size(fd);
    data[i] = malloc((size_t)sizes[i] + 64);
    assert(data[i] && hf_read(fd, data[i], (unsigned long)sizes[i]) == sizes[i]);
    hf_close(fd);
  }
  uint8_t *buffer = malloc((size_t)sizes[0] + sizes[1] + 64);
  uint32_t opened = 0;
  for (uint32_t it = 0; it < iterations; it++) {
    int k = (int)rnd(2);
    long n = sizes[k];
    memcpy(buffer, data[k], (size_t)n);
    for (uint32_t m = 1 + rnd(6); m; m--) {
      uint32_t at = rnd((uint32_t)n), kind = rnd(4);
      if (kind == 0)
        buffer[at] ^= (uint8_t)(1u << rnd(8));
      else if (kind == 1)
        buffer[at] = (uint8_t)rnd(256);
      else if (kind == 2 && n > 16)
        n = 12 + rnd((uint32_t)n - 12);
      else if (at + 4 <= (uint32_t)n) {
        uint32_t v = rnd(2) ? rnd(64) : 0xffffffffu - rnd(4);
        memcpy(buffer + at, &v, 4);
      }
    }
    assert(!hf_write_file("fz.avi", buffer, (unsigned long)n));
    if (mciSendStringA("open fz.avi alias z", 0, 0, 0))
      continue;
    opened++;
    char result[64];
    mciSendStringA("status z length", result, sizeof(result), 0);
    mciSendStringA("play z notify", 0, 0, 0x4321);
    run(40);
    mciSendStringA("step z by 3", 0, 0, 0);
    mciSendStringA("seek z to end", 0, 0, 0);
    mciSendStringA("where z source", result, sizeof(result), 0);
    mciSendStringA("put z destination at 0 0 7 5", 0, 0, 0);
    mciSendStringA("seek z to start", 0, 0, 0);
    mciSendStringA("close z", 0, 0, 0);
    pump();
  }
  for (int i = 0; i < 2; i++)
    free(data[i]);
  free(buffer);
  printf("PASS AVI fuzz iterations=%u opened=%u\n", iterations, opened);
  return 0;
}
extern unsigned mci_notifications, mci_last_notify;
static void partial_oracle(const char *file, uint32_t first, uint32_t end) {
  int fd = hf_open(file); assert(fd >= 0);
  int16_t *want = malloc((end-first)*2); assert(want);
  assert(hf_seek(fd, first*2, 0) == (long)first*2);
  assert(hf_read(fd, want, (end-first)*2) == (long)(end-first)*2); hf_close(fd);
  size_t start = 0;
  while (start < capture_frames && capture[2*start] != want[0]) start++;
  assert(start+end-first <= capture_frames);
  for (size_t i = 0; i < end-first; i++)
    assert(capture[2*(start+i)] == want[i] && capture[2*(start+i)+1] == want[i]);
  /* No queued sample may leak past the exclusive endpoint. */
  for (size_t i = start+end-first; i < capture_frames; i++)
    assert(capture[2*i] == 0 && capture[2*i+1] == 0);
  free(want); free(capture); capture = 0;
}
static void amendment2(void) {
  FILE *saved = trace; trace = tmpfile(); assert(trace);
  for (unsigned kind = 0; kind < 2; kind++) {
    const char *file = kind ? "clip-ima.pcm" : "clip-pcm.pcm";
    assert(!mciSendStringA(kind ? "open clip-ima.avi alias a" : "open clip-pcm.avi alias a", 0, 0, 0));
    oracle_begin();
    assert(!mciSendStringA("play a from 1 to 1 wait", 0, 0, 0));
    run(300); partial_oracle(file, 1470, 2940);
    assert(!mciSendStringA("close a", 0, 0, 0));
  }
  assert(!mciSendStringA("open bad-audio-first.avi alias a", 0, 0, 0));
  unsigned before = mci_notifications;
  assert(mciSendStringA("play a notify", 0, 0, 0x55) == MCIERR_INVALID_FILE);
  assert(mci_notifications == before);
  assert(!mciSendStringA("close a", 0, 0, 0));
  const char *bad[] = {"bad-video.avi", "bad-audio-late.avi"};
  for (unsigned kind = 0; kind < 2; kind++) {
    char cmd[128]; snprintf(cmd, sizeof(cmd), "open %s alias a", bad[kind]);
    assert(!mciSendStringA(cmd, 0, 0, 0));
    if (!kind) {
      before = mci_notifications;
      assert(mciSendStringA("play a from 1 notify", 0, 0, 0x55) == MCIERR_INVALID_FILE);
      assert(mci_notifications == before);
    }
    before = mci_notifications;
    assert(!mciSendStringA("play a notify", 0, 0, 0x55));
    run(4000);
    assert(mci_notifications == before + 1 && mci_last_notify == MCI_NOTIFY_FAILURE);
    assert(mciSendStringA("play a from 0 wait", 0, 0, 0) == MCIERR_INVALID_FILE);
    assert(!mciSendStringA("close a", 0, 0, 0));
  }
  assert(mciSendStringA("play bad-video.avi wait", 0, 0, 0) == MCIERR_INVALID_FILE);
  assert(!mciGetDeviceIDA("bad-video.avi"));
  assert(!mciSendStringA("open clip-pcm.avi alias a", 0, 0, 0));
  assert(!mciSendStringA("play a", 0, 0, 0));
  unsigned sources = engine.source_count;
  engine.callback_count = WA_CALLBACKS;
  assert(!mciSendStringA("close a", 0, 0, 0));
  assert(engine.source_count == sources && live_allocations > 0);
  mci_service_pass(); assert(engine.source_count == sources);
  engine.callback_count = 0;
  for (unsigned i = 0; i < 8; i++) { wa_dispatch(&engine); mci_service_pass(); }
  assert(engine.source_count == 0 && !live_allocations);
  pump(); fclose(trace); trace = saved;
}

int main(int argc, char **argv) {
  if ((argc != 3 && argc != 4) || hf_chdir(argv[1]))
    return 2;
  if (argc == 4) {
    trace = fopen("avi-fuzz.trace", "w");
    assert(trace);
    struct wndclass owner = {0};
    owner.proc = (uintptr_t)owner_proc;
    owner.name = (uintptr_t) "Owner";
    RegisterClassA(&owner);
    int r = fuzz((uint32_t)strtoul(argv[3], 0, 10));
    mciSendStringA("close all", 0, 0, 0);
    for (int i = 0; i < 40; i++)
      Sleep(5);
    fclose(trace);
    ag_exit();
    if (heap_live || live_allocations || ag_live()) {
      fprintf(stderr, "FAIL leaked heap=%ld audio=%ld gdi=%ld\n", heap_live,
              live_allocations, ag_live());
      return 1;
    }
    return r;
  }
  trace = fopen(argv[2], "w");
  assert(trace);
  struct wndclass owner = {0};
  owner.proc = (uintptr_t)owner_proc;
  owner.name = (uintptr_t) "Owner";
  RegisterClassA(&owner);
  avivideo();
  drawdib();
  mciwnd();
  amendment2();
  mciSendStringA("close all", 0, 0, 0);
  for (int i = 0; i < 40; i++)
    Sleep(5);
  out("LIVE heap=%ld audio=%ld\n", heap_live, live_allocations);
  fclose(trace);
  ag_exit();
  if (heap_live || live_allocations || ag_live()) {
    fprintf(stderr, "FAIL leaked heap=%ld audio=%ld gdi=%ld\n", heap_live,
            live_allocations, ag_live());
    return 1;
  }
  printf("PASS AVI avivideo DrawDib MCIWnd\n");
  return 0;
}
