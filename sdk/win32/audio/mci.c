/* WinMM MCI core: device table, aliases, command dispatch, MM_MCINOTIFY
 * bookkeeping (successful/superseded/aborted/failure), MCI_WAIT with yield
 * procedures and break keys, auto-open, and the service worker.
 * Semantics: Windows 95/98 Platform SDK MCI reference (see
 * docs/validation/f5-12b-media.md). SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "audio.h"
#include "mci.h"
#include <w32/kernel32.h>
extern HANDLE W32API CreateThread(SECURITY_ATTRIBUTES *, DWORD,
                                  DWORD(W32API *)(void *), void *, DWORD,
                                  DWORD *);
extern BOOL W32API SetThreadPriority(HANDLE, int);
extern DWORD W32API GetCurrentThreadId(void);
extern HANDLE W32API GetModuleHandleA(const char *);
extern HANDLE W32API LoadLibraryA(const char *);
extern void *W32API GetProcAddress(HANDLE, const char *);
extern void W32API InitializeCriticalSection(RTL_CRITICAL_SECTION *);
extern void W32API EnterCriticalSection(RTL_CRITICAL_SECTION *);
extern void W32API LeaveCriticalSection(RTL_CRITICAL_SECTION *);
extern void wa_deliver(const struct wa_event *);

#define MCI_DEVICES 32u
static struct mci_device *devices[MCI_DEVICES];
/* Core-only ownership state leaves the MCI driver boundary unchanged. Keep
 * the file open for the device lifetime, including sequencers which cache
 * their input and close the driver's read handle immediately after parsing. */
struct owned_device {
  struct mci_device device;
  HANDLE file;
  BY_HANDLE_FILE_INFORMATION identity;
};
static void free_device(struct mci_device *d) {
  struct owned_device *owned = (struct owned_device *)d;
  if (owned->file && owned->file != K32_INVALID)
    CloseHandle(owned->file);
  wa_free(d);
}
static uint64_t next_generation;
/* Waiters own their result on the calling thread's stack. Completion
 * survives auto-close and an immediate reuse of the numerical device ID. */
struct wait_result {
  struct wait_result *next;
  uint32_t id, serial, error;
  uint64_t generation;
};
static struct wait_result *waiters;
static void finish_waiters(struct mci_device *d) {
  for (struct wait_result *w = waiters; w; w = w->next)
    if (w->id == d->id && w->generation == d->generation && w->serial == d->busy_serial)
      w->error = d->completion_error;
}
static struct mci_cleanup *pending_cleanup;
static void host_defer(struct mci_cleanup *c) {
  c->next = pending_cleanup;
  pending_cleanup = c;
}
static RTL_CRITICAL_SECTION mci_guard;
static volatile uint32_t mci_initialized;
static HANDLE worker;
static volatile uint32_t worker_quit;

