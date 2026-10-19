/* Original system keyboard/mouse engine; SDK ABI facts, no Wine implementation.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/dinput.h>
#include <w32/input_bridge.h>
extern BOOL W32API SetEvent(HANDLE);
extern DWORD W32API GetCurrentProcessId(void);
#define MAX_OBJECTS 256u
#define MAX_BUFFER 65536u
struct input;
struct device;
struct input_view {
  IDirectInput iface;
  struct input *owner;
  UINT index;
};
struct device_view {
  IDirectInputDevice iface;
  struct device *owner;
  UINT index;
};
struct input {
  struct input_view views[8];
  ULONG refs;
  DWORD version;
};
struct device {
  struct device_view views[8];
  ULONG refs;
  DWORD version;
  BOOL keyboard;
  struct device *next;
  HWND window;
  DWORD coop, bytes, mode, requested, capacity, head, count;
  BOOL acquired, lost, overflow, formatted;
  HANDLE event;
  DWORD offsets[MAX_OBJECTS];
  uint8_t keys[256], buttons[8];
  LONG axes[3], minimum[3], maximum[3];
  DWORD deadzone[3];
  DIDEVICEOBJECTDATA *buffer;
};
static struct device *devices;
static DWORD sequence;
static ULONG live;
static BOOL subscribed;
static const IDirectInputVtbl input_vtable;
static const IDirectInput8Vtbl input8_vtable;
static const IDirectInputDeviceVtbl device_vtable;
static void W32API receive(void *, HWND, const struct ciuki_input_event *,
                           UINT);
static void unlock(int *unused) {
  (void)unused;
  CiukiUserInputUnlock();
}
#define GUARD                                                                  \
  int guard __attribute__((cleanup(unlock))) = (CiukiUserInputLock(), 0)
static void *allocate(DWORD n) { return HeapAlloc(GetProcessHeap(), 8, n); }
static void drop(void *p) {
  if (p)
    HeapFree(GetProcessHeap(), 0, p);
}
static BOOL same(const GUID *a, const GUID *b) {
  if (!a || !b)
    return 0;
  const uint8_t *x = (const void *)a, *y = (const void *)b;
  for (UINT i = 0; i < 16; i++)
    if (x[i] != y[i])
      return 0;
  return 1;
}
static BOOL unknown(const GUID *g) {
  static const GUID iid = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
  return same(g, &iid);
}
static int input_index(const GUID *g) {
  const GUID *ids[] = {&IID_IDirectInputA,  &IID_IDirectInputW,
                       &IID_IDirectInput2A, &IID_IDirectInput2W,
                       &IID_IDirectInput7A, &IID_IDirectInput7W,
                       &IID_IDirectInput8A, &IID_IDirectInput8W};
  for (int i = 0; i < 8; i++)
    if (same(g, ids[i]))
      return i;
  return -1;
}
static int device_index(const GUID *g) {
  const GUID *ids[] = {&IID_IDirectInputDeviceA,  &IID_IDirectInputDeviceW,
                       &IID_IDirectInputDevice2A, &IID_IDirectInputDevice2W,
                       &IID_IDirectInputDevice7A, &IID_IDirectInputDevice7W,
                       &IID_IDirectInputDevice8A, &IID_IDirectInputDevice8W};
  for (int i = 0; i < 8; i++)
    if (same(g, ids[i]))
      return i;
  return -1;
}
static struct input_view *iv(IDirectInput *p) { return (void *)p; }
static struct device_view *dv(IDirectInputDevice *p) { return (void *)p; }
static BOOL subscribe(void) {
  if (!subscribed)
    subscribed = CiukiUserInputSubscribe(receive, 0, 1);
  return subscribed;
}
static void release_subscription(void) {
  if (!live && subscribed) {
    CiukiUserInputSubscribe(receive, 0, 0);
    subscribed = 0;
  }
}
static DWORD device_type(struct device *d) {
  return (d->keyboard ? 0x400u : 0x200u) |
         ((d->version >= 0x800 ? 0x10u : 0) + (d->keyboard ? 3u : 2u));
}
static DWORD object_count(struct device *d) { return d->keyboard ? 256 : 11; }
static DWORD object_type(struct device *d, UINT id) {
  return d->keyboard ? DIDFT_PSHBUTTON | (id << 8)
         : id < 3    ? DIDFT_RELAXIS | (id << 8)
                     : DIDFT_PSHBUTTON | ((id - 3) << 8);
}
static DWORD object_size(struct device *d, UINT id) {
  return !d->keyboard && id < 3 ? 4 : 1;
}
static DWORD default_offset(struct device *d, UINT id) {
  return d->keyboard ? id : id < 3 ? id * 4 : 12 + id - 3;
}
static const GUID *object_guid(struct device *d, UINT id) {
  return d->keyboard ? &GUID_Key
         : id == 0   ? &GUID_XAxis
         : id == 1   ? &GUID_YAxis
         : id == 2   ? &GUID_ZAxis
                     : &GUID_Button;
}
static int object_id(struct device *d, DWORD obj, DWORD how) {
  if (how == DIPH_BYOFFSET) {
    for (UINT i = 0; i < object_count(d); i++)
      if (d->offsets[i] == obj)
        return i;
  } else if (how == DIPH_BYID) {
    for (UINT i = 0; i < object_count(d); i++) {
      DWORD type = object_type(d, i);
      if ((obj & 0xffff00) == (type & 0xffff00) && (obj & 255) & (type & 255))
        return i;
    }
  }
  return -1;
}
static void clear(struct device *d) {
  w32_fill(d->keys, 0, 256);
  w32_fill(d->buttons, 0, 8);
  w32_fill(d->axes, 0, sizeof(d->axes));
  d->head = d->count = 0;
  d->overflow = 0;
}
static BOOL pointer(void) {
  struct w32_input_snapshot state;
  CiukiUserInputSnapshot(&state);
  HWND window = 0;
  UINT flags = 0;
  for (struct device *d = devices; d; d = d->next)
    if (!d->keyboard && d->acquired && d->window == state.foreground) {
      if (d->coop & DISCL_EXCLUSIVE) {
        window = d->window;
        flags = 3;
        break;
      }
      window = d->window;
      flags = 1;
    }
  return CiukiUserInputPointer(window, flags);
}
static HRESULT status(struct device *d) {
  if (d->lost)
    return DIERR_INPUTLOST;
  return d->acquired ? DI_OK : DIERR_NOTACQUIRED;
}
static LONG sum(LONG a, LONG b) {
  int64_t n = (int64_t)a + b;
  return n > 0x7fffffff          ? 0x7fffffff
         : n < (-0x7fffffff - 1) ? (-0x7fffffff - 1)
                                 : (LONG)n;
}
static LONG ranged(struct device *d, UINT axis, LONG value) {
  if (value < d->minimum[axis])
    return d->minimum[axis];
  if (value > d->maximum[axis])
    return d->maximum[axis];
  return value;
}
static void data(struct device *d, UINT id, DWORD value, DWORD time,
                 DWORD serial) {
  if (d->offsets[id] == DI_INFINITE)
    return;
  if (d->capacity) {
    /* Keep the oldest records; discard incoming records once full. */
    if (d->count == d->capacity)
      d->overflow = 1;
    else {
      UINT at = (d->head + d->count) % d->capacity;
      d->buffer[at] =
          (DIDEVICEOBJECTDATA){d->offsets[id], value, time, serial, 0};
      d->count++;
    }
  }
  if (d->event)
    SetEvent(d->event);
}
static void W32API receive(void *context, HWND window,
                           const struct ciuki_input_event *e, UINT kind) {
  (void)context;
  (void)window;
  struct w32_input_snapshot state;
  CiukiUserInputSnapshot(&state);
  if (kind == 2 || kind == 3) {
    for (struct device *d = devices; d; d = d->next) {
      if (kind == 2 || d->window != state.foreground)
        clear(d);
      if (d->acquired && (d->coop & DISCL_FOREGROUND) &&
          (kind == 2 || d->window != state.foreground)) {
        d->acquired = 0;
        d->lost = 1;
        if (d->event)
          SetEvent(d->event);
      }
    }
    pointer();
    return;
  }
  if (!e)
    return;
  DWORD time = (DWORD)(e->monotonic_ns / 1000000), serial = ++sequence;
  for (struct device *d = devices; d; d = d->next)
    if (d->acquired) {
      if (d->keyboard && !kind && e->type == CIUKI_INPUT_KEY) {
        UINT code = e->code == 0x200 ? 0xc5
                    : e->code >= 0x100 && e->code < 0x180
                        ? (e->code & 127) | 128
                        : (UINT)e->code;
        if (code >= 256 || !code)
          continue;
        uint8_t value = e->value ? 128 : 0;
        if (d->keys[code] != value) {
          d->keys[code] = value;
          data(d, code, value, time, serial);
        }
      } else if (!d->keyboard) {
        if (kind == 1 && e->type == CIUKI_INPUT_MOTION &&
            d->mode == DIPROPAXISMODE_REL) {
          LONG values[] = {e->value, e->value2};
          for (UINT i = 0; i < 2; i++)
            if (values[i]) {
              d->axes[i] = sum(d->axes[i], values[i]);
              data(d, i, values[i], time, serial);
            }
        } else if (!kind && e->type == CIUKI_INPUT_MOTION &&
                   d->mode == DIPROPAXISMODE_ABS) {
          for (UINT i = 0; i < 2; i++) {
            LONG value = ranged(d, i, i ? state.cursor.y : state.cursor.x);
            if (value != d->axes[i]) {
              d->axes[i] = value;
              data(d, i, value, time, serial);
            }
          }
        } else if (!kind && e->type == CIUKI_INPUT_WHEEL) {
          int64_t wheel = (int64_t)e->value * 120;
          LONG value = wheel > 0x7fffffff          ? 0x7fffffff
                       : wheel < (-0x7fffffff - 1) ? (-0x7fffffff - 1)
                                                   : (LONG)wheel;
          d->axes[2] = sum(d->axes[2], value);
          if (d->mode == DIPROPAXISMODE_ABS)
            d->axes[2] = ranged(d, 2, d->axes[2]);
          data(d, 2, d->mode == DIPROPAXISMODE_REL ? value : d->axes[2], time,
               serial);
        } else if (!kind && e->type == CIUKI_INPUT_BUTTON && e->code >= 1 &&
                   e->code <= 8) {
          UINT id = e->code - 1;
          uint8_t value = e->value ? 128 : 0;
          if (d->buttons[id] != value) {
            d->buttons[id] = value;
            data(d, 3 + id, value, time, serial);
          }
        }
      }
    }
}
static HRESULT W32API iquery(IDirectInput *p, const GUID *iid, void **out) {
  GUARD;
  if (!out)
    return DIERR_INVALIDPARAM;
  *out = 0;
  struct input *o = iv(p)->owner;
  int i = unknown(iid) ? 0 : input_index(iid);
  if (i < 0 || (i >= 6 && o->version < 0x800))
    return DIERR_NOINTERFACE;
  *out = &o->views[i];
  o->refs++;
  return DI_OK;
}
static ULONG W32API iadd(IDirectInput *p) {
  GUARD;
  return ++iv(p)->owner->refs;
}
static ULONG W32API irelease(IDirectInput *p) {
  GUARD;
  struct input *o = iv(p)->owner;
  ULONG refs = --o->refs;
  if (!refs) {
    drop(o);
    live--;
    release_subscription();
  }
  return refs;
}
static HRESULT W32API dquery(IDirectInputDevice *p, const GUID *iid,
                             void **out) {
  GUARD;
  if (!out)
    return DIERR_INVALIDPARAM;
  *out = 0;
  struct device *o = dv(p)->owner;
  int i = unknown(iid) ? 0 : device_index(iid);
  if (i < 0 || (i >= 6 && o->version < 0x800))
    return DIERR_NOINTERFACE;
  *out = &o->views[i];
  o->refs++;
  return DI_OK;
}
static ULONG W32API dadd(IDirectInputDevice *p) {
  GUARD;
  return ++dv(p)->owner->refs;
}
static ULONG W32API drelease(IDirectInputDevice *p) {
  GUARD;
  struct device *d = dv(p)->owner;
  ULONG refs = --d->refs;
  if (!refs) {
    struct device **link = &devices;
    while (*link && *link != d)
      link = &(*link)->next;
    if (*link)
      *link = d->next;
    drop(d->buffer);
    drop(d);
    live--;
    pointer();
    release_subscription();
  }
  return refs;
}
static HRESULT W32API create_ex(IDirectInput *p, const GUID *guid,
                                const GUID *iid, void **out, void *outer) {
  GUARD;
  if (!out)
    return DIERR_INVALIDPARAM;
  *out = 0;
  if (outer)
    return DIERR_NOAGGREGATION;
  int index = device_index(iid);
  struct input *o = iv(p)->owner;
  if (index < 0 || (index >= 6 && o->version < 0x800))
    return DIERR_NOINTERFACE;
  BOOL keyboard = same(guid, &GUID_SysKeyboard);
  if (!keyboard && !same(guid, &GUID_SysMouse))
    return DIERR_NOTFOUND;
  struct device *d = allocate(sizeof(*d));
  if (!d)
    return DIERR_OUTOFMEMORY;
  d->refs = 1;
  d->version = o->version;
  d->keyboard = keyboard;
  d->mode = DIPROPAXISMODE_REL;
  for (UINT i = 0; i < 8; i++)
    d->views[i] = (struct device_view){{&device_vtable}, d, i};
  for (UINT i = 0; i < MAX_OBJECTS; i++)
    d->offsets[i] = DI_INFINITE;
  for (UINT i = 0; i < 3; i++) {
    d->minimum[i] = (-0x7fffffff - 1);
    d->maximum[i] = 0x7fffffff;
  }
  d->next = devices;
  devices = d;
  live++;
  *out = &d->views[index];
  return DI_OK;
}
static HRESULT W32API create(IDirectInput *p, const GUID *guid,
                             IDirectInputDevice **out, void *outer) {
  const GUID *iid = iv(p)->index >= 6
                        ? (iv(p)->index & 1 ? &IID_IDirectInputDevice8W
                                            : &IID_IDirectInputDevice8A)
                        : (iv(p)->index & 1 ? &IID_IDirectInputDeviceW
                                            : &IID_IDirectInputDeviceA);
  return create_ex(p, guid, iid, (void **)out, outer);
}
/* Build A/W structures by byte offset, never by a host-sized pointer layout. */
static void text(void *out, UINT offset, const char *name, BOOL wide) {
  uint8_t *b = out;
  for (UINT i = 0; name[i] && i < 259; i++) {
    if (wide)
      w32_put16(b + offset + i * 2, (uint8_t)name[i]);
    else
      b[offset + i] = name[i];
  }
}
static HRESULT instance(struct device *d, void *out, BOOL wide) {
  if (!out)
    return DIERR_INVALIDPARAM;
  DWORD size = w32_u32(out), old = wide ? 1080 : 560, full = old + 20;
  if (size != old && size != full)
    return DIERR_INVALIDPARAM;
  w32_fill(out, 0, size);
  w32_put32(out, size);
  const GUID *g = d->keyboard ? &GUID_SysKeyboard : &GUID_SysMouse;
  w32_copy((uint8_t *)out + 4, g, 16);
  w32_copy((uint8_t *)out + 20, g, 16);
  w32_put32((uint8_t *)out + 36, device_type(d));
  const char *name = d->keyboard ? "System Keyboard" : "System Mouse";
  text(out, 40, name, wide);
  text(out, 40 + 260 * (wide ? 2 : 1), name, wide);
  if (size == full) {
    w32_put16((uint8_t *)out + old + 16, 1);
    w32_put16((uint8_t *)out + old + 18, d->keyboard ? 6 : 2);
  }
  return DI_OK;
}
static HRESULT W32API enumerate(IDirectInput *p, DWORD type, DIENUM callback,
                                void *context, DWORD flags) {
  GUARD;
  if (!callback || flags & ~0x70101u)
    return DIERR_INVALIDPARAM;
  if (flags & 0x100)
    return DI_OK;
  struct device d;
  w32_fill(&d, 0, sizeof(d));
  d.version = iv(p)->owner->version;
  BOOL wide = iv(p)->index & 1;
  for (UINT k = 0; k < 2; k++) {
    d.keyboard = k == 1;
    if (type && type != (d.keyboard ? 3u : 2u) &&
        type != (d.keyboard ? 0x13u : 0x12u))
      continue;
    uint8_t record[1100];
    w32_put32(record,
              d.version < 0x500 ? (wide ? 1080 : 560) : (wide ? 1100 : 580));
    instance(&d, record, wide);
    if (!callback(record, context))
      break;
  }
  return DI_OK;
}
static HRESULT W32API device_status(IDirectInput *p, const GUID *guid) {
  (void)p;
  return same(guid, &GUID_SysKeyboard) || same(guid, &GUID_SysMouse)
             ? DI_OK
             : DIERR_NOTFOUND;
}
static HRESULT W32API ipanel(IDirectInput *p, HWND h, DWORD flags) {
  (void)p;
  (void)h;
  return flags ? DIERR_INVALIDPARAM : DI_OK;
}
static HRESULT W32API iinitialize(IDirectInput *p, HINSTANCE h, DWORD version) {
  (void)p;
  (void)h;
  return version >= 0x300 && version <= 0x800 ? DI_OK : DIERR_INVALIDPARAM;
}
static HRESULT W32API find(IDirectInput *p, const GUID *guid, const void *name,
                           GUID *out) {
  (void)p;
  (void)name;
  if (!out)
    return DIERR_INVALIDPARAM;
  if (!same(guid, &GUID_SysKeyboard) && !same(guid, &GUID_SysMouse))
    return DIERR_NOTFOUND;
  *out = *guid;
  return DI_OK;
}
static HRESULT W32API semantics(IDirectInput *p, const void *user, void *format,
                                void *callback, void *context, DWORD flags) {
  (void)p;
  (void)user;
  (void)format;
  (void)callback;
  (void)context;
  (void)flags;
  return DI_OK;
}
static HRESULT W32API configure(IDirectInput *p, void *callback, void *params,
                                DWORD flags, void *context) {
  (void)p;
  (void)callback;
  (void)params;
  (void)flags;
  (void)context;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API capabilities(IDirectInputDevice *p, DIDEVCAPS *caps) {
  GUARD;
  if (!caps || (caps->dwSize != 24 && caps->dwSize != 44))
    return DIERR_INVALIDPARAM;
  struct device *d = dv(p)->owner;
  DWORD size = caps->dwSize;
  w32_fill(caps, 0, size);
  caps->dwSize = size;
  caps->dwFlags = 1 | 4;
  caps->dwDevType = device_type(d);
  caps->dwAxes = d->keyboard ? 0 : 3;
  caps->dwButtons = d->keyboard ? 256 : 8;
  return DI_OK;
}
static HRESULT object_info(struct device_view *v, void *out, UINT id) {
  struct device *d = v->owner;
  BOOL wide = v->index & 1;
  if (!out)
    return DIERR_INVALIDPARAM;
  DWORD size = w32_u32(out), old = wide ? 552 : 292;
  if (size != old && size != old + 24)
    return DIERR_INVALIDPARAM;
  w32_fill(out, 0, size);
  w32_put32(out, size);
  w32_copy((uint8_t *)out + 4, object_guid(d, id), 16);
  w32_put32((uint8_t *)out + 20,
            d->formatted ? d->offsets[id] : default_offset(d, id));
  w32_put32((uint8_t *)out + 24, object_type(d, id));
  char name[32] = {0};
  if (d->keyboard) {
    UINT code = id == 0xc5 ? 0x200 : id & 128 ? 0x100 | (id & 127) : id;
    DWORD lp = (code == 0x200 ? 0x45 : code & 255) << 16;
    if ((code & 0x100) || id == 0x45)
      lp |= 1u << 24;
    GetKeyNameTextA(lp, name, sizeof(name));
    if (!name[0]) {
      w32_copy(name, "Key ", 4);
      name[4] = "0123456789ABCDEF"[id >> 4];
      name[5] = "0123456789ABCDEF"[id & 15];
    }
  } else if (id < 3) {
    name[0] = "XYZ"[id];
    w32_copy(name + 1, " Axis", 6);
  } else {
    w32_copy(name, "Button ", 7);
    name[7] = '0' + id - 3;
  }
  text(out, 32, name, wide);
  return DI_OK;
}
static HRESULT W32API objects(IDirectInputDevice *p, DIENUM callback,
                              void *context, DWORD flags) {
  GUARD;
  if (!callback || flags & ~0xffff0fu)
    return DIERR_INVALIDPARAM;
  struct device_view *v = dv(p);
  struct device *d = v->owner;
  d->refs++;
  for (UINT i = 0; i < object_count(d); i++) {
    if (flags && !(flags & object_type(d, i) & 255))
      continue;
    if ((flags & DIDFT_ANYINSTANCE) &&
        (flags & DIDFT_ANYINSTANCE) != DIDFT_ANYINSTANCE &&
        (flags & DIDFT_ANYINSTANCE) != (object_type(d, i) & DIDFT_ANYINSTANCE))
      continue;
    uint8_t record[576];
    w32_put32(record, d->version < 0x500 ? (v->index & 1 ? 552 : 292)
                                         : (v->index & 1 ? 576 : 316));
    object_info(v, record, i);
    if (!callback(record, context))
      break;
  }
  drelease(p);
  return DI_OK;
}
static HRESULT W32API get_object(IDirectInputDevice *p, void *out, DWORD obj,
                                 DWORD how) {
  GUARD;
  int id = object_id(dv(p)->owner, obj, how);
  return id < 0 ? DIERR_NOTFOUND : object_info(dv(p), out, id);
}
static HRESULT W32API get_info(IDirectInputDevice *p, void *out) {
  GUARD;
  return instance(dv(p)->owner, out, dv(p)->index & 1);
}
static HRESULT W32API format(IDirectInputDevice *p, const DIDATAFORMAT *f) {
  GUARD;
  struct device *d = dv(p)->owner;
  if (d->acquired)
    return DIERR_ACQUIRED;
  if (!f || f->dwSize != sizeof(*f) ||
      f->dwObjSize != sizeof(DIOBJECTDATAFORMAT) ||
      (f->dwFlags != DIDF_RELAXIS && f->dwFlags != DIDF_ABSAXIS) ||
      !f->dwDataSize || f->dwDataSize > 65536 || f->dwNumObjs > MAX_OBJECTS ||
      (f->dwNumObjs && !f->rgodf))
    return DIERR_INVALIDPARAM;
  DWORD offsets[MAX_OBJECTS];
  for (UINT i = 0; i < MAX_OBJECTS; i++)
    offsets[i] = DI_INFINITE;
  for (UINT i = 0; i < f->dwNumObjs; i++) {
    const DIOBJECTDATAFORMAT *o = &f->rgodf[i];
    if (o->dwFlags & ~0x100u ||
        o->dwType & ~(DIDFT_OPTIONAL | DIDFT_ANYINSTANCE | 15u))
      return DIERR_INVALIDPARAM;
    int match = -1;
    for (UINT id = 0; id < object_count(d); id++) {
      if (offsets[id] != DI_INFINITE)
        continue;
      DWORD type = object_type(d, id),
            instance_mask = o->dwType & DIDFT_ANYINSTANCE;
      if (id < 3 && !d->keyboard ? !(o->dwType & DIDFT_AXIS)
                                 : !(o->dwType & type & 255))
        continue;
      if (instance_mask != DIDFT_ANYINSTANCE &&
          instance_mask != (type & DIDFT_ANYINSTANCE))
        continue;
      if (o->pguid && !same(o->pguid, object_guid(d, id)))
        continue;
      match = id;
      break;
    }
    DWORD width = (o->dwType & DIDFT_AXIS) ? 4 : 1;
    if (match >= 0)
      width = object_size(d, match);
    if (o->dwOfs > f->dwDataSize || width > f->dwDataSize - o->dwOfs ||
        (width == 4 && o->dwOfs % 4))
      return DIERR_INVALIDPARAM;
    for (UINT j = 0; j < i; j++) {
      const DIOBJECTDATAFORMAT *prev = &f->rgodf[j];
      DWORD w = (prev->dwType & DIDFT_AXIS) ? 4 : 1;
      if (o->dwOfs < prev->dwOfs + w && prev->dwOfs < o->dwOfs + width)
        return DIERR_INVALIDPARAM;
    }
    if (match < 0) {
      if (o->dwType & DIDFT_OPTIONAL)
        continue;
      return DIERR_NOTFOUND;
    }
    offsets[match] = o->dwOfs;
  }
  w32_copy(d->offsets, offsets, sizeof(offsets));
  d->bytes = f->dwDataSize;
  d->formatted = 1;
  d->mode =
      f->dwFlags == DIDF_RELAXIS ? DIPROPAXISMODE_REL : DIPROPAXISMODE_ABS;
  clear(d);
  return DI_OK;
}
static HRESULT property(struct device *d, const GUID *prop, DIPROPHEADER *h,
                        BOOL set) {
  uintptr_t id = (uintptr_t)prop;
  UINT size = id == 4 ? 24 : 20;
  if (id < 1 || id > 5)
    return DIERR_UNSUPPORTED;
  if (!h || h->dwSize != size || h->dwHeaderSize != 16 || h->dwHow > 2)
    return DIERR_INVALIDPARAM;
  if (set && d->acquired)
    return DIERR_ACQUIRED;
  int object = -1;
  if (h->dwHow == DIPH_DEVICE) {
    if (h->dwObj)
      return DIERR_INVALIDPARAM;
  } else {
    object = object_id(d, h->dwObj, h->dwHow);
    if (object < 0)
      return DIERR_NOTFOUND;
  }
  DIPROPDWORD *value = (void *)h;
  if (id == 1 || id == 2) {
    if (h->dwHow != DIPH_DEVICE)
      return DIERR_INVALIDPARAM;
    if (id == 1) {
      if (set) {
        DWORD capacity =
            value->dwData > MAX_BUFFER ? MAX_BUFFER : value->dwData;
        DIDEVICEOBJECTDATA *b = capacity ? allocate(capacity * sizeof(*b)) : 0;
        if (capacity && !b)
          return DIERR_OUTOFMEMORY;
        drop(d->buffer);
        d->buffer = b;
        d->capacity = capacity;
        d->requested = value->dwData;
        clear(d);
      } else
        value->dwData = d->requested;
    } else {
      if (d->keyboard)
        return DIERR_UNSUPPORTED;
      if (set) {
        if (value->dwData > 1)
          return DIERR_INVALIDPARAM;
        d->mode = value->dwData;
        clear(d);
      } else
        value->dwData = d->mode;
    }
    return DI_OK;
  }
  if (d->keyboard || object >= 3)
    return DIERR_UNSUPPORTED;
  if (id == 3) {
    if (set)
      return DIERR_UNSUPPORTED;
    value->dwData = 1;
    return DI_OK;
  }
  UINT first = object < 0 ? 0 : (UINT)object, last = object < 0 ? 3 : first + 1;
  if (id == 4) {
    DIPROPRANGE *r = (void *)h;
    if (set && r->lMin >= r->lMax)
      return DIERR_INVALIDPARAM;
    if (set)
      for (UINT i = first; i < last; i++) {
        d->minimum[i] = r->lMin;
        d->maximum[i] = r->lMax;
      }
    else {
      r->lMin = d->minimum[first];
      r->lMax = d->maximum[first];
    }
  } else {
    if (set && value->dwData > 10000)
      return DIERR_INVALIDPARAM;
    if (set)
      for (UINT i = first; i < last; i++)
        d->deadzone[i] = value->dwData;
    else
      value->dwData = d->deadzone[first];
  }
  return DI_OK;
}
static HRESULT W32API get_property(IDirectInputDevice *p, const GUID *prop,
                                   DIPROPHEADER *h) {
  GUARD;
  return property(dv(p)->owner, prop, h, 0);
}
static HRESULT W32API set_property(IDirectInputDevice *p, const GUID *prop,
                                   const DIPROPHEADER *h) {
  GUARD;
  if (!h)
    return DIERR_INVALIDPARAM;
  uint8_t copy[24];
  if (h->dwSize > 24 || h->dwSize < 16)
    return DIERR_INVALIDPARAM;
  w32_copy(copy, h, h->dwSize);
  return property(dv(p)->owner, prop, (void *)copy, 1);
}
static HRESULT W32API cooperative(IDirectInputDevice *p, HWND window,
                                  DWORD flags) {
  GUARD;
  struct device *d = dv(p)->owner;
  if (d->acquired)
    return DIERR_ACQUIRED;
  if (flags & ~31u || ((flags & 3) != 1 && (flags & 3) != 2) ||
      ((flags & 12) != 4 && (flags & 12) != 8))
    return DIERR_INVALIDPARAM;
  if ((flags & (DISCL_EXCLUSIVE | DISCL_BACKGROUND)) ==
      (DISCL_EXCLUSIVE | DISCL_BACKGROUND))
    return DIERR_UNSUPPORTED;
  if (!window || !IsWindow(window) ||
      (GetWindowLongA(window, -16) & WS_CHILD) ||
      GetWindowThreadProcessId(window, 0) == 0)
    return DIERR_INVALIDPARAM;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != GetCurrentProcessId())
    return DIERR_INVALIDPARAM;
  d->window = window;
  d->coop = flags;
  return DI_OK;
}
static HRESULT W32API acquire(IDirectInputDevice *p) {
  GUARD;
  struct device *d = dv(p)->owner;
  if (!d->formatted || !d->coop)
    return DIERR_NOTINITIALIZED;
  if (d->acquired)
    return DI_NOEFFECT;
  struct w32_input_snapshot state;
  CiukiUserInputSnapshot(&state);
  if (!IsWindow(d->window) ||
      ((d->coop & DISCL_FOREGROUND) && state.foreground != d->window))
    return DIERR_OTHERAPPHASPRIO;
  for (struct device *other = devices; other; other = other->next)
    if (other != d && other->acquired && other->keyboard == d->keyboard &&
        (other->coop & d->coop & DISCL_EXCLUSIVE))
      return DIERR_OTHERAPPHASPRIO;
  clear(d);
  w32_copy(d->keys, state.keys, 256);
  w32_copy(d->buttons, state.buttons, 8);
  d->lost = 0;
  d->acquired = 1;
  d->refs++;
  BOOL ok = pointer();
  if (!ok) {
    d->acquired = 0;
    clear(d);
  }
  HRESULT hr = !ok           ? DIERR_OTHERAPPHASPRIO
               : d->acquired ? DI_OK
                             : DIERR_INPUTLOST;
  drelease(p);
  return hr;
}
static HRESULT W32API unacquire(IDirectInputDevice *p) {
  GUARD;
  struct device *d = dv(p)->owner;
  BOOL acquired = d->acquired;
  d->acquired = d->lost = 0;
  clear(d);
  pointer();
  return acquired ? DI_OK : DI_NOEFFECT;
}
static HRESULT W32API get_state(IDirectInputDevice *p, DWORD bytes, void *out) {
  GUARD;
  struct device *d = dv(p)->owner;
  /* The pump may dispatch application callbacks; retain this interface. */
  d->refs++;
  CiukiUserInputPump();
  HRESULT hr = status(d);
  if (!hr && (!out || bytes != d->bytes))
    hr = DIERR_INVALIDPARAM;
  if (!hr) {
    w32_fill(out, 0, bytes);
    struct w32_input_snapshot snapshot;
    CiukiUserInputSnapshot(&snapshot);
    if (!d->keyboard && d->mode == DIPROPAXISMODE_ABS) {
      d->axes[0] = ranged(d, 0, snapshot.cursor.x);
      d->axes[1] = ranged(d, 1, snapshot.cursor.y);
    }
    for (UINT id = 0; id < object_count(d); id++)
      if (d->offsets[id] != DI_INFINITE) {
        uint8_t *target = (uint8_t *)out + d->offsets[id];
        if (d->keyboard)
          *target = d->keys[id];
        else if (id < 3)
          w32_put32(target, d->axes[id]);
        else
          *target = d->buttons[id - 3];
      }
    if (!d->keyboard && d->mode == DIPROPAXISMODE_REL)
      w32_fill(d->axes, 0, sizeof(d->axes));
  }
  drelease(p);
  return hr;
}
static HRESULT W32API get_data(IDirectInputDevice *p, DWORD size, void *out,
                               DWORD *count, DWORD flags) {
  GUARD;
  struct device *d = dv(p)->owner;
  if (!count || (size != 16 && (size != 20 || d->version < 0x800)) ||
      flags & ~DIGDD_PEEK)
    return DIERR_INVALIDPARAM;
  d->refs++;
  CiukiUserInputPump();
  HRESULT hr = status(d);
  if (!hr && !d->capacity)
    hr = DIERR_NOTBUFFERED;
  if (!hr) {
    DWORD n = *count;
    if (n > d->count)
      n = d->count;
    if (out)
      for (UINT i = 0; i < n; i++)
        w32_copy((uint8_t *)out + i * size,
                 &d->buffer[(d->head + i) % d->capacity], size);
    *count = n;
    hr = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    if (!(flags & DIGDD_PEEK)) {
      d->head = (d->head + n) % d->capacity;
      d->count -= n;
      d->overflow = 0;
    }
  } else
    *count = 0;
  drelease(p);
  return hr;
}
static HRESULT W32API notification(IDirectInputDevice *p, HANDLE event) {
  GUARD;
  struct device *d = dv(p)->owner;
  if (d->acquired)
    return DIERR_ACQUIRED;
  d->event = event;
  return DI_OK;
}
static HRESULT W32API poll(IDirectInputDevice *p) {
  (void)p;
  return DI_NOEFFECT;
}
static HRESULT W32API dpanel(IDirectInputDevice *p, HWND h, DWORD flags) {
  (void)p;
  (void)h;
  return flags ? DIERR_INVALIDPARAM : DI_OK;
}
static HRESULT W32API dinitialize(IDirectInputDevice *p, HINSTANCE h,
                                  DWORD version, const GUID *g) {
  (void)h;
  struct device *d = dv(p)->owner;
  return version >= 0x300 && version <= 0x800 &&
                 same(g, d->keyboard ? &GUID_SysKeyboard : &GUID_SysMouse)
             ? DI_OK
             : DIERR_INVALIDPARAM;
}
/* Unsupported methods keep every interface's exact stdcall arity. */
static HRESULT W32API effect(IDirectInputDevice *p, const GUID *g,
                             const void *params, void **out, void *outer) {
  (void)p;
  (void)g;
  (void)params;
  (void)outer;
  if (out)
    *out = 0;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API enum_effects(IDirectInputDevice *p, DIENUM cb, void *ctx,
                                   DWORD flags) {
  (void)p;
  (void)cb;
  (void)ctx;
  (void)flags;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API effect_info(IDirectInputDevice *p, void *out,
                                  const GUID *g) {
  (void)p;
  (void)out;
  (void)g;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API ffstate(IDirectInputDevice *p, DWORD *out) {
  (void)p;
  if (out)
    *out = 0;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API command(IDirectInputDevice *p, DWORD cmd) {
  (void)p;
  (void)cmd;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API escape(IDirectInputDevice *p, void *params) {
  (void)p;
  (void)params;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API send_data(IDirectInputDevice *p, DWORD size,
                                const void *data, DWORD *count, DWORD flags) {
  (void)p;
  (void)size;
  (void)data;
  (void)flags;
  if (count)
    *count = 0;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API effects_file(IDirectInputDevice *p, const void *name,
                                   DIENUM cb, void *ctx, DWORD flags) {
  (void)p;
  (void)name;
  (void)cb;
  (void)ctx;
  (void)flags;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API write_file(IDirectInputDevice *p, const void *name,
                                 DWORD n, const void *data, DWORD flags) {
  (void)p;
  (void)name;
  (void)n;
  (void)data;
  (void)flags;
  return DIERR_UNSUPPORTED;
}
static HRESULT W32API action(IDirectInputDevice *p, void *map, const void *user,
                             DWORD flags) {
  (void)p;
  (void)map;
  (void)user;
  (void)flags;
  return DIERR_UNSUPPORTED;
}
static const IDirectInputDeviceVtbl device_vtable = {
    dquery,       dadd,         drelease,     capabilities, objects,
    get_property, set_property, acquire,      unacquire,    get_state,
    get_data,     format,       notification, cooperative,  get_object,
    get_info,     dpanel,       dinitialize,  effect,       enum_effects,
    effect_info,  ffstate,      command,      enum_effects, escape,
    poll,         send_data,    effects_file, write_file,   action,
    action,       escape};
static const IDirectInputVtbl input_vtable = {
    iquery,        iadd,   irelease,    create, enumerate,
    device_status, ipanel, iinitialize, find,   create_ex};
static const IDirectInput8Vtbl input8_vtable = {
    iquery, iadd,        irelease, create,    enumerate, device_status,
    ipanel, iinitialize, find,     semantics, configure};
HRESULT W32API DirectInputCreateEx(HINSTANCE instance, DWORD version,
                                   const GUID *iid, void **out, void *outer) {
  GUARD;
  (void)instance;
  if (!out)
    return DIERR_INVALIDPARAM;
  *out = 0;
  if (outer)
    return DIERR_NOAGGREGATION;
  int index = input_index(iid);
  if (version < 0x300 || version > 0x800)
    return DIERR_INVALIDPARAM;
  if (index < 0 || (index >= 6 && version < 0x800))
    return DIERR_NOINTERFACE;
  struct input *o = allocate(sizeof(*o));
  if (!o)
    return DIERR_OUTOFMEMORY;
  if (!subscribe()) {
    drop(o);
    return DIERR_OUTOFMEMORY;
  }
  o->refs = 1;
  o->version = version;
  for (UINT i = 0; i < 8; i++)
    o->views[i] = (struct input_view){
        {i >= 6 ? (const void *)&input8_vtable : &input_vtable}, o, i};
  *out = &o->views[index];
  live++;
  return DI_OK;
}
HRESULT W32API DirectInputCreateA(HINSTANCE h, DWORD version,
                                  IDirectInput **out, void *outer) {
  return DirectInputCreateEx(h, version, &IID_IDirectInputA, (void **)out,
                             outer);
}
HRESULT W32API DirectInputCreateW(HINSTANCE h, DWORD version,
                                  IDirectInput **out, void *outer) {
  return DirectInputCreateEx(h, version, &IID_IDirectInputW, (void **)out,
                             outer);
}
HRESULT W32API DirectInput8Create(HINSTANCE h, DWORD version, const GUID *iid,
                                  void **out, void *outer) {
  if (version != 0x800 || input_index(iid) < 6) {
    if (out)
      *out = 0;
    return DIERR_INVALIDPARAM;
  }
  return DirectInputCreateEx(h, version, iid, out, outer);
}
HRESULT W32API DllCanUnloadNow(void) {
  GUARD;
  return live ? 1 : 0;
}
HRESULT W32API DllGetClassObject(const GUID *cls, const GUID *iid, void **out) {
  (void)cls;
  (void)iid;
  if (out)
    *out = 0;
  static unsigned once;
  if (!__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED))
    w32_diag("stub", "dinput.dll", "DllGetClassObject");
  SetLastError(120);
  return DIERR_UNSUPPORTED;
}
HRESULT W32API DllRegisterServer(void) { return DI_OK; }
HRESULT W32API DllUnregisterServer(void) { return DI_OK; }
