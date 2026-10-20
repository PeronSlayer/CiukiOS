/* SPDX-License-Identifier: LGPL-2.1-or-later
 * msvfw32 MCIWnd subset (f5-12B; census: MCIWndCreateA and
 * MCIWndRegisterClass): the "MCIWndClass" window over the winmm MCI command
 * interface. Messages follow the Video for Windows MCIWnd reference (vfw.h
 * MCIWNDM_* = WM_USER + n and the MCI_* command messages); the window
 * draws the movie in its client area (MCI "window handle"), sizes itself
 * to the movie unless MCIWNDF_NOAUTOSIZEWINDOW, and notifies its owner of
 * mode/position/size/media/error changes as requested by MCIWNDF_NOTIFY*.
 * No toolbar, trackbar, menu or error dialog is drawn: every window
 * behaves as MCIWNDF_NOPLAYBAR | MCIWNDF_NOMENU | MCIWNDF_NOERRORDLG. */
#include "codec.h"
#ifndef W32_HOST_MODEL
#define IMPORT __declspec(dllimport)
#else
#define IMPORT
#endif
struct wnd_class {
  uint32_t style;
  uintptr_t proc;
  int32_t cls_extra, wnd_extra;
  uintptr_t instance, icon, cursor, background, menu, name;
};
struct create {
  uintptr_t params, instance, menu, parent;
  int32_t cy, cx, y, x, style;
  uintptr_t name, klass;
  uint32_t ex_style;
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
/* user32 and winmm are bound at first use (see drawdib.c): the codec
 * images import kernel32 only. Names: user32/winmm exports. */
#define API_LIST(X)                                                            \
  X(user, RegisterClassA, uint16_t, (const struct wnd_class *))                \
  X(user, CreateWindowExA, uintptr_t,                                          \
    (uint32_t, const char *, const char *, uint32_t, int, int, int, int,       \
     uintptr_t, uintptr_t, uintptr_t, void *))                                 \
  X(user, DestroyWindow, int, (uintptr_t))                                     \
  X(user, DefWindowProcA, intptr_t, (uintptr_t, uint32_t, uintptr_t, intptr_t)) \
  X(user, SendMessageA, intptr_t, (uintptr_t, uint32_t, uintptr_t, intptr_t))  \
  X(user, GetParent, uintptr_t, (uintptr_t))                                   \
  X(user, GetClientRect, int, (uintptr_t, struct rect *))                      \
  X(user, SetWindowPos, int,                                                   \
    (uintptr_t, uintptr_t, int, int, int, int, uint32_t))                      \
  X(user, AdjustWindowRect, int, (struct rect *, uint32_t, int))               \
  X(user, BeginPaint, uintptr_t, (uintptr_t, struct paint *))                  \
  X(user, EndPaint, int, (uintptr_t, const struct paint *))                    \
  X(user, SetTimer, uintptr_t, (uintptr_t, uintptr_t, uint32_t, uintptr_t))    \
  X(user, KillTimer, int, (uintptr_t, uintptr_t))                              \
  X(user, GetWindowLongA, int32_t, (uintptr_t, int))                           \
  X(winmm, mciSendCommandA, uint32_t,                                          \
    (uint32_t, uint32_t, uintptr_t, uintptr_t))                                \
  X(winmm, mciSendStringA, uint32_t, (const char *, char *, uint32_t,          \
                                      uintptr_t))                              \
  X(winmm, mciGetErrorStringA, int, (uint32_t, char *, uint32_t))
#ifdef W32_HOST_MODEL
#define X(dll, name, ret, args) ret name args;
API_LIST(X)
#undef X
#else
IMPORT void *W32API GetModuleHandleA(const char *);
IMPORT void *W32API LoadLibraryA(const char *);
IMPORT void *W32API GetProcAddress(void *, const char *);
#endif
static struct {
#define X(dll, name, ret, args) ret(W32API *name) args;
  API_LIST(X)
#undef X
} api;
static int bind(void) {
  static volatile uint32_t state;
#ifdef W32_HOST_MODEL
  if (!state) {
#define X(dll, name, ret, args) api.name = name;
    API_LIST(X)
#undef X
    state = 1;
  }
#else
  if (!state) {
    void *user = GetModuleHandleA("user32.dll"),
         *winmm = GetModuleHandleA("winmm.dll");
    if (!user)
      user = LoadLibraryA("user32.dll");
    if (!winmm)
      winmm = LoadLibraryA("winmm.dll");
    uint32_t ok = user && winmm;
#define X(dll, name, ret, args)                                                \
  api.name = ok ? (ret(W32API *) args)GetProcAddress(dll, #name) : 0;          \
  ok = ok && api.name;
    API_LIST(X)
#undef X
    state = ok ? 1 : 2;
  }
#endif
  return state == 1;
}
#define RegisterClassA api.RegisterClassA
#define CreateWindowExA api.CreateWindowExA
#define DestroyWindow api.DestroyWindow
#define DefWindowProcA api.DefWindowProcA
#define SendMessageA api.SendMessageA
#define GetParent api.GetParent
#define GetClientRect api.GetClientRect
#define SetWindowPos api.SetWindowPos
#define AdjustWindowRect api.AdjustWindowRect
#define BeginPaint api.BeginPaint
#define EndPaint api.EndPaint
#define SetTimer api.SetTimer
#define KillTimer api.KillTimer
#define GetWindowLongA api.GetWindowLongA
#define mciSendCommandA api.mciSendCommandA
#define mciSendStringA api.mciSendStringA
#define mciGetErrorStringA api.mciGetErrorStringA
IMPORT void *W32API GetModuleHandleA(const char *);

#define WM_USER 0x0400u
#define MCIWNDM_GETDEVICEID (WM_USER + 100)
#define MCIWNDM_SENDSTRINGA (WM_USER + 101)
#define MCIWNDM_GETPOSITIONA (WM_USER + 102)
#define MCIWNDM_GETSTART (WM_USER + 103)
#define MCIWNDM_GETLENGTH (WM_USER + 104)
#define MCIWNDM_GETEND (WM_USER + 105)
#define MCIWNDM_GETMODEA (WM_USER + 106)
#define MCIWNDM_EJECT (WM_USER + 107)
#define MCIWNDM_SETZOOM (WM_USER + 108)
#define MCIWNDM_GETZOOM (WM_USER + 109)
#define MCIWNDM_SETVOLUME (WM_USER + 110)
#define MCIWNDM_GETVOLUME (WM_USER + 111)
#define MCIWNDM_SETSPEED (WM_USER + 112)
#define MCIWNDM_GETSPEED (WM_USER + 113)
#define MCIWNDM_SETREPEAT (WM_USER + 114)
#define MCIWNDM_GETREPEAT (WM_USER + 115)
#define MCIWNDM_REALIZE (WM_USER + 118)
#define MCIWNDM_SETTIMEFORMATA (WM_USER + 119)
#define MCIWNDM_GETTIMEFORMATA (WM_USER + 120)
#define MCIWNDM_VALIDATEMEDIA (WM_USER + 121)
#define MCIWNDM_PLAYFROM (WM_USER + 122)
#define MCIWNDM_PLAYTO (WM_USER + 123)
#define MCIWNDM_GETFILENAMEA (WM_USER + 124)
#define MCIWNDM_GETDEVICEA (WM_USER + 125)
#define MCIWNDM_GETPALETTE (WM_USER + 126)
#define MCIWNDM_SETPALETTE (WM_USER + 127)
#define MCIWNDM_GETERRORA (WM_USER + 128)
#define MCIWNDM_SETTIMERS (WM_USER + 129)
#define MCIWNDM_SETACTIVETIMER (WM_USER + 130)
#define MCIWNDM_SETINACTIVETIMER (WM_USER + 131)
#define MCIWNDM_GETACTIVETIMER (WM_USER + 132)
#define MCIWNDM_GETINACTIVETIMER (WM_USER + 133)
#define MCIWNDM_NEWA (WM_USER + 134)
#define MCIWNDM_CHANGESTYLES (WM_USER + 135)
#define MCIWNDM_GETSTYLES (WM_USER + 136)
#define MCIWNDM_GETALIAS (WM_USER + 137)
#define MCIWNDM_RETURNSTRINGA (WM_USER + 138)
#define MCIWNDM_PLAYREVERSE (WM_USER + 139)
#define MCIWNDM_GET_SOURCE (WM_USER + 140)
#define MCIWNDM_PUT_SOURCE (WM_USER + 141)
#define MCIWNDM_GET_DEST (WM_USER + 142)
#define MCIWNDM_PUT_DEST (WM_USER + 143)
#define MCIWNDM_CAN_PLAY (WM_USER + 144)
#define MCIWNDM_CAN_WINDOW (WM_USER + 145)
#define MCIWNDM_CAN_RECORD (WM_USER + 146)
#define MCIWNDM_CAN_SAVE (WM_USER + 147)
#define MCIWNDM_CAN_EJECT (WM_USER + 148)
#define MCIWNDM_CAN_CONFIG (WM_USER + 149)
#define MCIWNDM_PALETTEKICK (WM_USER + 150)
#define MCIWNDM_OPENINTERFACE (WM_USER + 151)
#define MCIWNDM_SETOWNER (WM_USER + 152)
#define MCIWNDM_OPENA (WM_USER + 153)
#define MCIWNDM_SENDSTRINGW (WM_USER + 201)
#define MCIWNDM_GETPOSITIONW (WM_USER + 202)
#define MCIWNDM_GETMODEW (WM_USER + 206)
#define MCIWNDM_SETTIMEFORMATW (WM_USER + 219)
#define MCIWNDM_GETTIMEFORMATW (WM_USER + 220)
#define MCIWNDM_GETFILENAMEW (WM_USER + 224)
#define MCIWNDM_GETDEVICEW (WM_USER + 225)
#define MCIWNDM_GETERRORW (WM_USER + 228)
#define MCIWNDM_NEWW (WM_USER + 234)
#define MCIWNDM_RETURNSTRINGW (WM_USER + 238)
#define MCIWNDM_OPENW (WM_USER + 252)
#define MCIWNDM_NOTIFYMODE (WM_USER + 200)
#define MCIWNDM_NOTIFYPOS (WM_USER + 201)
#define MCIWNDM_NOTIFYSIZE (WM_USER + 202)
#define MCIWNDM_NOTIFYMEDIA (WM_USER + 203)
#define MCIWNDM_NOTIFYERROR (WM_USER + 205)
#define MCIWNDF_NOAUTOSIZEWINDOW 0x0001u
#define MCIWNDF_NOAUTOSIZEMOVIE 0x0004u
#define MCIWNDF_NOTIFYMODE 0x0100u
#define MCIWNDF_NOTIFYPOS 0x0200u
#define MCIWNDF_NOTIFYSIZE 0x0400u
#define MCIWNDF_NOTIFYMEDIA 0x0880u
#define MCIWNDF_NOTIFYERROR 0x1000u
#define MCIWNDF_NOTIFYANSI 0x0080u
#define MCIWND_START (-1)
#define MCIWND_END (-2)
#define WS_CHILD 0x40000000u
#define WS_POPUP 0x80000000u
#define WS_VISIBLE 0x10000000u
#define WS_BORDER 0x00800000u
#define WS_OVERLAPPEDWINDOW 0x00CF0000u
#define MCI_OPEN 0x0803u
#define MCI_CLOSE 0x0804u
#define MCI_PLAY 0x0806u
#define MCI_SEEK 0x0807u
#define MCI_STOP 0x0808u
#define MCI_PAUSE 0x0809u
#define MCI_GETDEVCAPS 0x080Bu
#define MCI_SET 0x080Du
#define MCI_STEP 0x080Eu
#define MCI_STATUS 0x0814u
#define MCI_REALIZE 0x0840u
#define MCI_WINDOW 0x0841u
#define MCI_PUT 0x0842u
#define MCI_WHERE 0x0843u
#define MCI_UPDATE 0x0854u
#define MCI_RESUME 0x0855u
#define MM_MCINOTIFY 0x03B9u

struct mciwnd {
  uintptr_t window, owner;
  uint32_t flags, device, devtype, mode, position, repeat, zoom;
  uint32_t active, inactive, error;
  char file[260], device_name[32], returned[128];
};
#define MCIWNDS 16u
static struct mciwnd *windows[MCIWNDS];
static uint16_t registered;
static void *heap_alloc(uint32_t n);
static void heap_free(void *p);
IMPORT void *W32API GetProcessHeap(void);
IMPORT void *W32API HeapAlloc(void *, uint32_t, uint32_t);
IMPORT int W32API HeapFree(void *, uint32_t, void *);
static void *heap_alloc(uint32_t n) {
  return HeapAlloc(GetProcessHeap(), 8, n);
}
static void heap_free(void *p) {
  if (p)
    HeapFree(GetProcessHeap(), 0, p);
}
static struct mciwnd *find(uintptr_t h) {
  for (unsigned i = 0; i < MCIWNDS; i++)
    if (windows[i] && windows[i]->window == h)
      return windows[i];
  return 0;
}
static uint32_t text_copy(char *out, uint32_t cap, const char *s) {
  uint32_t i = 0;
  if (!out || !cap)
    return 0;
  for (; s[i] && i + 1 < cap; i++)
    out[i] = s[i];
  out[i] = 0;
  return i;
}
static void wide_copy(WCHAR *out, uint32_t cap, const char *s) {
  uint32_t i = 0;
  if (!out || !cap)
    return;
  for (; s[i] && i + 1 < cap; i++)
    out[i] = (uint8_t)s[i];
  out[i] = 0;
}
static int narrow(const WCHAR *w, char *out, uint32_t cap) {
  uint32_t i = 0;
  if (!w)
    return 0;
  for (; w[i]; i++) {
    if (i + 1 >= cap || w[i] > 255)
      return 0;
    out[i] = (char)w[i];
  }
  out[i] = 0;
  return 1;
}
static void notify(struct mciwnd *m, uint32_t message, intptr_t value) {
  if (m->owner)
    SendMessageA(m->owner, message, m->window, value);
}
static uint32_t command(struct mciwnd *m, uint32_t msg, uint32_t flags,
                        void *parms) {
  if (!m->device)
    return m->error = 263; /* MCIERR_INVALID_DEVICE_NAME */
  uint32_t e = mciSendCommandA(m->device, msg, flags, (uintptr_t)parms);
  m->error = e;
  if (e && (m->flags & MCIWNDF_NOTIFYERROR))
    notify(m, MCIWNDM_NOTIFYERROR, (intptr_t)e);
  return e;
}
static uint32_t status(struct mciwnd *m, uint32_t item, uint32_t flags) {
  struct {
    uintptr_t callback, value;
    uint32_t item, track;
  } s = {0, 0, item, 0};
  if (!m->device || mciSendCommandA(m->device, MCI_STATUS, 0x100u | flags,
                                    (uintptr_t)&s))
    return 0;
  return (uint32_t)s.value;
}
static void check_mode(struct mciwnd *m) {
  uint32_t mode = m->device ? status(m, 4, 0) : 524;
  if (mode != m->mode) {
    m->mode = mode;
    if (m->flags & MCIWNDF_NOTIFYMODE)
      notify(m, MCIWNDM_NOTIFYMODE, (intptr_t)mode);
  }
  uint32_t position = m->device ? status(m, 2, 0) : 0;
  if (position != m->position) {
    m->position = position;
    if (m->flags & MCIWNDF_NOTIFYPOS)
      notify(m, MCIWNDM_NOTIFYPOS, (intptr_t)position);
  }
}
static void fit(struct mciwnd *m) {
  struct rect c;
  struct {
    uintptr_t callback;
    struct rect r;
  } p = {0, {0, 0, 0, 0}};
  if (!m->device || m->devtype != 520) /* digitalvideo only */
    return;
  if (!(m->flags & MCIWNDF_NOAUTOSIZEWINDOW) &&
      !mciSendCommandA(m->device, MCI_WHERE, 0x20000u | 0x400000u,
                       (uintptr_t)&p)) { /* source, max: the movie size */
    struct rect r = {0, 0, p.r.right * (int32_t)m->zoom / 100,
                     p.r.bottom * (int32_t)m->zoom / 100};
    uint32_t style = (uint32_t)GetWindowLongA(m->window, -16);
    if (!(style & WS_CHILD))
      AdjustWindowRect(&r, style, 0);
    else if (style & WS_BORDER) {
      r.right += 2;
      r.bottom += 2;
    }
    SetWindowPos(m->window, 0, 0, 0, r.right - r.left, r.bottom - r.top,
                 0x0016); /* NOMOVE | NOZORDER | NOACTIVATE */
  }
  if (!(m->flags & MCIWNDF_NOAUTOSIZEMOVIE)) {
    GetClientRect(m->window, &c);
    p.r = (struct rect){0, 0, c.right - c.left, c.bottom - c.top};
    if (p.r.right > 0 && p.r.bottom > 0)
      mciSendCommandA(m->device, MCI_PUT, 0x40000u | 0x10000u,
                      (uintptr_t)&p);
  }
}
static void close_device(struct mciwnd *m) {
  if (m->device) {
    mciSendCommandA(m->device, MCI_CLOSE, 0, 0);
    m->device = 0;
    m->file[0] = 0;
    check_mode(m);
  }
}
static uint32_t open_file(struct mciwnd *m, const char *file) {
  struct {
    uintptr_t callback;
    uint32_t device;
    uintptr_t type, element, alias;
    uint32_t style;
    uintptr_t parent;
  } o = {0, 0, 0, (uintptr_t)file, 0, WS_CHILD, m->window};
  close_device(m);
  m->error = mciSendCommandA(0, MCI_OPEN, 0x200u | 0x10000u | 0x20000u,
                             (uintptr_t)&o); /* element, ws, parent */
  if (m->error == 274 || m->error == 277) /* not a windowed device */
    m->error = mciSendCommandA(0, MCI_OPEN, 0x200u, (uintptr_t)&o);
  if (m->error) {
    if (m->flags & MCIWNDF_NOTIFYERROR)
      notify(m, MCIWNDM_NOTIFYERROR, (intptr_t)m->error);
    return m->error;
  }
  m->device = o.device;
  text_copy(m->file, sizeof(m->file), file);
  struct {
    uintptr_t callback;
    uint32_t value, item;
  } caps = {0, 0, 4}; /* MCI_GETDEVCAPS_DEVICE_TYPE */
  if (!mciSendCommandA(m->device, MCI_GETDEVCAPS, 0x100u, (uintptr_t)&caps))
    m->devtype = caps.value;
  if (m->devtype == 520) {
    struct {
      uintptr_t callback, window;
      uint32_t show;
      uintptr_t text;
    } w = {0, m->window, 0, 0};
    mciSendCommandA(m->device, MCI_WINDOW, 0x10000u, (uintptr_t)&w);
  }
  text_copy(m->device_name, sizeof(m->device_name),
            m->devtype == 520   ? "AVIVideo"
            : m->devtype == 522 ? "WaveAudio"
            : m->devtype == 523 ? "Sequencer"
                                : "Other");
  fit(m);
  if (m->flags & 0x0800u)
    notify(m, MCIWNDM_NOTIFYMEDIA, (intptr_t)m->file);
  check_mode(m);
  return 0;
}
static uint32_t play(struct mciwnd *m, uint32_t flags, uint32_t from,
                     uint32_t to) {
  struct {
    uintptr_t callback;
    uint32_t from, to;
  } p = {m->window, from, to};
  if (m->repeat && m->devtype == 520)
    flags |= 0x40000u; /* MCI_DGV_PLAY_REPEAT */
  uint32_t e = command(m, MCI_PLAY, flags | 1u, &p);
  check_mode(m);
  return e;
}
static intptr_t W32API proc(uintptr_t h, uint32_t msg, uintptr_t wp,
                            intptr_t lp);
int MCIWndRegisterClass(void) {
  if (registered)
    return 1;
  if (!bind())
    return 0;
  struct wnd_class c = {0};
  c.proc = (uintptr_t)proc;
  c.instance = (uintptr_t)GetModuleHandleA("msvfw32.dll");
  c.name = (uintptr_t) "MCIWndClass";
  c.style = 0x0003; /* CS_VREDRAW | CS_HREDRAW */
  registered = RegisterClassA(&c);
  return registered != 0;
}
uintptr_t MCIWndCreateA(uintptr_t parent, uintptr_t instance, uint32_t style,
                        const char *file) {
  if (!MCIWndRegisterClass())
    return 0;
  uint32_t ws = style & 0xffff0000u;
  if (!(ws & (WS_CHILD | WS_POPUP | 0x00C00000u)))
    ws |= parent ? (WS_CHILD | WS_BORDER | WS_VISIBLE)
                 : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
  struct {
    uint32_t flags;
    const char *file;
  } params = {style & 0xffffu, file};
  return CreateWindowExA(0, "MCIWndClass", file ? file : "", ws,
                         (int)0x80000000, 0, 0, 0, parent, 0, instance,
                         &params);
}
uintptr_t MCIWndCreateW(uintptr_t parent, uintptr_t instance, uint32_t style,
                        const WCHAR *file) {
  char text[260];
  if (file && !narrow(file, text, sizeof(text)))
    return 0;
  return MCIWndCreateA(parent, instance, style, file ? text : 0);
}
static intptr_t message(struct mciwnd *m, uint32_t msg, uintptr_t wp,
                        intptr_t lp) {
  char text[260];
  switch (msg) {
  case MCI_PLAY:
    return play(m, 0, 0, 0);
  case MCIWNDM_PLAYFROM:
    return play(m, 4u, (uint32_t)lp, 0);
  case MCIWNDM_PLAYTO:
    return play(m, 8u, 0, (uint32_t)lp);
  case MCIWNDM_PLAYREVERSE:
    return play(m, 0x20000u, 0, 0);
  case MCI_STOP:
  case MCI_PAUSE:
  case MCI_RESUME: {
    uint32_t e = command(m, msg, 0, 0);
    check_mode(m);
    return e;
  }
  case MCI_SEEK: {
    struct {
      uintptr_t callback;
      uint32_t to;
    } s = {0, (uint32_t)lp};
    uint32_t flags = lp == MCIWND_START ? 0x100u
                     : lp == MCIWND_END ? 0x200u
                                        : 8u;
    uint32_t e = command(m, MCI_SEEK, flags, &s);
    check_mode(m);
    return e;
  }
  case MCI_STEP: {
    struct {
      uintptr_t callback;
      uint32_t frames;
    } s = {0, (uint32_t)(lp < 0 ? -lp : lp)};
    return command(m, MCI_STEP, 0x20000u | (lp < 0 ? 0x10000u : 0), &s);
  }
  case MCI_CLOSE:
    close_device(m);
    return 0;
  case MCIWNDM_GETDEVICEID:
  case MCIWNDM_GETALIAS:
    return m->device;
  case MCIWNDM_GETLENGTH:
    return status(m, 1, 0);
  case MCIWNDM_GETSTART:
    return status(m, 2, 0x200u);
  case MCIWNDM_GETEND:
    return status(m, 2, 0x200u) + status(m, 1, 0);
  case MCIWNDM_GETPOSITIONA:
  case MCIWNDM_GETPOSITIONW:
  case MCIWNDM_GETMODEA:
  case MCIWNDM_GETMODEW:
  case MCIWNDM_GETTIMEFORMATA:
  case MCIWNDM_GETTIMEFORMATW: {
    int position = msg == MCIWNDM_GETPOSITIONA || msg == MCIWNDM_GETPOSITIONW;
    int mode = msg == MCIWNDM_GETMODEA || msg == MCIWNDM_GETMODEW;
    uint32_t item = position ? 2 : mode ? 4 : 6;
    if (lp && wp && m->device) {
      uint32_t v = status(m, item, 0);
      const char *word = 0;
      static const char *const modes[] = {"not ready", "stopped", "playing",
                                          "recording", "seeking", "paused",
                                          "open"};
      static const char *const formats[] = {"milliseconds", "hms", "msf",
                                            "frames", "smpte 24", "smpte 25",
                                            "smpte 30", "smpte 30 drop",
                                            "bytes", "samples", "tmsf"};
      if (mode && v >= 524 && v <= 530)
        word = modes[v - 524];
      else if (!position && !mode && v <= 10)
        word = formats[v];
      char number[12];
      if (!word) {
        uint32_t x = v, d = 0;
        char rev[12];
        do
          rev[d++] = (char)('0' + x % 10);
        while ((x /= 10) && d < 11);
        for (uint32_t i = 0; i < d; i++)
          number[i] = rev[d - 1 - i];
        number[d] = 0;
        word = number;
      }
      if (msg == MCIWNDM_GETPOSITIONW || msg == MCIWNDM_GETMODEW ||
          msg == MCIWNDM_GETTIMEFORMATW)
        wide_copy((WCHAR *)lp, (uint32_t)wp, word);
      else
        text_copy((char *)lp, (uint32_t)wp, word);
    }
    return status(m, item, 0);
  }
  case MCIWNDM_SETTIMEFORMATA:
  case MCIWNDM_SETTIMEFORMATW: {
    struct {
      uintptr_t callback;
      uint32_t format, audio, extra[12];
    } s = {0};
    const char *f = (const char *)lp;
    if (msg == MCIWNDM_SETTIMEFORMATW) {
      if (!narrow((const WCHAR *)lp, text, sizeof(text)))
        return 290;
      f = text;
    }
    if (!f)
      return 290;
    s.format = (f[0] == 'f' || f[0] == 'F') ? 3u : 0u; /* frames | ms */
    return command(m, MCI_SET, 0x400u, &s);
  }
  case MCIWNDM_SENDSTRINGA:
  case MCIWNDM_SENDSTRINGW: {
    /* "verb args" -> "verb <alias> args"; the device is opened without an
     * alias, so the element name is its MCI name. */
    const char *s = (const char *)lp;
    char cmd[512];
    if (msg == MCIWNDM_SENDSTRINGW) {
      if (!narrow((const WCHAR *)lp, text, sizeof(text)))
        return 268;
      s = text;
    }
    if (!s || !m->device)
      return 263;
    uint32_t n = 0, i = 0;
    while (s[i] && s[i] != ' ' && n + 1 < sizeof(cmd))
      cmd[n++] = s[i++];
    cmd[n++] = ' ';
    cmd[n++] = '"';
    for (uint32_t k = 0; m->file[k] && n + 4 < sizeof(cmd); k++)
      cmd[n++] = m->file[k];
    cmd[n++] = '"';
    for (; s[i] && n + 1 < sizeof(cmd); i++)
      cmd[n++] = s[i];
    cmd[n] = 0;
    m->error = mciSendStringA(cmd, m->returned, sizeof(m->returned),
                              m->window);
    return m->error;
  }
  case MCIWNDM_RETURNSTRINGA:
    text_copy((char *)lp, (uint32_t)wp, m->returned);
    return m->error;
  case MCIWNDM_RETURNSTRINGW:
    wide_copy((WCHAR *)lp, (uint32_t)wp, m->returned);
    return m->error;
  case MCIWNDM_GETERRORA:
  case MCIWNDM_GETERRORW:
    if (lp && wp) {
      text[0] = 0;
      if (m->error)
        mciGetErrorStringA(m->error, text, sizeof(text));
      if (msg == MCIWNDM_GETERRORW)
        wide_copy((WCHAR *)lp, (uint32_t)wp, text);
      else
        text_copy((char *)lp, (uint32_t)wp, text);
    }
    return m->error;
  case MCIWNDM_GETFILENAMEA:
    text_copy((char *)lp, (uint32_t)wp, m->file);
    return 0;
  case MCIWNDM_GETFILENAMEW:
    wide_copy((WCHAR *)lp, (uint32_t)wp, m->file);
    return 0;
  case MCIWNDM_GETDEVICEA:
    text_copy((char *)lp, (uint32_t)wp, m->device ? m->device_name : "");
    return 0;
  case MCIWNDM_GETDEVICEW:
    wide_copy((WCHAR *)lp, (uint32_t)wp, m->device ? m->device_name : "");
    return 0;
  case MCIWNDM_OPENA:
  case MCIWNDM_OPENW: {
    const char *f = (const char *)lp;
    if (msg == MCIWNDM_OPENW) {
      if (!narrow((const WCHAR *)lp, text, sizeof(text)))
        return 275;
      f = text;
    }
    if (!f || (wp & 0x0002u)) /* MCIWNDOPENF_NEW: recording unsupported */
      return 274;
    return open_file(m, f);
  }
  case MCIWNDM_NEWA:
  case MCIWNDM_NEWW:
    return 274; /* MCIERR_UNSUPPORTED_FUNCTION: no recording */
  case MCIWNDM_CHANGESTYLES:
    m->flags = (m->flags & ~(uint32_t)wp) | ((uint32_t)lp & (uint32_t)wp);
    fit(m);
    return 0;
  case MCIWNDM_GETSTYLES:
    return m->flags;
  case MCIWNDM_SETOWNER:
    m->owner = wp;
    return 0;
  case MCIWNDM_SETREPEAT:
    m->repeat = !!lp;
    return 0;
  case MCIWNDM_GETREPEAT:
    return m->repeat;
  case MCIWNDM_SETZOOM:
    m->zoom = lp > 0 ? (uint32_t)lp : 100;
    fit(m);
    return 0;
  case MCIWNDM_GETZOOM:
    return m->zoom;
  case MCIWNDM_SETSPEED:
    return lp == 1000 ? 0 : 274;
  case MCIWNDM_GETSPEED:
    return 1000;
  case MCIWNDM_SETVOLUME:
    return 274;
  case MCIWNDM_GETVOLUME:
    return 1000;
  case MCIWNDM_SETTIMERS:
    m->active = wp ? (uint32_t)wp : 500;
    m->inactive = lp ? (uint32_t)lp : 2000;
    SetTimer(m->window, 1, m->active, 0);
    return 0;
  case MCIWNDM_SETACTIVETIMER:
    m->active = wp ? (uint32_t)wp : 500;
    SetTimer(m->window, 1, m->active, 0);
    return 0;
  case MCIWNDM_SETINACTIVETIMER:
    m->inactive = wp ? (uint32_t)wp : 2000;
    return 0;
  case MCIWNDM_GETACTIVETIMER:
    return m->active;
  case MCIWNDM_GETINACTIVETIMER:
    return m->inactive;
  case MCIWNDM_GET_DEST:
  case MCIWNDM_GET_SOURCE: {
    struct {
      uintptr_t callback;
      struct rect r;
    } p = {0, {0, 0, 0, 0}};
    struct rect *out = (struct rect *)lp;
    uint32_t e = out ? command(m, MCI_WHERE,
                               msg == MCIWNDM_GET_DEST ? 0x40000u : 0x20000u,
                               &p)
                     : 297;
    if (!e)
      *out = (struct rect){p.r.left, p.r.top, p.r.left + p.r.right,
                           p.r.top + p.r.bottom};
    return e;
  }
  case MCIWNDM_PUT_DEST:
  case MCIWNDM_PUT_SOURCE: {
    const struct rect *in = (const struct rect *)lp;
    if (!in)
      return 297;
    struct {
      uintptr_t callback;
      struct rect r;
    } p = {0,
           {in->left, in->top, in->right - in->left, in->bottom - in->top}};
    return command(m, MCI_PUT,
                   (msg == MCIWNDM_PUT_DEST ? 0x40000u : 0x20000u) | 0x10000u,
                   &p);
  }
  case MCIWNDM_CAN_PLAY:
  case MCIWNDM_CAN_RECORD:
  case MCIWNDM_CAN_SAVE:
  case MCIWNDM_CAN_EJECT: {
    struct {
      uintptr_t callback;
      uint32_t value, item;
    } c = {0, 0,
           msg == MCIWNDM_CAN_PLAY     ? 8u
           : msg == MCIWNDM_CAN_RECORD ? 1u
           : msg == MCIWNDM_CAN_SAVE   ? 9u
                                       : 7u};
    return m->device &&
           !mciSendCommandA(m->device, MCI_GETDEVCAPS, 0x100u, (uintptr_t)&c) &&
           c.value;
  }
  case MCIWNDM_CAN_WINDOW:
    return m->device && m->devtype == 520;
  case MCIWNDM_CAN_CONFIG:
  case MCIWNDM_GETPALETTE:
  case MCIWNDM_OPENINTERFACE:
    return 0;
  case MCIWNDM_SETPALETTE:
  case MCIWNDM_PALETTEKICK:
    return 0;
  case MCIWNDM_REALIZE:
    return m->device ? command(m, MCI_REALIZE, 0, 0) : 0;
  case MCIWNDM_VALIDATEMEDIA:
    check_mode(m);
    return 0;
  case MCIWNDM_EJECT:
    return 274;
  }
  return DefWindowProcA(m->window, msg, wp, lp);
}
static intptr_t W32API proc(uintptr_t h, uint32_t msg, uintptr_t wp,
                            intptr_t lp) {
  struct mciwnd *m = find(h);
  if (msg == 0x0001) { /* WM_CREATE */
    const struct create *c = (const void *)lp;
    const struct {
      uint32_t flags;
      const char *file;
    } *params = c ? (const void *)c->params : 0;
    unsigned i = 0;
    while (i < MCIWNDS && windows[i])
      i++;
    if (i == MCIWNDS || !(m = heap_alloc(sizeof(*m))))
      return -1;
    m->window = h;
    m->owner = GetParent(h);
    m->flags = params ? params->flags : 0;
    m->zoom = 100;
    m->active = 500;
    m->inactive = 2000;
    m->mode = 524;
    windows[i] = m;
    if (m->flags & (MCIWNDF_NOTIFYMODE | MCIWNDF_NOTIFYPOS))
      SetTimer(h, 1, m->active, 0);
    if (params && params->file && params->file[0])
      open_file(m, params->file);
    return 0;
  }
  if (!m)
    return DefWindowProcA(h, msg, wp, lp);
  switch (msg) {
  case 0x0002: /* WM_DESTROY */
    KillTimer(h, 1);
    close_device(m);
    for (unsigned i = 0; i < MCIWNDS; i++)
      if (windows[i] == m)
        windows[i] = 0;
    heap_free(m);
    return 0;
  case 0x0010: /* WM_CLOSE (MCIWndDestroy) */
    DestroyWindow(h);
    return 0;
  case 0x0005: /* WM_SIZE */
    if (m->device && m->devtype == 520 &&
        !(m->flags & MCIWNDF_NOAUTOSIZEMOVIE)) {
      struct {
        uintptr_t callback;
        struct rect r;
      } p = {0, {0, 0, (int32_t)(lp & 0xffff), (int32_t)((lp >> 16) & 0xffff)}};
      if (p.r.right > 0 && p.r.bottom > 0)
        mciSendCommandA(m->device, MCI_PUT, 0x40000u | 0x10000u,
                        (uintptr_t)&p);
    }
    if (m->flags & MCIWNDF_NOTIFYSIZE)
      notify(m, MCIWNDM_NOTIFYSIZE, 0);
    return 0;
  case 0x000F: { /* WM_PAINT: the device repaints its current frame */
    struct paint ps;
    uintptr_t dc = BeginPaint(h, &ps);
    if (m->device && m->devtype == 520) {
      struct {
        uintptr_t callback;
        struct rect r;
        uintptr_t dc;
      } u = {0, {0, 0, 0, 0}, dc};
      mciSendCommandA(m->device, MCI_UPDATE, 0x20000u | 0x40000u,
                      (uintptr_t)&u);
    }
    EndPaint(h, &ps);
    return 0;
  }
  case 0x0113: /* WM_TIMER */
  case MM_MCINOTIFY:
    check_mode(m);
    return 0;
  }
  return message(m, msg, wp, lp);
}