void mci_lock(void);
void mci_unlock(void);
static void init(void) {
  uint32_t expected = 0;
  if (__atomic_compare_exchange_n(&mci_initialized, &expected, 1, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    InitializeCriticalSection(&mci_guard);
    __atomic_store_n(&mci_initialized, 2, __ATOMIC_RELEASE);
  } else
    while (__atomic_load_n(&mci_initialized, __ATOMIC_ACQUIRE) != 2)
      Sleep(0);
}
void mci_lock(void) {
  init();
  EnterCriticalSection(&mci_guard);
}
void mci_unlock(void) { LeaveCriticalSection(&mci_guard); }

int mci_strieq(const char *a, const char *b) {
  for (;; a++, b++) {
    unsigned x = (uint8_t)*a, y = (uint8_t)*b;
    if (x >= 'A' && x <= 'Z')
      x += 32;
    if (y >= 'A' && y <= 'Z')
      y += 32;
    if (x != y)
      return 0;
    if (!x)
      return 1;
  }
}
static void copy_text(char *out, uint32_t cap, const char *s) {
  uint32_t i = 0;
  if (!cap)
    return;
  for (; s && s[i] && i + 1 < cap; i++)
    out[i] = s[i];
  out[i] = 0;
}

/* Notification delivery: PostMessage(hwnd, MM_MCINOTIFY, flag, device id). */
#ifdef W32_HOST_MODEL
void mci_host_post(uintptr_t window, uint32_t status, uint32_t id);
#endif
static void post(uintptr_t window, uint32_t status, uint32_t id) {
  if (!window)
    return;
#ifdef W32_HOST_MODEL
  mci_host_post(window, status, id);
#else
  struct wa_event e = {.callback = {window, 0, WA_CALLBACK_WINDOW},
                       .handle = status,
                       .message = MM_MCINOTIFY,
                       .p1 = id};
  wa_deliver(&e);
#endif
}
static void host_begin(struct mci_device *d) {
  d->completion_error = 0;
  d->busy = 1;
  d->busy_serial++;
}
static void close_locked(struct mci_device *d);
static void host_complete(struct mci_device *d, uint32_t status) {
  finish_waiters(d);
  d->busy = 0;
  d->busy_serial++;
  if (d->notify_pending) {
    d->notify_pending = 0;
    post(d->notify, status, d->id);
  }
}
static void host_abort(struct mci_device *d) {
  d->busy = 0;
  d->busy_serial++;
  if (d->notify_pending) {
    d->notify_pending = 0;
    post(d->notify, MCI_NOTIFY_ABORTED, d->id);
  }
}
/* Auto-opened devices close when their command completes, is aborted or
 * fails (Platform SDK "Command Shortcuts and Variations"). A cancelled
 * auto-open device closes on the next service pass, never inside the
 * driver call that cancelled it. */
static void host_wake(void) {}
static const struct mci_host host = {MCI_BOUNDARY_VERSION,
                                     host_begin,
                                     host_complete,
                                     host_abort,
                                     wa_alloc,
                                     wa_free,
                                     mci_lock,
                                     mci_unlock,
                                     host_wake, host_defer};

/* Installed device types (system.ini [mci32] of Windows 98). */
struct mci_type {
  const char *name, *library;
  uint32_t devtype;
  const struct mci_driver *(*internal)(void);
};
static const struct mci_type types[] = {
    {"waveaudio", 0, MCI_DEVTYPE_WAVEFORM_AUDIO, mci_wave_driver},
    {"sequencer", 0, MCI_DEVTYPE_SEQUENCER, mci_seq_driver},
    {"avivideo", "mciavi32.dll", MCI_DEVTYPE_DIGITAL_VIDEO, 0},
    {"cdaudio", 0, MCI_DEVTYPE_CD_AUDIO, mci_cd_driver},
};
#define MCI_TYPES (sizeof(types) / sizeof(types[0]))
static const struct {
  const char *ext, *type;
} extensions[] = {{"wav", "waveaudio"},
                  {"mid", "sequencer"},
                  {"rmi", "sequencer"},
                  {"midi", "sequencer"},
                  {"avi", "avivideo"}};
uint32_t mci_installed_count(void) { return MCI_TYPES; }
const char *mci_installed_name(uint32_t index) {
  return index && index <= MCI_TYPES ? types[index - 1].name : 0;
}
const char *mci_type_for_element(const char *element) {
  const char *dot = 0;
  for (const char *p = element; p && *p; p++)
    if (*p == '.')
      dot = p + 1;
    else if (*p == '\\' || *p == '/' || *p == ':')
      dot = 0;
  if (!dot)
    return 0;
  for (unsigned i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
    if (mci_strieq(dot, extensions[i].ext))
      return extensions[i].type;
  return 0;
}
int mci_is_type(const char *name) {
  for (unsigned i = 0; i < MCI_TYPES; i++)
    if (mci_strieq(name, types[i].name))
      return 1;
  return 0;
}
static const struct mci_driver *avi_driver;
static uint32_t load_driver(const struct mci_type *t,
                            const struct mci_driver **out) {
  *out = 0;
  if (t->internal) {
    *out = t->internal();
    return 0;
  }
  if (!t->library)
    return MCIERR_DEVICE_NOT_INSTALLED;
  if (!avi_driver) {
#ifdef W32_HOST_MODEL
    extern const struct mci_driver *mci_host_avi(const struct mci_host *);
    avi_driver = mci_host_avi(&host);
#else
    HANDLE module = LoadLibraryA(t->library);
    mci_driver_entry entry =
        module ? (mci_driver_entry)GetProcAddress(module, "CiukiMciDriver")
               : 0;
    avi_driver = entry ? entry(&host) : 0;
#endif
    if (!avi_driver || avi_driver->version != MCI_BOUNDARY_VERSION) {
      avi_driver = 0;
      return MCIERR_CANNOT_LOAD_DRIVER;
    }
  }
  *out = avi_driver;
  return 0;
}

static struct mci_device *find_id(uint32_t id) {
  if (!id || id > MCI_DEVICES)
    return 0;
  return devices[id - 1];
}
struct mci_device *mci_find_name(const char *name) {
  for (unsigned i = 0; i < MCI_DEVICES; i++)
    if (devices[i] && mci_strieq(devices[i]->alias, name))
      return devices[i];
  return 0;
}
uint32_t mci_device_type(uint32_t id) {
  mci_lock();
  struct mci_device *d = find_id(id);
  uint32_t type = d ? d->devtype : 0;
  mci_unlock();
  return type;
}
uint32_t mci_lookup(const char *name, uint32_t *devtype) {
  mci_lock();
  struct mci_device *d = mci_find_name(name);
  uint32_t id = d ? d->id : 0;
  *devtype = d ? d->devtype : 0;
  mci_unlock();
  return id;
}

static DWORD W32API worker_main(void *p);
static void ensure_worker(void) {
  if (worker)
    return;
#ifndef W32_HOST_MODEL
  /* The worker's code must outlive FreeLibrary("winmm.dll"). */
  if (!LoadLibraryA("winmm.dll"))
    return;
#endif
  worker = CreateThread(0, 65536, worker_main, 0, 0, 0);
  if (worker)
    SetThreadPriority(worker, 1);
}
/* One service pass; the worker calls it every 5 ms, host tests directly. */
void mci_service_pass(void) {
  mci_lock();
  uint32_t now = GetTickCount();
  for (unsigned i = 0; i < MCI_DEVICES; i++) {
    struct mci_device *d = devices[i];
    if (d && d->driver->service)
      d->driver->service(d, now);
    d = devices[i];
    if (d && d->auto_open == 2 && !d->busy)
      close_locked(d);
  }
  struct mci_cleanup **link = &pending_cleanup;
  while (*link) {
    struct mci_cleanup *c = *link, *next = c->next;
    if (c->run(c->context))
      *link = next; /* run may free its embedded node */
    else
      link = &c->next;
  }
  mci_unlock();
}
static DWORD W32API worker_main(void *p) {
  (void)p;
  while (!worker_quit) {
    mci_service_pass();
    Sleep(5);
  }
  return 0;
}

static void close_locked(struct mci_device *d) {
  if (d->busy)
    host_abort(d);
  if (d->notify_pending) {
    d->notify_pending = 0;
    post(d->notify, MCI_NOTIFY_ABORTED, d->id);
  }
  d->driver->close(d);
  devices[d->id - 1] = 0;
  free_device(d);
}

/* MCI_OPEN: element/type/alias resolution, then the driver's open. */
static uint32_t open_locked(uint32_t flags, struct mci_open *p, int automatic,
                            struct mci_device **result) {
  char type[32] = {0}, element[260] = {0};
  const char *name = 0;
  if (!p)
    return MCIERR_NULL_PARAMETER_BLOCK;
  if (flags & MCI_OPEN_ELEMENT_ID)
    return MCIERR_UNSUPPORTED_FUNCTION;
  if (flags & MCI_OPEN_ELEMENT) {
    if (!p->element)
      return MCIERR_MISSING_PARAMETER;
    copy_text(element, sizeof(element), (const char *)p->element);
    if (w32_strlen((const char *)p->element) >= sizeof(element))
      return MCIERR_INVALID_FILE;
  }
  if (flags & MCI_OPEN_TYPE) {
    if (flags & MCI_OPEN_TYPE_ID) {
      uint32_t id = (uint32_t)p->type & 0xffff;
      for (unsigned i = 0; i < MCI_TYPES; i++)
        if (types[i].devtype == id)
          copy_text(type, sizeof(type), types[i].name);
      if (!type[0])
        return MCIERR_INVALID_DEVICE_NAME;
    } else {
      if (!p->type)
        return MCIERR_MISSING_PARAMETER;
      if (w32_strlen((const char *)p->type) >= sizeof(type) + 260)
        return MCIERR_INVALID_DEVICE_NAME;
      const char *s = (const char *)p->type;
      /* "type!element" in the type field (Platform SDK MCI_OPEN). */
      unsigned i = 0;
      while (s[i] && s[i] != '!' && i + 1 < sizeof(type)) {
        type[i] = s[i];
        i++;
      }
      type[i] = 0;
      if (s[i] == '!') {
        if (flags & MCI_OPEN_ELEMENT)
          return MCIERR_FLAGS_NOT_COMPATIBLE;
        copy_text(element, sizeof(element), s + i + 1);
      } else if (s[i])
        return MCIERR_INVALID_DEVICE_NAME;
    }
  } else if (element[0]) {
    unsigned i = 0;
    while (element[i] && element[i] != '!')
      i++;
    if (element[i] == '!' && i < sizeof(type)) {
      w32_copy(type, element, i);
      type[i] = 0;
      copy_text(element, sizeof(element), element + i + 1);
    } else if (mci_is_type(element)) {
      copy_text(type, sizeof(type), element);
      element[0] = 0;
    } else {
      const char *t = mci_type_for_element(element);
      if (!t)
        return MCIERR_EXTENSION_NOT_FOUND;
      copy_text(type, sizeof(type), t);
    }
  } else
    return MCIERR_MISSING_PARAMETER;
  const struct mci_type *t = 0;
  for (unsigned i = 0; i < MCI_TYPES; i++)
    if (mci_strieq(type, types[i].name))
      t = &types[i];
  if (!t)
    return MCIERR_INVALID_DEVICE_NAME;
  if (flags & MCI_OPEN_ALIAS) {
    if (!p->alias)
      return MCIERR_MISSING_PARAMETER;
    name = (const char *)p->alias;
    if (!name[0] || w32_strlen(name) >= 260)
      return MCIERR_INVALID_DEVICE_NAME;
    if (mci_strieq(name, "all"))
      return MCIERR_CANNOT_USE_ALL;
    if (mci_find_name(name))
      return MCIERR_DUPLICATE_ALIAS;
  } else {
    name = element[0] ? element : t->name;
    if (w32_strlen(name) >= 260)
      return MCIERR_DEVICE_LENGTH;
    struct mci_device *other = mci_find_name(name);
    if (other)
      return other->shareable && (flags & MCI_OPEN_SHAREABLE)
                 ? MCIERR_DUPLICATE_ALIAS
                 : MCIERR_DEVICE_OPEN;
  }
  const struct mci_driver *driver;
  uint32_t error = load_driver(t, &driver);
  if (error)
    return error;
  unsigned slot = 0;
  while (slot < MCI_DEVICES && devices[slot])
    slot++;
  if (slot == MCI_DEVICES)
    return MCIERR_OUT_OF_MEMORY;
  struct owned_device *owned = wa_alloc(sizeof(*owned));
  if (!owned)
    return MCIERR_OUT_OF_MEMORY;
  w32_fill(owned, 0, sizeof(*owned));
  struct mci_device *d = &owned->device;
  if (element[0] && t->devtype != MCI_DEVTYPE_CD_AUDIO) {
    owned->file = CreateFileA(element, 0x80000000u, 1, 0, 3, 0, 0);
    if (owned->file == K32_INVALID) {
      free_device(d);
      return MCIERR_FILE_NOT_FOUND;
    }
    if (!GetFileInformationByHandle(owned->file, &owned->identity)) {
      free_device(d);
      return MCIERR_FILE_READ;
    }
    for (unsigned i = 0; i < MCI_DEVICES; i++) {
      struct mci_device *other = devices[i];
      if (!other || other->devtype != t->devtype) continue;
      struct owned_device *previous = (struct owned_device *)other;
      if (!previous->file ||
          owned->identity.serial != previous->identity.serial ||
          owned->identity.index_high != previous->identity.index_high ||
          owned->identity.index_low != previous->identity.index_low) continue;
      if (t->devtype == MCI_DEVTYPE_WAVEFORM_AUDIO ||
          t->devtype == MCI_DEVTYPE_SEQUENCER || !other->shareable ||
          !(flags & MCI_OPEN_SHAREABLE)) {
        free_device(d);
        return MCIERR_DEVICE_OPEN;
      }
    }
  }
  d->id = slot + 1;
  d->generation = ++next_generation;
  d->devtype = t->devtype;
  d->creator = GetCurrentThreadId();
  d->shareable = !!(flags & MCI_OPEN_SHAREABLE);
  d->auto_open = automatic ? 1 : 0;
  d->driver = driver;
  d->host = &host;
  d->break_key = 3; /* VK_CANCEL (Ctrl+Break), MCI_BREAK default */
  copy_text(d->alias, sizeof(d->alias), name);
  copy_text(d->type, sizeof(d->type), t->name);
  copy_text(d->element, sizeof(d->element), element);
  devices[slot] = d;
  error = driver->open(d, flags, p) & 0xffff;
  if (error) {
    devices[slot] = 0;
    free_device(d);
    return error;
  }
  p->device = d->id;
  *result = d;
  ensure_worker();
  return 0;
}

static uint32_t sysinfo_locked(uint32_t id, uint32_t flags,
                               struct mci_sysinfo *p, const char *type,
                               uint32_t *kind) {
  if (!p)
    return MCIERR_NULL_PARAMETER_BLOCK;
  const char *want = 0;
  if (id != MCI_ALL_DEVICE_ID) {
    for (unsigned i = 0; i < MCI_TYPES; i++)
      if ((type && mci_strieq(type, types[i].name)) ||
          (!type && types[i].devtype == p->type))
        want = types[i].name;
    if (!want)
      return MCIERR_INVALID_DEVICE_NAME;
  }
  if (flags & MCI_SYSINFO_QUANTITY) {
    uint32_t n = 0;
    if (flags & MCI_SYSINFO_OPEN) {
      for (unsigned i = 0; i < MCI_DEVICES; i++)
        if (devices[i] && (!want || mci_strieq(devices[i]->type, want)))
          n++;
    } else
      for (unsigned i = 0; i < MCI_TYPES; i++)
        if ((types[i].internal || types[i].library) &&
            (!want || mci_strieq(want, types[i].name)))
          n++;
    if (!p->text || p->size < 4)
      return MCIERR_PARAM_OVERFLOW;
    w32_put32((void *)p->text, n);
    *kind = MCI_INTEGER_RETURNED;
    return 0;
  }
  const char *result = 0;
  if (flags & MCI_SYSINFO_NAME) {
    uint32_t n = 0;
    if (flags & MCI_SYSINFO_OPEN) {
      for (unsigned i = 0; i < MCI_DEVICES && !result; i++)
        if (devices[i] && (!want || mci_strieq(devices[i]->type, want)) &&
            ++n == p->number)
          result = devices[i]->alias;
    } else
      for (unsigned i = 0; i < MCI_TYPES && !result; i++)
        if ((types[i].internal || types[i].library) &&
            (!want || mci_strieq(want, types[i].name)) && ++n == p->number)
          result = types[i].name;
    if (!result)
      return MCIERR_OUTOFRANGE;
  } else if (flags & MCI_SYSINFO_INSTALLNAME) {
    if (!want)
      return MCIERR_CANNOT_USE_ALL;
    result = want;
  } else
    return MCIERR_MISSING_PARAMETER;
  if (!p->text)
    return MCIERR_PARAM_OVERFLOW;
  if (w32_strlen(result) >= p->size)
    return MCIERR_PARAM_OVERFLOW;
  copy_text((char *)p->text, p->size, result);
  return 0;
}

/* Default yield: Ctrl+Break (or the MCI_BREAK key) aborts a wait. */
static uint32_t default_yield(int32_t break_key) {
  if (break_key < 0)
    return 0;
#ifdef W32_HOST_MODEL
  extern int mci_host_key(int);
  return mci_host_key(break_key) ? 1 : 0;
#else
  HANDLE user = GetModuleHandleA("user32.dll");
  typedef int16_t(W32API * key_fn)(int);
  key_fn key = user ? (key_fn)GetProcAddress(user, "GetAsyncKeyState") : 0;
  return key && (key(break_key) & 0x8000) ? 1 : 0;
#endif
}

/* Executes one command. Returns the error in the low word; *kind receives
 * the driver's return flags for the string interface. */
uint32_t mci_execute(uint32_t id, uint32_t msg, uint32_t flags, void *parms,
                     uint32_t *kind, const char *sysinfo_type) {
  uint32_t dummy;
  if (!kind)
    kind = &dummy;
  *kind = 0;
  mci_lock();
  uint32_t error = 0;
  if (msg == MCI_OPEN) {
    struct mci_device *d = 0;
    error = open_locked(flags, parms, 0, &d);
    if (!error && (flags & MCI_NOTIFY))
      post(((struct mci_open *)parms)->callback, MCI_NOTIFY_SUCCESSFUL,
           d->id);
    mci_unlock();
    return error;
  }
  if (msg == MCI_SYSINFO) {
    error = sysinfo_locked(id, flags, parms, sysinfo_type, kind);
    mci_unlock();
    return error;
  }
  if (id == MCI_ALL_DEVICE_ID) {
    if (msg != MCI_CLOSE && msg != MCI_STOP && msg != MCI_PAUSE &&
        msg != MCI_RESUME) {
      mci_unlock();
      return MCIERR_CANNOT_USE_ALL;
    }
    if ((flags & MCI_NOTIFY) && !parms) {
      mci_unlock();
      return MCIERR_NULL_PARAMETER_BLOCK;
    }
    for (unsigned i = 0; i < MCI_DEVICES; i++) {
      struct mci_device *d = devices[i];
      if (!d)
        continue;
      if (msg == MCI_CLOSE)
        close_locked(d);
      else {
        uint32_t e = d->driver->command(d, msg, flags & ~MCI_NOTIFY, parms);
        if ((e & 0xffff) && !error)
          error = e & 0xffff;
      }
    }
    if (!error && (flags & MCI_NOTIFY))
      post(((struct mci_generic *)parms)->callback, MCI_NOTIFY_SUCCESSFUL,
           MCI_ALL_DEVICE_ID);
    mci_unlock();
    return error;
  }
  struct mci_device *d = find_id(id);
  if (!d) {
    mci_unlock();
    return MCIERR_INVALID_DEVICE_ID;
  }
  if ((flags & MCI_NOTIFY) && !parms) {
    mci_unlock();
    return MCIERR_NULL_PARAMETER_BLOCK;
  }
  if (msg == MCI_CLOSE) {
    uintptr_t window = (flags & MCI_NOTIFY)
                           ? ((struct mci_generic *)parms)->callback
                           : 0;
    close_locked(d);
    post(window, MCI_NOTIFY_SUCCESSFUL, id);
    mci_unlock();
    return 0;
  }
  if (msg == MCI_BREAK) {
    struct mci_break *b = parms;
    if (!b)
      error = MCIERR_NULL_PARAMETER_BLOCK;
    else if ((flags & MCI_BREAK_KEY) && (flags & MCI_BREAK_OFF))
      error = MCIERR_FLAGS_NOT_COMPATIBLE;
    else if (flags & MCI_BREAK_KEY) {
      d->break_key = b->key;
      d->break_window = (flags & MCI_BREAK_HWND) ? b->window : 0;
    } else if (flags & MCI_BREAK_OFF)
      d->break_key = -1;
    else
      error = MCIERR_MISSING_PARAMETER;
    if (!error && (flags & MCI_NOTIFY))
      post(b->callback, MCI_NOTIFY_SUCCESSFUL, id);
    mci_unlock();
    return error;
  }
  uint32_t r = d->driver->command(d, msg, flags, parms);
  error = r & 0xffff;
  *kind = r & 0x00ff0000u;
  if (!error) {
    /* Drivers post MCI_NOTIFY_ABORTED (host->abort) when a command cancels
     * the pending operation: stop, seek, close, or a play that changes the
     * end position or direction. Pause/resume keep it (MM_MCINOTIFY). */
    if ((msg == MCI_STOP || msg == MCI_SEEK) && d->notify_pending &&
        !d->busy) {
      d->notify_pending = 0;
      post(d->notify, MCI_NOTIFY_ABORTED, id);
    }
    if (flags & MCI_NOTIFY) {
      uintptr_t window = ((struct mci_generic *)parms)->callback;
      if (d->notify_pending) {
        d->notify_pending = 0;
        post(d->notify, MCI_NOTIFY_SUPERSEDED, id);
      }
      if ((r & MCI_ASYNC) && d->busy) {
        d->notify = window;
        d->notify_pending = 1;
      } else
        post(window, MCI_NOTIFY_SUCCESSFUL, id);
    }
    if ((flags & MCI_WAIT) && (r & MCI_ASYNC) && d->busy) {
      /* A break (yield procedure or break key) returns control without
       * interrupting the command (Platform SDK "The Wait Flag"). */
      uint32_t waiting = d->busy_serial;
      uint64_t generation = d->generation;
      struct wait_result result = {waiters, id, waiting, 0, generation};
      waiters = &result;
      while ((d = find_id(id)) && d->generation == generation &&
             d->busy && d->busy_serial == waiting) {
        int32_t break_key = d->break_key;
        uintptr_t yield = d->yield;
        uint32_t data = d->yield_data, broken;
        mci_unlock();
        if (yield) {
          typedef uint32_t(W32API * yield_fn)(uint32_t, uint32_t);
          broken = ((yield_fn)yield)(id, data) != 0;
        } else
          broken = default_yield(break_key);
        if (!broken)
          Sleep(5);
        mci_lock();
        if (broken)
          break;
      }
      error = result.error;
      struct wait_result **link = &waiters;
      while (*link != &result) link = &(*link)->next;
      *link = result.next;
    }
  }
  mci_unlock();
  return error;
}

/* Opens a device for a string command that names an unopened element or a
 * simple device type (Platform SDK "auto-open"). */
uint32_t mci_auto_open(const char *name, uint32_t *id) {
  struct mci_open p = {0};
  uint32_t flags = MCI_OPEN_ELEMENT;
  p.element = (uintptr_t)name;
  mci_lock();
  struct mci_device *d = 0;
  uint32_t e = open_locked(flags, &p, 1, &d);
  if (!e)
    *id = d->id;
  mci_unlock();
  return e;
}
/* After a command on an auto-opened device: close now, or at completion. */
void mci_auto_close(uint32_t id) {
  mci_lock();
  struct mci_device *d = find_id(id);
  if (d && d->auto_open) {
    if (d->busy)
      d->auto_open = 2;
    else
      close_locked(d);
  }
  mci_unlock();
}

uint32_t W32API mciSendCommandA(uint32_t id, uint32_t msg, uintptr_t flags,
                                uintptr_t parms) {
  uint32_t kind = 0;
  if (msg < MCI_OPEN_DRIVER || msg > 0x0FFF)
    return MCIERR_UNRECOGNIZED_COMMAND;
  uint32_t r =
      mci_execute(id, msg, (uint32_t)flags, (void *)parms, &kind, 0);
  /* The command interface receives plain values: a resource return keeps
   * only its low word (for example MCI_MODE_PLAY or TRUE/FALSE). */
  if (!r && parms && (kind & MCI_RESOURCE_RETURNED)) {
    if (msg == MCI_STATUS) {
      struct mci_status *s = (void *)parms;
      s->value &= 0xffff;
    } else if (msg == MCI_GETDEVCAPS) {
      struct mci_caps *c = (void *)parms;
      c->value &= 0xffff;
    }
  }
  return r;
}
static uint32_t wide_to_ansi(const WCHAR *w, char *out, uint32_t cap) {
  uint32_t i = 0;
  if (!w) {
    out[0] = 0;
    return 0;
  }
  for (; w[i]; i++) {
    if (i + 1 >= cap || w[i] > 0xff)
      return 1;
    out[i] = (char)w[i];
  }
  out[i] = 0;
  return 0;
}
static void ansi_to_wide(const char *s, WCHAR *out, uint32_t cap) {
  uint32_t i = 0;
  if (!cap)
    return;
  for (; s[i] && i + 1 < cap; i++)
    out[i] = (uint8_t)s[i];
  out[i] = 0;
}
uint32_t W32API mciSendCommandW(uint32_t id, uint32_t msg, uintptr_t flags,
                                uintptr_t parms) {
  if (!parms || (msg != MCI_OPEN && msg != MCI_INFO && msg != MCI_SYSINFO &&
                 msg != MCI_WINDOW))
    return mciSendCommandA(id, msg, flags, parms);
  if (msg == MCI_OPEN) {
    struct mci_open copy = *(struct mci_open *)parms;
    static char strings[3][260];
    mci_lock();
    if (((flags & MCI_OPEN_TYPE) && !(flags & MCI_OPEN_TYPE_ID) &&
         wide_to_ansi((const WCHAR *)copy.type, strings[0], 260)) ||
        ((flags & MCI_OPEN_ELEMENT) &&
         wide_to_ansi((const WCHAR *)copy.element, strings[1], 260)) ||
        ((flags & MCI_OPEN_ALIAS) &&
         wide_to_ansi((const WCHAR *)copy.alias, strings[2], 260))) {
      mci_unlock();
      return MCIERR_INVALID_DEVICE_NAME;
    }
    if ((flags & MCI_OPEN_TYPE) && !(flags & MCI_OPEN_TYPE_ID))
      copy.type = (uintptr_t)strings[0];
    if (flags & MCI_OPEN_ELEMENT)
      copy.element = (uintptr_t)strings[1];
    if (flags & MCI_OPEN_ALIAS)
      copy.alias = (uintptr_t)strings[2];
    uint32_t r = mciSendCommandA(id, msg, flags, (uintptr_t)&copy);
    mci_unlock();
    ((struct mci_open *)parms)->device = copy.device;
    return r;
  }
  if (msg == MCI_WINDOW) {
    struct mci_window copy = *(struct mci_window *)parms;
    char text[260];
    if ((flags & MCI_DGV_WINDOW_TEXT) &&
        wide_to_ansi((const WCHAR *)copy.text, text, sizeof(text)))
      return MCIERR_PARAM_OVERFLOW;
    copy.text = (uintptr_t)text;
    return mciSendCommandA(id, msg, flags, (uintptr_t)&copy);
  }
  /* MCI_INFO / MCI_SYSINFO return text: use an ANSI scratch buffer. The
   * caller's block is only as large as its own structure. */
  struct mci_sysinfo *w = (void *)parms, copy = {0};
  char text[512];
  w32_copy(&copy, w,
           msg == MCI_SYSINFO ? sizeof(copy) : sizeof(struct mci_info));
  if (!w->text)
    return mciSendCommandA(id, msg, flags, parms);
  copy.text = (uintptr_t)text;
  copy.size = w->size < sizeof(text) ? w->size : sizeof(text);
  uint32_t r = mciSendCommandA(id, msg, flags, (uintptr_t)&copy);
  if (!r) {
    if (msg == MCI_SYSINFO && (flags & MCI_SYSINFO_QUANTITY))
      w32_copy((void *)w->text, text, 4);
    else
      ansi_to_wide(text, (WCHAR *)w->text, w->size);
  }
  return r;
}
uint32_t W32API mciGetDeviceIDA(const char *name) {
  if (!name)
    return 0;
  if (mci_strieq(name, "all"))
    return MCI_ALL_DEVICE_ID;
  mci_lock();
  struct mci_device *d = mci_find_name(name);
  uint32_t id = d ? d->id : 0;
  mci_unlock();
  return id;
}
uint32_t W32API mciGetDeviceIDW(const WCHAR *name) {
  char text[260];
  if (!name || wide_to_ansi(name, text, sizeof(text)))
    return 0;
  return mciGetDeviceIDA(text);
}
uint32_t W32API mciGetDeviceIDFromElementIDA(uint32_t element,
                                             const char *type) {
  (void)element;
  (void)type;
  return 0; /* MCI_OPEN_ELEMENT_ID is not supported */
}
uint32_t W32API mciGetDeviceIDFromElementIDW(uint32_t element,
                                             const WCHAR *type) {
  (void)element;
  (void)type;
  return 0;
}
uintptr_t W32API mciGetCreatorTask(uint32_t id) {
  mci_lock();
  struct mci_device *d = find_id(id);
  uintptr_t task = d ? d->creator : 0;
  mci_unlock();
  return task;
}
BOOL W32API mciSetYieldProc(uint32_t id, uintptr_t proc, uint32_t data) {
  mci_lock();
  struct mci_device *d = find_id(id);
  if (d) {
    d->yield = proc;
    d->yield_data = data;
  }
  mci_unlock();
  return d != 0;
}
uintptr_t W32API mciGetYieldProc(uint32_t id, uint32_t *data) {
  mci_lock();
  struct mci_device *d = find_id(id);
  uintptr_t proc = d ? d->yield : 0;
  if (d && data)
    *data = d->yield_data;
  mci_unlock();
  return proc;
}

/* Error texts: original CiukiOS wording, codes from mmsystem.h. */
static const struct {
  uint16_t code;
  const char *text;
} errors[] = {
    {0, "The specified command was carried out."},
    {MCIERR_INVALID_DEVICE_ID, "Invalid MCI device ID."},
    {MCIERR_UNRECOGNIZED_KEYWORD, "The driver cannot recognize a parameter."},
    {MCIERR_UNRECOGNIZED_COMMAND, "The driver cannot recognize the command."},
    {MCIERR_HARDWARE, "The media device reported a hardware problem."},
    {MCIERR_INVALID_DEVICE_NAME,
     "The specified device is not open or is not recognized by MCI."},
    {MCIERR_OUT_OF_MEMORY, "Not enough memory for the media operation."},
    {MCIERR_DEVICE_OPEN, "The device name is already used by this program."},
    {MCIERR_CANNOT_LOAD_DRIVER, "The device driver could not be loaded."},
    {MCIERR_MISSING_COMMAND_STRING, "No command was specified."},
    {MCIERR_PARAM_OVERFLOW, "The output string was too large for the buffer."},
    {MCIERR_MISSING_STRING_ARGUMENT, "The command requires a string parameter."},
    {MCIERR_BAD_INTEGER, "The specified integer is invalid."},
    {MCIERR_PARSER_INTERNAL, "The MCI command parser failed internally."},
    {MCIERR_DRIVER_INTERNAL, "The device driver failed internally."},
    {MCIERR_MISSING_PARAMETER, "The command requires a parameter."},
    {MCIERR_UNSUPPORTED_FUNCTION, "The device cannot carry out the command."},
    {MCIERR_FILE_NOT_FOUND, "The specified file cannot be found."},
    {MCIERR_DEVICE_NOT_READY, "The device is not ready."},
    {MCIERR_INTERNAL, "MCI failed internally."},
    {MCIERR_DRIVER, "The device driver reported an error."},
    {MCIERR_CANNOT_USE_ALL, "The device name \"all\" is not allowed here."},
    {MCIERR_MULTIPLE, "Errors occurred in more than one device."},
    {MCIERR_EXTENSION_NOT_FOUND,
     "The file extension does not identify a device type."},
    {MCIERR_OUTOFRANGE, "The parameter is out of range."},
    {MCIERR_FLAGS_NOT_COMPATIBLE, "The parameters cannot be used together."},
    {MCIERR_FILE_NOT_SAVED, "The file was not saved."},
    {MCIERR_DEVICE_TYPE_REQUIRED, "A device type is required."},
    {MCIERR_DEVICE_LOCKED, "The device is locked."},
    {MCIERR_DUPLICATE_ALIAS, "The alias is already used by this program."},
    {MCIERR_BAD_CONSTANT, "The parameter constant is invalid."},
    {MCIERR_MUST_USE_SHAREABLE, "The device must be opened shareable."},
    {MCIERR_MISSING_DEVICE_NAME, "The command requires a device name."},
    {MCIERR_BAD_TIME_FORMAT, "The time value is invalid."},
    {MCIERR_NO_CLOSING_QUOTE, "A closing quotation mark is missing."},
    {MCIERR_DUPLICATE_FLAGS, "A parameter or value was given twice."},
    {MCIERR_INVALID_FILE, "The file cannot be played by this device."},
    {MCIERR_NULL_PARAMETER_BLOCK, "A null parameter block was passed."},
    {MCIERR_UNNAMED_RESOURCE, "An unnamed file cannot be saved."},
    {MCIERR_NEW_REQUIRES_ALIAS, "An alias is required with \"new\"."},
    {MCIERR_NOTIFY_ON_AUTO_OPEN,
     "\"notify\" cannot be used with an automatically opened device."},
    {MCIERR_NO_ELEMENT_ALLOWED, "A file name is not allowed for this device."},
    {MCIERR_NONAPPLICABLE_FUNCTION,
     "The commands cannot be carried out in this order."},
    {MCIERR_ILLEGAL_FOR_AUTO_OPEN,
     "The command is not allowed for an automatically opened device."},
    {MCIERR_FILENAME_REQUIRED, "A valid file name is required."},
    {MCIERR_EXTRA_CHARACTERS, "Unexpected text follows a quoted string."},
    {MCIERR_DEVICE_NOT_INSTALLED, "The device is not installed."},
    {MCIERR_GET_CD, "The specified file or MCI device cannot be accessed."},
    {MCIERR_SET_CD, "The file or device cannot be accessed."},
    {MCIERR_SET_DRIVE, "The drive cannot be accessed."},
    {MCIERR_DEVICE_LENGTH, "The device or driver name is too long."},
    {MCIERR_DEVICE_ORD_LENGTH, "The device or driver name is too long."},
    {MCIERR_NO_INTEGER, "The command requires an integer parameter."},
    {MCIERR_WAVE_OUTPUTSINUSE, "All wave output devices are in use."},
    {MCIERR_WAVE_SETOUTPUTINUSE, "The wave output device is in use."},
    {MCIERR_WAVE_INPUTSINUSE, "All wave input devices are in use."},
    {MCIERR_WAVE_SETINPUTINUSE, "The wave input device is in use."},
    {MCIERR_WAVE_OUTPUTUNSPECIFIED, "No wave output device is available."},
    {MCIERR_WAVE_INPUTUNSPECIFIED, "No wave input device is available."},
    {MCIERR_WAVE_OUTPUTSUNSUITABLE,
     "No wave output device can play this format."},
    {MCIERR_WAVE_SETOUTPUTUNSUITABLE,
     "The wave output device cannot play this format."},
    {MCIERR_WAVE_INPUTSUNSUITABLE, "No wave input device can record this format."},
    {MCIERR_WAVE_SETINPUTUNSUITABLE,
     "The wave input device cannot record this format."},
    {MCIERR_SEQ_DIV_INCOMPATIBLE,
     "The time formats of the song pointer and SMPTE are incompatible."},
    {MCIERR_SEQ_PORT_INUSE, "The MIDI output port is in use."},
    {MCIERR_SEQ_PORT_NONEXISTENT, "The MIDI device is not installed."},
    {MCIERR_SEQ_PORT_MAPNODEVICE, "The MIDI mapper has no device."},
    {MCIERR_SEQ_PORT_MISCERROR, "The MIDI port reported an error."},
    {MCIERR_SEQ_TIMER, "The multimedia timers are in use."},
    {MCIERR_SEQ_PORTUNSPECIFIED, "No MIDI port is selected."},
    {MCIERR_SEQ_NOMIDIPRESENT, "No MIDI device is installed."},
    {MCIERR_NO_WINDOW, "There is no display window."},
    {MCIERR_CREATEWINDOW, "The display window could not be created."},
    {MCIERR_FILE_READ, "The file cannot be read."},
    {MCIERR_FILE_WRITE, "The file cannot be written."},
    {MCIERR_NO_IDENTITY, "The media has no identity."},
};
static const char *error_text(uint32_t code) {
  for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
    if (errors[i].code == code)
      return errors[i].text;
  return 0;
}
BOOL W32API mciGetErrorStringA(uint32_t error, char *out, uint32_t n) {
  const char *text = error_text(error);
  if (!out || !n)
    return 0;
  if (!text) {
    out[0] = 0;
    return 0;
  }
  copy_text(out, n, text);
  return 1;
}
BOOL W32API mciGetErrorStringW(uint32_t error, WCHAR *out, uint32_t n) {
  const char *text = error_text(error);
  if (!out || !n)
    return 0;
  if (!text) {
    out[0] = 0;
    return 0;
  }
  ansi_to_wide(text, out, n);
  return 1;
}
