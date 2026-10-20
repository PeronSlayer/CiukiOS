/* Menus: objects, MENU/MENUEX resources, the menu bar in the top-level
 * surface, popup windows (#32768), keyboard/mouse tracking, the system
 * menu subset and accelerator tables.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "unicode.h"
#define MENUS 256u
#define ITEMS_MAX 512u
#define ACCELS 64u
#define DEPTH 8u
#define DC32(dc) U32_PTR(dc)
struct mitem {
  UINT type, state, id;
  HMENU sub;
  char *text;
  uintptr_t data;
  HANDLE checked, unchecked, bitmap;
  RECT rect;
};
struct menu {
  UINT generation;
  BOOL used, popup, system;
  struct mitem *items;
  UINT count, capacity;
  HWND owner;
  int width, height;
  /* f5-20B MENUINFO record (Get/SetMenuInfo). */
  DWORD info_style, help_id;
  UINT max_height;
  HBRUSH background;
  uintptr_t menu_data;
};
static struct menu menus[MENUS];
static HMENU handle_of(UINT i) {
  return (HMENU)(uintptr_t)(0x20000000u | (menus[i].generation << 8) | i);
}
static struct menu *menu(HMENU h) {
  uintptr_t v = (uintptr_t)h;
  UINT i = (UINT)(v & 0xff);
  if ((v & 0xf0000000u) != 0x20000000u || i >= MENUS || !menus[i].used ||
      ((v >> 8) & 0xfffffu) != menus[i].generation)
    return 0;
  return &menus[i];
}
static HMENU create(BOOL popup) {
  for (UINT i = 0; i < MENUS; i++)
    if (!menus[i].used) {
      UINT generation = (menus[i].generation + 1) & 0xfffffu;
      w32_fill(&menus[i], 0, sizeof(menus[i]));
      menus[i].generation = generation ? generation : 1;
      menus[i].used = 1;
      menus[i].popup = popup;
      return handle_of(i);
    }
  u32_fail(8);
  return 0;
}
HMENU W32API CreateMenu(void) {
  U32_GUARD;
  return create(0);
}
HMENU W32API CreatePopupMenu(void) {
  U32_GUARD;
  return create(1);
}
BOOL W32API IsMenu(HMENU h) {
  U32_GUARD;
  return menu(h) != 0;
}
static void free_items(struct menu *m, BOOL recursive);
BOOL W32API DestroyMenu(HMENU h) {
  U32_GUARD;
  struct menu *m = menu(h);
  if (!m)
    return u32_fail(1401);
  free_items(m, 1);
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    if (u32_windows[i].menu == h)
      u32_windows[i].menu = 0;
    if (u32_windows[i].system_menu == h)
      u32_windows[i].system_menu = 0;
  }
  UINT generation = m->generation;
  w32_fill(m, 0, sizeof(*m));
  m->generation = generation;
  return 1;
}
static void free_items(struct menu *m, BOOL recursive) {
  for (UINT i = 0; i < m->count; i++) {
    u32_free(m->items[i].text);
    if (recursive && m->items[i].sub)
      DestroyMenu(m->items[i].sub);
  }
  u32_free(m->items);
  m->items = 0;
  m->count = m->capacity = 0;
}
/* Item lookup: by position, or by command searching popups depth-first. */
static struct mitem *find(HMENU h, UINT item, UINT flags, struct menu **owner,
                          UINT *index) {
  struct menu *m = menu(h);
  if (!m)
    return 0;
  if (flags & MF_BYPOSITION) {
    if (item >= m->count)
      return 0;
    if (owner)
      *owner = m;
    if (index)
      *index = item;
    return &m->items[item];
  }
  for (UINT i = 0; i < m->count; i++) {
    if (!m->items[i].sub && m->items[i].id == item) {
      if (owner)
        *owner = m;
      if (index)
        *index = i;
      return &m->items[i];
    }
    if (m->items[i].sub) {
      struct mitem *found = find(m->items[i].sub, item, flags, owner, index);
      if (found)
        return found;
      if (m->items[i].id == item && (uintptr_t)m->items[i].sub == item) {
        if (owner)
          *owner = m;
        if (index)
          *index = i;
        return &m->items[i];
      }
    }
  }
  return 0;
}
static void touched(struct menu *m) {
  if (m && m->owner && IsWindow(m->owner))
    DrawMenuBar(m->owner);
}
static BOOL set_item(struct mitem *it, UINT flags, uintptr_t id,
                     const char *data) {
  char *text = 0;
  if (!(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) {
    const char *s = data ? data : "";
    UINT n = w32_strlen(s);
    if (n > 255)
      n = 255;
    text = u32_alloc(n + 1);
    if (!text)
      return 0;
    w32_copy(text, s, n);
    text[n] = 0;
  }
  u32_free(it->text);
  it->text = text;
  it->type = flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR |
                      MF_MENUBREAK | MF_MENUBARBREAK | MF_RIGHTJUSTIFY |
                      MFT_RADIOCHECK);
  it->state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_HILITE |
                       MF_DEFAULT);
  if (flags & MF_POPUP) {
    it->sub = (HMENU)id;
    it->id = (UINT)id;
    struct menu *s = menu(it->sub);
    if (s)
      s->popup = 1;
  } else {
    it->sub = 0;
    it->id = (UINT)id;
  }
  if (flags & MF_BITMAP)
    it->bitmap = (HANDLE)data;
  if (flags & MF_OWNERDRAW)
    it->data = (uintptr_t)data;
  return 1;
}
static BOOL insert(HMENU h, UINT at, UINT flags, uintptr_t id,
                   const char *data, BOOL by_position) {
  struct menu *m = menu(h);
  if (!m)
    return u32_fail(1401);
  if ((flags & MF_POPUP) && !menu((HMENU)id))
    return u32_fail(1401);
  if (m->count >= ITEMS_MAX)
    return u32_fail(8);
  UINT index = m->count;
  if (by_position) {
    if (at < m->count)
      index = at;
  } else if (at != (UINT)-1) {
    struct menu *owner;
    UINT i;
    if (find(h, at, 0, &owner, &i)) {
      m = owner;
      index = i;
    }
  }
  if (m->count == m->capacity) {
    UINT cap = m->capacity ? m->capacity * 2 : 8;
    struct mitem *p = u32_alloc(cap * sizeof(*p));
    if (!p)
      return u32_fail(8);
    if (m->items)
      w32_copy(p, m->items, m->count * sizeof(*p));
    u32_free(m->items);
    m->items = p;
    m->capacity = cap;
  }
  for (UINT i = m->count; i > index; i--)
    m->items[i] = m->items[i - 1];
  w32_fill(&m->items[index], 0, sizeof(m->items[index]));
  if (!set_item(&m->items[index], flags, id, data)) {
    for (UINT i = index; i + 1 <= m->count; i++)
      m->items[i] = m->items[i + 1];
    return u32_fail(8);
  }
  m->count++;
  touched(m);
  return 1;
}
BOOL W32API AppendMenuA(HMENU h, UINT flags, uintptr_t id, const char *data) {
  U32_GUARD;
  return insert(h, (UINT)-1, flags & ~MF_BYPOSITION, id, data, 1);
}
BOOL W32API InsertMenuA(HMENU h, UINT at, UINT flags, uintptr_t id,
                        const char *data) {
  U32_GUARD;
  return insert(h, at, flags, id, data, !!(flags & MF_BYPOSITION));
}
BOOL W32API ModifyMenuA(HMENU h, UINT at, UINT flags, uintptr_t id,
                        const char *data) {
  U32_GUARD;
  struct menu *owner;
  struct mitem *it = find(h, at, flags, &owner, 0);
  if (!it)
    return u32_fail(1456);
  if (it->sub && it->sub != (HMENU)id && (flags & MF_POPUP))
    DestroyMenu(it->sub);
  if (!set_item(it, flags, id, data))
    return u32_fail(8);
  touched(owner);
  return 1;
}
static BOOL remove_item(HMENU h, UINT at, UINT flags, BOOL destroy) {
  struct menu *owner;
  UINT index;
  struct mitem *it = find(h, at, flags, &owner, &index);
  if (!it)
    return u32_fail(1456);
  HMENU sub = it->sub;
  u32_free(it->text);
  for (UINT i = index; i + 1 < owner->count; i++)
    owner->items[i] = owner->items[i + 1];
  owner->count--;
  if (destroy && sub)
    DestroyMenu(sub);
  touched(owner);
  return 1;
}
BOOL W32API DeleteMenu(HMENU h, UINT at, UINT flags) {
  U32_GUARD;
  return remove_item(h, at, flags, 1);
}
BOOL W32API RemoveMenu(HMENU h, UINT at, UINT flags) {
  U32_GUARD;
  return remove_item(h, at, flags, 0);
}
DWORD W32API CheckMenuItem(HMENU h, UINT at, UINT flags) {
  U32_GUARD;
  struct mitem *it = find(h, at, flags, 0, 0);
  if (!it)
    return (DWORD)-1;
  DWORD old = it->state & MF_CHECKED;
  it->state = (it->state & ~MF_CHECKED) | (flags & MF_CHECKED);
  return old;
}
BOOL W32API CheckMenuRadioItem(HMENU h, UINT first, UINT last, UINT check,
                               UINT flags) {
  U32_GUARD;
  struct menu *owner;
  UINT a, b;
  if (!find(h, first, flags, &owner, &a))
    return 0;
  struct menu *o2;
  if (!find(h, last, flags, &o2, &b) || o2 != owner)
    return 0;
  if (a > b) {
    UINT t = a;
    a = b;
    b = t;
  }
  BOOL found = 0;
  for (UINT i = a; i <= b; i++) {
    struct mitem *it = &owner->items[i];
    BOOL on = flags & MF_BYPOSITION ? i == check : it->id == check;
    if (on) {
      it->state |= MF_CHECKED;
      it->type |= MFT_RADIOCHECK;
      found = 1;
    } else
      it->state &= ~MF_CHECKED;
  }
  return found;
}
UINT W32API EnableMenuItem(HMENU h, UINT at, UINT flags) {
  U32_GUARD;
  struct menu *owner;
  struct mitem *it = find(h, at, flags, &owner, 0);
  if (!it)
    return (UINT)-1;
  UINT old = it->state & (MF_GRAYED | MF_DISABLED);
  it->state = (it->state & ~(MF_GRAYED | MF_DISABLED)) |
              (flags & (MF_GRAYED | MF_DISABLED));
  if (!owner->popup)
    touched(owner);
  return old;
}
BOOL W32API HiliteMenuItem(HWND w, HMENU h, UINT at, UINT flags) {
  U32_GUARD;
  struct mitem *it = find(h, at, flags, 0, 0);
  if (!it)
    return 0;
  it->state = (it->state & ~MF_HILITE) | (flags & MF_HILITE);
  if (IsWindow(w))
    DrawMenuBar(w);
  return 1;
}
int W32API GetMenuItemCount(HMENU h) {
  U32_GUARD;
  struct menu *m = menu(h);
  return m ? (int)m->count : (u32_fail(1401), -1);
}
UINT W32API GetMenuItemID(HMENU h, int pos) {
  U32_GUARD;
  struct menu *m = menu(h);
  if (!m || pos < 0 || (UINT)pos >= m->count || m->items[pos].sub)
    return (UINT)-1;
  return m->items[pos].id;
}
UINT W32API GetMenuState(HMENU h, UINT at, UINT flags) {
  U32_GUARD;
  struct mitem *it = find(h, at, flags, 0, 0);
  if (!it)
    return (UINT)-1;
  UINT state = it->state | (it->type & ~MFT_RADIOCHECK);
  if (it->sub) {
    struct menu *s = menu(it->sub);
    return (state & 0xff) | MF_POPUP | ((s ? s->count : 0) << 8);
  }
  return state;
}
int W32API GetMenuStringA(HMENU h, UINT at, char *out, int n, UINT flags) {
  U32_GUARD;
  struct mitem *it = find(h, at, flags, 0, 0);
  if (!it)
    return 0;
  const char *s = it->text ? it->text : "";
  int length = (int)w32_strlen(s);
  if (!out)
    return length;
  if (n <= 0)
    return 0;
  int k = length < n - 1 ? length : n - 1;
  w32_copy(out, s, (DWORD)k);
  out[k] = 0;
  return k;
}
HMENU W32API GetSubMenu(HMENU h, int pos) {
  U32_GUARD;
  struct menu *m = menu(h);
  if (!m || pos < 0 || (UINT)pos >= m->count)
    return 0;
  return m->items[pos].sub;
}
LONG W32API GetMenuCheckMarkDimensions(void) {
  return 13 | (13 << 16);
}
BOOL W32API SetMenuItemBitmaps(HMENU h, UINT at, UINT flags, HANDLE unchecked,
                               HANDLE checked) {
  U32_GUARD;
  struct mitem *it = find(h, at, flags, 0, 0);
  if (!it)
    return 0;
  it->unchecked = unchecked;
  it->checked = checked;
  return 1;
}
static BOOL info_size(const MENUITEMINFOA *i) {
  return i && (i->cbSize == sizeof(*i) || i->cbSize == sizeof(*i) - sizeof(HANDLE));
}
static BOOL apply_info(struct mitem *it, const MENUITEMINFOA *i) {
  if (i->fMask & MIIM_TYPE) {
    UINT flags = i->fType;
    if (!set_item(it, (flags & ~MF_POPUP) | (it->sub ? MF_POPUP : 0),
                  it->sub ? (uintptr_t)it->sub : it->id,
                  flags & (MF_BITMAP | MF_OWNERDRAW) ? (const char *)i->dwTypeData
                                                     : i->dwTypeData))
      return 0;
  }
  if (i->fMask & MIIM_FTYPE)
    it->type = (it->type & (MF_BITMAP | MF_OWNERDRAW)) | (i->fType & ~(MF_BITMAP | MF_OWNERDRAW));
  if (i->fMask & MIIM_STRING) {
    char *text = 0;
    const char *s = i->dwTypeData ? i->dwTypeData : "";
    UINT n = w32_strlen(s);
    if (n > 255)
      n = 255;
    text = u32_alloc(n + 1);
    if (!text)
      return 0;
    w32_copy(text, s, n);
    text[n] = 0;
    u32_free(it->text);
    it->text = text;
  }
  if (i->fMask & MIIM_STATE)
    it->state = i->fState & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_HILITE |
                             MF_DEFAULT);
  if (i->fMask & MIIM_ID)
    it->id = i->wID;
  if (i->fMask & MIIM_SUBMENU) {
    it->sub = i->hSubMenu;
    struct menu *s = menu(it->sub);
    if (s)
      s->popup = 1;
  }
  if (i->fMask & MIIM_CHECKMARKS) {
    it->checked = i->hbmpChecked;
    it->unchecked = i->hbmpUnchecked;
  }
  if (i->fMask & MIIM_DATA)
    it->data = i->dwItemData;
  if ((i->fMask & MIIM_BITMAP) && i->cbSize == sizeof(*i))
    it->bitmap = i->hbmpItem;
  return 1;
}
BOOL W32API InsertMenuItemA(HMENU h, UINT at, BOOL by_position,
                            const MENUITEMINFOA *i) {
  U32_GUARD;
  if (!info_size(i))
    return u32_fail(87);
  UINT flags = (i->fMask & MIIM_TYPE ? i->fType : MF_STRING) |
               (i->fMask & MIIM_SUBMENU && i->hSubMenu ? MF_POPUP : 0);
  if (!(i->fMask & (MIIM_TYPE | MIIM_STRING)))
    flags |= MF_STRING;
  const char *data = i->fMask & (MIIM_TYPE | MIIM_STRING) ? i->dwTypeData : "";
  uintptr_t id = flags & MF_POPUP ? (uintptr_t)i->hSubMenu
                                  : (i->fMask & MIIM_ID ? i->wID : 0);
  if (!insert(h, at, flags, id, data, by_position))
    return 0;
  struct menu *m = menu(h);
  UINT index = by_position ? (at < m->count ? at : m->count - 1) : 0;
  struct mitem *it = by_position ? &m->items[index]
                                 : find(h, at, 0, 0, 0);
  if (!by_position && !it)
    it = &m->items[m->count - 1];
  MENUITEMINFOA rest = *i;
  rest.fMask &= ~(MIIM_TYPE | MIIM_SUBMENU | MIIM_STRING);
  return apply_info(it, &rest);
}
BOOL W32API SetMenuItemInfoA(HMENU h, UINT at, BOOL by_position,
                             const MENUITEMINFOA *i) {
  U32_GUARD;
  if (!info_size(i))
    return u32_fail(87);
  struct menu *owner;
  struct mitem *it = find(h, at, by_position ? MF_BYPOSITION : 0, &owner, 0);
  if (!it)
    return u32_fail(1456);
  if (!apply_info(it, i))
    return 0;
  touched(owner);
  return 1;
}
BOOL W32API GetMenuItemInfoA(HMENU h, UINT at, BOOL by_position,
                             MENUITEMINFOA *i) {
  U32_GUARD;
  if (!info_size(i))
    return u32_fail(87);
  struct mitem *it = find(h, at, by_position ? MF_BYPOSITION : 0, 0, 0);
  if (!it)
    return u32_fail(1456);
  if (i->fMask & (MIIM_TYPE | MIIM_FTYPE))
    i->fType = it->type;
  if (i->fMask & MIIM_STATE)
    i->fState = it->state;
  if (i->fMask & MIIM_ID)
    i->wID = it->id;
  if (i->fMask & MIIM_SUBMENU)
    i->hSubMenu = it->sub;
  if (i->fMask & MIIM_CHECKMARKS) {
    i->hbmpChecked = it->checked;
    i->hbmpUnchecked = it->unchecked;
  }
  if (i->fMask & MIIM_DATA)
    i->dwItemData = it->data;
  if ((i->fMask & MIIM_BITMAP) && i->cbSize == sizeof(*i))
    i->hbmpItem = it->bitmap;
  if ((i->fMask & (MIIM_TYPE | MIIM_STRING)) &&
      !(it->type & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) {
    const char *s = it->text ? it->text : "";
    UINT n = w32_strlen(s);
    if (i->dwTypeData && i->cch) {
      UINT k = n < i->cch - 1 ? n : i->cch - 1;
      w32_copy(i->dwTypeData, s, k);
      i->dwTypeData[k] = 0;
      i->cch = k;
    } else
      i->cch = n;
  } else if (i->fMask & MIIM_TYPE)
    i->dwTypeData = it->type & MF_BITMAP ? (char *)it->bitmap
                                         : (char *)it->data;
  return 1;
}
BOOL W32API SetMenuDefaultItem(HMENU h, UINT item, UINT by_position) {
  U32_GUARD;
  struct menu *m = menu(h);
  if (!m)
    return 0;
  for (UINT i = 0; i < m->count; i++)
    m->items[i].state &= ~MF_DEFAULT;
  if (item == (UINT)-1)
    return 1;
  struct mitem *it = find(h, item, by_position ? MF_BYPOSITION : 0, 0, 0);
  if (!it)
    return 0;
  it->state |= MF_DEFAULT;
  return 1;
}
UINT W32API GetMenuDefaultItem(HMENU h, UINT by_position, UINT flags) {
  U32_GUARD;
  (void)flags;
  struct menu *m = menu(h);
  for (UINT i = 0; m && i < m->count; i++)
    if (m->items[i].state & MF_DEFAULT)
      return by_position ? i : m->items[i].id;
  return (UINT)-1;
}

/* ---- MENU and MENUEX resources -------------------------------------------- */
struct reader {
  const uint8_t *p;
  DWORD n, at;
  BOOL bad;
};
static UINT r16(struct reader *r) {
  if (r->bad || r->at + 2 > r->n) {
    r->bad = 1;
    return 0;
  }
  UINT v = w32_u16(r->p + r->at);
  r->at += 2;
  return v;
}
static DWORD r32(struct reader *r) {
  UINT lo = r16(r);
  return lo | ((DWORD)r16(r) << 16);
}
static void rtext(struct reader *r, char *out, UINT cap) {
  UINT n = 0;
  for (;;) {
    UINT c = r16(r);
    if (r->bad || !c)
      break;
    if (n + 1 < cap) {
      int a = u32_ansi(c);
      out[n++] = (char)(a >= 0 ? a : '?');
    }
  }
  out[n] = 0;
}
static BOOL parse_items(struct reader *r, HMENU h, UINT depth) {
  if (depth > 8)
    return 0;
  for (UINT guard = 0; guard < ITEMS_MAX; guard++) {
    UINT flags = r16(r), id = 0;
    if (!(flags & MF_POPUP))
      id = r16(r);
    char text[256];
    rtext(r, text, sizeof(text));
    if (r->bad)
      return 0;
    BOOL separator = !(flags & MF_POPUP) && !id && !text[0];
    if (flags & MF_POPUP) {
      HMENU sub = CreatePopupMenu();
      if (!sub)
        return 0;
      if (!parse_items(r, sub, depth + 1) ||
          !AppendMenuA(h, (flags & ~MF_END) | MF_POPUP, (uintptr_t)sub, text)) {
        DestroyMenu(sub);
        return 0;
      }
    } else if (!AppendMenuA(h, (flags & ~MF_END) | (separator ? MF_SEPARATOR : 0),
                            id, text))
      return 0;
    if (flags & MF_END)
      return 1;
  }
  return 0;
}
static void align4(struct reader *r) {
  r->at = (r->at + 3) & ~3u;
  if (r->at > r->n)
    r->bad = 1;
}
static BOOL parse_ex(struct reader *r, HMENU h, UINT depth) {
  if (depth > 8)
    return 0;
  for (UINT guard = 0; guard < ITEMS_MAX; guard++) {
    align4(r);
    DWORD type = r32(r), state = r32(r), id = r32(r);
    UINT flags = r16(r);
    char text[256];
    rtext(r, text, sizeof(text));
    if (r->bad)
      return 0;
    MENUITEMINFOA info = {sizeof(info),
                          MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_STRING,
                          type,
                          state,
                          id,
                          0,
                          0,
                          0,
                          0,
                          text,
                          0,
                          0};
    if (flags & 1) {
      align4(r);
      r32(r);
      HMENU sub = CreatePopupMenu();
      if (!sub || !parse_ex(r, sub, depth + 1)) {
        if (sub)
          DestroyMenu(sub);
        return 0;
      }
      info.fMask |= MIIM_SUBMENU;
      info.hSubMenu = sub;
    }
    if (type & MF_SEPARATOR)
      info.fMask &= ~MIIM_STRING;
    if (!InsertMenuItemA(h, (UINT)-1, 1, &info))
      return 0;
    if (flags & MF_END)
      return 1;
  }
  return 0;
}
HMENU W32API w32_load_menu(const void *data, DWORD size) {
  U32_GUARD;
  struct reader r = {data, size, 0, 0};
  if (!data || size < 4)
    return (HMENU)(uintptr_t)u32_fail(87);
  UINT version = r16(&r), offset = r16(&r);
  HMENU h = CreateMenu();
  if (!h)
    return 0;
  BOOL ok;
  if (version == 0 && offset == 0)
    ok = parse_items(&r, h, 0);
  else if (version == 1) {
    r.at = 4 + offset;
    ok = r.at <= size && parse_ex(&r, h, 0);
  } else
    ok = 0;
  if (!ok) {
    DestroyMenu(h);
    u32_fail(1402);
    return 0;
  }
  return h;
}
HMENU W32API LoadMenuIndirectA(const void *data) {
  return w32_load_menu(data, 1u << 20);
}
HMENU W32API LoadMenuA(HINSTANCE instance, const char *name) {
  U32_GUARD;
  DWORD size = 0;
  const void *data = u32_resource(instance, name, 4, &size);
  if (!data)
    return (HMENU)(uintptr_t)u32_fail(1814);
  return w32_load_menu(data, size);
}

/* ---- metrics and the menu font --------------------------------------------- */
static HFONT menu_font(BOOL bold) {
  static HFONT fonts[2];
  const struct u32_graphics *g = u32_graphics();
  if (!fonts[bold] && g && g->font_indirect) {
    struct u32_logfont lf = {0};
    lf.height = -11;
    lf.weight = bold ? 700 : 400;
    lf.charset = 1;
    u32_strcpy(lf.face, sizeof(lf.face), "MS Sans Serif");
    fonts[bold] = (HFONT)(uintptr_t)g->font_indirect(&lf);
  }
  return fonts[bold];
}
static int label_width(const char *s, const struct u32_font_info *f) {
  int w = 0;
  for (; s && *s && *s != '\t'; s++) {
    if (*s == '&') {
      if (s[1] != '&')
        continue;
      s++;
    }
    w += f->widths[(uint8_t)*s];
  }
  return w;
}
static const char *accel_text(const char *s) {
  for (; s && *s; s++)
    if (*s == '\t')
      return s + 1;
  return 0;
}
static int item_height(void) {
  int h = u32_font_info(menu_font(0))->height + 5;
  return h < 16 ? 16 : h;
}
/* Menu bar rows inside the client width. */
static int bar_layout(struct menu *m, int width) {
  const struct u32_font_info *f = u32_font_info(menu_font(0));
  int row = GetSystemMetrics(15), x = 0, y = 0;
  if (width <= 0)
    width = 1;
  for (UINT i = 0; i < m->count; i++) {
    struct mitem *it = &m->items[i];
    int w = it->type & MF_SEPARATOR ? 6 : label_width(it->text, f) + 16;
    if (i && ((it->type & (MF_MENUBREAK | MF_MENUBARBREAK)) ||
              (x + w > width && x > 0))) {
      x = 0;
      y += row;
    }
    it->rect = (RECT){x, y, x + w, y + row};
    x += w;
  }
  /* MF_RIGHTJUSTIFY moves it and the following items of its row right. */
  for (UINT i = 0; i < m->count; i++)
    if (m->items[i].type & MF_RIGHTJUSTIFY) {
      int rowy = m->items[i].rect.top, end = 0;
      UINT j = i;
      while (j < m->count && m->items[j].rect.top == rowy)
        end = m->items[j++].rect.right;
      int shift = width - end;
      for (UINT k = i; k < j && shift > 0; k++)
        OffsetRect(&m->items[k].rect, shift, 0);
      break;
    }
  return m->count ? y + row : row;
}
int u32_menu_height(struct u32_window *w) {
  struct menu *m = w ? menu(w->menu) : 0;
  if (!m || (w->style & WS_CHILD))
    return 0;
  int bx, by;
  u32_frame(w->style, w->exstyle, &bx, &by);
  return bar_layout(m, w->rect.right - w->rect.left - 2 * bx);
}
BOOL W32API SetMenu(HWND h, HMENU hm) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || (w->style & WS_CHILD))
    return u32_fail(1400);
  struct menu *m = menu(hm);
  if (hm && !m)
    return u32_fail(1401);
  struct menu *old = menu(w->menu);
  if (old && old->owner == h)
    old->owner = 0;
  w->menu = hm;
  if (m)
    m->owner = h;
  u32_resize(w);
  RECT r;
  GetClientRect(h, &r);
  SendMessageA(h, WM_SIZE, 0, (LPARAM)((r.right & 0xffff) | (r.bottom << 16)));
  InvalidateRect(h, 0, 1);
  DrawMenuBar(h);
  return 1;
}
HMENU W32API GetMenu(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  return w && !(w->style & WS_CHILD) ? w->menu : 0;
}

/* ---- tracking state ------------------------------------------------------------ */
struct level {
  HMENU menu;
  HWND window;
  int selected;
  BOOL open;
};
static struct {
  BOOL active, bar, system, done, keyboard;
  HWND owner;
  struct level levels[DEPTH];
  UINT depth, flags, result;
  POINT anchor;
} T;
static void menu_bar_paint(struct u32_window *w);
void u32_menu_paint(struct u32_window *w) { menu_bar_paint(w); }
static void menu_bar_paint(struct u32_window *w) {
  struct menu *m = menu(w->menu);
  int top = u32_menu_height(w);
  if (!m || !top)
    return;
  UINT width, height;
  u32_surface_size(w, &width, &height);
  struct u32_window *root;
  HDC dc = u32_band_dc(w, (RECT){0, 0, (LONG)width, top}, &root);
  if (!dc)
    return;
  u32_color_rect(dc, (RECT){0, 0, (LONG)width, top}, GetSysColor(COLOR_MENU));
  bar_layout(m, (int)width);
  HFONT font = menu_font(0);
  for (UINT i = 0; i < m->count; i++) {
    struct mitem *it = &m->items[i];
    if (it->type & MF_SEPARATOR)
      continue;
    RECT r = it->rect;
    BOOL selected = T.active && T.bar && T.owner == w->handle &&
                    T.levels[0].selected == (int)i;
    if (selected)
      DrawEdge(dc, &r, T.depth > 1 ? BDR_SUNKENOUTER : BDR_RAISEDINNER,
               BF_RECT);
    RECT text = r;
    if (selected && T.depth > 1)
      OffsetRect(&text, 1, 1);
    u32_mnemonic_text(dc, text, it->text ? it->text : "", font,
                      GetSysColor(COLOR_MENUTEXT), 0x25u,
                      !!(it->state & MF_GRAYED));
  }
  u32_band_release(root, dc);
}
BOOL W32API DrawMenuBar(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (!(w->style & WS_CHILD) && w->menu) {
    UINT width, height;
    u32_surface_size(w, &width, &height);
    if (w->surface.width != width || w->surface.height != height)
      u32_resize(w);
    menu_bar_paint(w);
  }
  return 1;
}
/* Popup layout: item rectangles relative to the popup window. */
static void popup_layout(struct menu *m, HWND owner) {
  const struct u32_font_info *f = u32_font_info(menu_font(0));
  int text = 0, accel = 0, y = 3, h = item_height();
  for (UINT i = 0; i < m->count; i++) {
    struct mitem *it = &m->items[i];
    int ih = it->type & MF_SEPARATOR ? 9 : h;
    if (it->type & MF_OWNERDRAW) {
      MEASUREITEMSTRUCT mi = {1, 0, it->id, 0, (UINT)h, it->data};
      if (owner)
        SendMessageA(owner, WM_MEASUREITEM, 0, (LPARAM)&mi);
      ih = mi.itemHeight ? (int)mi.itemHeight : h;
      if ((int)mi.itemWidth > text)
        text = (int)mi.itemWidth;
    } else if (!(it->type & MF_SEPARATOR)) {
      int w = label_width(it->text, f);
      if (w > text)
        text = w;
      const char *a = accel_text(it->text);
      int aw = a ? label_width(a, f) : 0;
      if (aw > accel)
        accel = aw;
    }
    it->rect = (RECT){3, y, 0, y + ih};
    y += ih;
  }
  int width = 3 + 19 + text + (accel ? 16 + accel : 0) + 19 + 3;
  if (width < 60)
    width = 60;
  for (UINT i = 0; i < m->count; i++)
    m->items[i].rect.right = width - 3;
  m->width = width;
  m->height = y + 3;
}
static void popup_paint(HWND h, HDC dc, struct menu *m, int selected) {
  RECT client;
  GetClientRect(h, &client);
  u32_color_rect(dc, client, GetSysColor(COLOR_MENU));
  RECT edge = client;
  DrawEdge(dc, &edge, EDGE_RAISED, BF_RECT);
  const struct u32_font_info *f = u32_font_info(menu_font(0));
  for (UINT i = 0; i < m->count; i++) {
    struct mitem *it = &m->items[i];
    RECT r = it->rect;
    if (it->type & MF_SEPARATOR) {
      RECT line = {r.left + 1, (r.top + r.bottom) / 2 - 1, r.right - 1,
                   (r.top + r.bottom) / 2 + 1};
      DrawEdge(dc, &line, EDGE_ETCHED, BF_TOP);
      continue;
    }
    BOOL hot = (int)i == selected;
    BOOL grayed = !!(it->state & MF_GRAYED);
    if (it->type & MF_OWNERDRAW) {
      DRAWITEMSTRUCT d = {1,
                          0,
                          it->id,
                          1,
                          (hot ? 1u : 0u) | (grayed ? 6u : 0u) |
                              (it->state & MF_CHECKED ? 8u : 0u),
                          (HWND)(uintptr_t)m,
                          dc,
                          r,
                          it->data};
      if (T.owner)
        SendMessageA(T.owner, WM_DRAWITEM, 0, (LPARAM)&d);
      continue;
    }
    if (hot)
      u32_color_rect(dc, r, GetSysColor(COLOR_HIGHLIGHT));
    DWORD ink = GetSysColor(hot ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
    if (it->state & MF_CHECKED) {
      RECT mark = {r.left, r.top, r.left + 19, r.bottom};
      const struct u32_graphics *g = u32_graphics();
      (void)g;
      int cx = (mark.left + mark.right) / 2, cy = (mark.top + mark.bottom) / 2;
      DWORD color = grayed ? GetSysColor(COLOR_GRAYTEXT) : ink;
      if (it->type & MFT_RADIOCHECK) {
        u32_color_rect(dc, (RECT){cx - 2, cy - 1, cx + 2, cy + 2}, color);
        u32_color_rect(dc, (RECT){cx - 1, cy - 2, cx + 1, cy + 3}, color);
      } else {
        static const int rise[7] = {2, 3, 4, 3, 2, 1, 0};
        for (int k = 0; k < 7; k++)
          u32_color_rect(dc,
                         (RECT){cx - 3 + k, cy - 1 + rise[k] - 2,
                                cx - 2 + k, cy - 1 + rise[k] + 1},
                         color);
      }
    }
    HFONT font = menu_font(!!(it->state & MF_DEFAULT));
    RECT label = {r.left + 19, r.top, r.right - 19, r.bottom};
    char text[256];
    u32_strcpy(text, sizeof(text), it->text ? it->text : "");
    for (UINT k = 0; text[k]; k++)
      if (text[k] == '\t')
        text[k] = 0;
    if (grayed && !hot)
      u32_mnemonic_text(dc, label, text, font, ink, 0x24u, 1);
    else
      u32_label(dc, label, text, font,
                grayed ? GetSysColor(COLOR_GRAYTEXT) : ink, 0x24u);
    const char *a = accel_text(it->text);
    if (a) {
      RECT right = {label.left, r.top, r.right - 19, r.bottom};
      if (grayed && !hot)
        u32_mnemonic_text(dc, right, a, font, ink, 0x26u, 1);
      else
        u32_label(dc, right, a, font,
                  grayed ? GetSysColor(COLOR_GRAYTEXT) : ink, 0x26u);
    }
    if (it->sub) {
      RECT arrow = {r.right - 16, r.top, r.right - 4, r.bottom};
      int cx = (arrow.left + arrow.right) / 2, cy = (arrow.top + arrow.bottom) / 2;
      for (int k = 0; k < 4; k++)
        u32_color_rect(dc, (RECT){cx - 2 + k, cy - 3 + k, cx - 1 + k, cy + 4 - k},
                       ink);
    }
    (void)f;
  }
}
static int level_of(HWND h) {
  for (UINT i = 0; i < T.depth; i++)
    if (T.levels[i].window == h)
      return (int)i;
  return -1;
}
LRESULT W32API u32_menu_window_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  if (m == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    int at = level_of(h);
    struct menu *mm = at >= 0 ? menu(T.levels[at].menu) : 0;
    if (dc && mm)
      popup_paint(h, dc, mm, T.levels[at].selected);
    if (dc)
      EndPaint(h, &ps);
    return 0;
  }
  if (m == WM_ERASEBKGND)
    return 1;
  if (m == WM_GETDLGCODE)
    return DLGC_WANTALLKEYS;
  return DefWindowProcA(h, m, wp, lp);
}

/* ---- tracking engine -------------------------------------------------------- */
static BOOL selectable(struct mitem *it) {
  return !(it->type & MF_SEPARATOR);
}
static void select_item(UINT at, int index) {
  struct level *l = &T.levels[at];
  if (l->selected == index)
    return;
  l->selected = index;
  struct menu *m = menu(l->menu);
  if (!(T.flags & TPM_NONOTIFY) && m && T.owner) {
    if (index >= 0 && (UINT)index < m->count) {
      struct mitem *it = &m->items[index];
      UINT flags = (it->state & 0xffffu) | (it->sub ? MF_POPUP : 0) |
                   MF_HILITE | (T.system ? 0x2000u : 0);
      SendMessageA(T.owner, WM_MENUSELECT,
                   (it->sub ? (UINT)index : it->id) | (flags << 16),
                   (LPARAM)l->menu);
    }
  }
  if (l->window)
    InvalidateRect(l->window, 0, 0);
  else if (T.bar) {
    struct u32_window *w = u32_window(T.owner);
    if (w)
      menu_bar_paint(w);
  }
}
static void close_from(UINT at) {
  while (T.depth > at) {
    struct level *l = &T.levels[T.depth - 1];
    if (l->window && IsWindow(l->window))
      DestroyWindow(l->window);
    if (!(T.flags & TPM_NONOTIFY) && T.owner && l->window)
      SendMessageA(T.owner, WM_UNINITMENUPOPUP, (WPARAM)l->menu, 0);
    *l = (struct level){0, 0, -1, 0};
    T.depth--;
  }
  if (T.depth)
    T.levels[T.depth - 1].open = 0;
  if (T.bar) {
    struct u32_window *w = u32_window(T.owner);
    if (w)
      menu_bar_paint(w);
  }
}
static BOOL open_popup(HMENU hm, POINT p, RECT exclude, BOOL horizontal_parent,
                       UINT index) {
  struct menu *m = menu(hm);
  if (!m || T.depth >= DEPTH)
    return 0;
  if (!(T.flags & TPM_NONOTIFY) && T.owner)
    SendMessageA(T.owner, WM_INITMENUPOPUP, (WPARAM)hm,
                 (LPARAM)(index | (T.system ? 0x10000u : 0)));
  if (!(m = menu(hm)) || !m->count)
    return 0;
  popup_layout(m, T.owner);
  int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
  int x = p.x, y = p.y;
  if (!horizontal_parent && T.depth == 0) {
    if (T.flags & TPM_RIGHTALIGN)
      x -= m->width;
    else if (T.flags & TPM_CENTERALIGN)
      x -= m->width / 2;
    if (T.flags & TPM_BOTTOMALIGN)
      y -= m->height;
    else if (T.flags & TPM_VCENTERALIGN)
      y -= m->height / 2;
  }
  if (x + m->width > sw)
    x = horizontal_parent ? sw - m->width
        : exclude.left > m->width ? exclude.left - m->width + 3
                                  : sw - m->width;
  if (y + m->height > sh)
    y = horizontal_parent && exclude.top - m->height >= 0
            ? exclude.top - m->height
            : sh - m->height;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  HWND window = CreateWindowExA(WS_EX_TOPMOST, U32_MENU_CLASS, "", WS_POPUP, x,
                                y, m->width, m->height, T.owner, 0, 0, 0);
  if (!window)
    return 0;
  struct level *l = &T.levels[T.depth++];
  *l = (struct level){hm, window, -1, 0};
  if (T.depth > 1)
    T.levels[T.depth - 2].open = 1;
  ShowWindow(window, 8);
  UpdateWindow(window);
  return 1;
}
static RECT screen_item(UINT at, int index) {
  struct level *l = &T.levels[at];
  struct menu *m = menu(l->menu);
  RECT r = m && index >= 0 && (UINT)index < m->count ? m->items[index].rect
                                                      : (RECT){0, 0, 0, 0};
  POINT o;
  if (l->window) {
    o = u32_origin(u32_window(l->window));
  } else {
    struct u32_window *w = u32_window(T.owner);
    o = w ? u32_surface_origin(w) : (POINT){0, 0};
  }
  OffsetRect(&r, o.x, o.y);
  return r;
}
static void open_selected(UINT at, BOOL select_first) {
  struct level *l = &T.levels[at];
  struct menu *m = menu(l->menu);
  if (!m || l->selected < 0 || (UINT)l->selected >= m->count)
    return;
  struct mitem *it = &m->items[l->selected];
  if (!it->sub || (it->state & MF_GRAYED))
    return;
  close_from(at + 1);
  RECT r = screen_item(at, l->selected);
  POINT p = l->window ? (POINT){r.right - 3, r.top - 3}
                      : (POINT){r.left, r.bottom};
  if (open_popup(it->sub, p, r, !l->window, (UINT)l->selected) && select_first) {
    struct menu *s = menu(it->sub);
    for (UINT i = 0; s && i < s->count; i++)
      if (selectable(&s->items[i])) {
        select_item(T.depth - 1, (int)i);
        break;
      }
  }
}
static void choose(UINT at) {
  struct level *l = &T.levels[at];
  struct menu *m = menu(l->menu);
  if (!m || l->selected < 0 || (UINT)l->selected >= m->count)
    return;
  struct mitem *it = &m->items[l->selected];
  if (it->sub) {
    open_selected(at, 1);
    return;
  }
  if (it->state & (MF_GRAYED | MF_DISABLED) || (it->type & MF_SEPARATOR)) {
    if (it->type & MF_SEPARATOR)
      return;
    T.done = 1;
    T.result = 0;
    return;
  }
  T.result = it->id;
  T.done = 1;
}
static void move(UINT at, int step) {
  struct level *l = &T.levels[at];
  struct menu *m = menu(l->menu);
  if (!m || !m->count)
    return;
  int i = l->selected;
  for (UINT k = 0; k < m->count; k++) {
    i = i < 0 ? (step > 0 ? 0 : (int)m->count - 1)
              : (i + step + (int)m->count) % (int)m->count;
    if (selectable(&m->items[i])) {
      select_item(at, i);
      return;
    }
  }
}
static int hit(UINT at, POINT screen) {
  struct level *l = &T.levels[at];
  struct menu *m = menu(l->menu);
  for (UINT i = 0; m && i < m->count; i++) {
    RECT r = screen_item(at, (int)i);
    if (PtInRect(&r, screen))
      return (int)i;
  }
  return -1;
}
static int find_mnemonic(struct menu *m, UINT ch) {
  if (ch >= 'a' && ch <= 'z')
    ch -= 32;
  for (UINT i = 0; m && i < m->count; i++)
    if (selectable(&m->items[i]) && u32_mnemonic(m->items[i].text) == ch)
      return (int)i;
  return -1;
}
static void key(UINT vk) {
  UINT top = T.depth - 1;
  struct level *l = &T.levels[top];
  BOOL bar_level = T.bar && top == 0;
  switch (vk) {
  case VK_ESCAPE:
    if (top == 0 || (T.bar && top == 1)) {
      if (T.bar && top == 1 && T.keyboard) {
        close_from(1);
        return;
      }
      T.done = 1;
      T.result = 0;
    } else
      close_from(top);
    return;
  case VK_UP:
  case VK_DOWN:
    if (bar_level) {
      open_selected(0, 1);
      return;
    }
    move(top, vk == VK_UP ? -1 : 1);
    return;
  case VK_LEFT:
  case VK_RIGHT:
    if (bar_level) {
      move(0, vk == VK_LEFT ? -1 : 1);
      return;
    }
    if (vk == VK_RIGHT) {
      struct menu *m = menu(l->menu);
      if (m && l->selected >= 0 && m->items[l->selected].sub) {
        open_selected(top, 1);
        return;
      }
    } else if (top > (T.bar ? 1u : 0u)) {
      close_from(top);
      return;
    }
    if (T.bar) {
      close_from(1);
      move(0, vk == VK_LEFT ? -1 : 1);
      open_selected(0, 1);
    }
    return;
  case VK_RETURN:
    if (bar_level)
      open_selected(0, 1);
    else
      choose(top);
    return;
  default:
    return;
  }
}
static void character(UINT ch) {
  UINT top = T.depth - 1;
  struct level *l = &T.levels[top];
  int i = find_mnemonic(menu(l->menu), ch);
  if (i < 0) {
    MessageBeep(0);
    return;
  }
  select_item(top, i);
  if (T.bar && top == 0)
    open_selected(0, 1);
  struct menu *m = menu(l->menu);
  if (m && !(T.bar && top == 0) && !m->items[i].sub)
    choose(top);
  else if (m && m->items[i].sub && !(T.bar && top == 0))
    open_selected(top, 1);
}
static void mouse(UINT message, POINT screen) {
  for (int at = (int)T.depth - 1; at >= 0; at--) {
    int i = hit((UINT)at, screen);
    if (i < 0)
      continue;
    close_from((UINT)at + 1);
    if (T.levels[at].selected != i)
      select_item((UINT)at, i);
    struct menu *m = menu(T.levels[at].menu);
    struct mitem *it = m ? &m->items[i] : 0;
    if (!it)
      return;
    if (it->sub) {
      if (message == WM_LBUTTONDOWN || message == WM_NCLBUTTONDOWN ||
          T.levels[at].window)
        open_selected((UINT)at, 0);
      return;
    }
    if (message == WM_LBUTTONUP || message == WM_NCLBUTTONUP ||
        ((T.flags & TPM_RIGHTBUTTON) && message == WM_RBUTTONUP))
      choose((UINT)at);
    return;
  }
  if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
      message == WM_NCLBUTTONDOWN || message == WM_NCRBUTTONDOWN) {
    T.done = 1;
    T.result = 0;
  }
}
static UINT run(void) {
  if (T.owner && !(T.flags & TPM_NONOTIFY)) {
    SendMessageA(T.owner, WM_ENTERMENULOOP, !T.bar, 0);
    SendMessageA(T.owner, WM_INITMENU, (WPARAM)T.levels[0].menu, 0);
  }
  HWND capture = T.owner;
  if (capture)
    SetCapture(capture);
  MSG msg;
  for (UINT guard = 0; !T.done && guard < 1000000; guard++) {
    if (!T.depth || !IsWindow(T.owner))
      break;
    if (!PeekMessageA(&msg, 0, 0, 0, 0) && T.owner) {
      HWND popup = T.levels[T.depth - 1].window;
      SendMessageA(T.owner, WM_ENTERIDLE, MSGF_MENU, (LPARAM)popup);
      if (T.done)
        break;
    }
    BOOL got = GetMessageA(&msg, 0, 0, 0);
    if ((int)got <= 0) {
      if (!got)
        PostQuitMessage((int)msg.wParam);
      break;
    }
    UINT m = msg.message;
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
      if (msg.wParam == VK_MENU || msg.wParam == VK_F10) {
        T.done = 1;
        T.result = 0;
      } else
        key((UINT)msg.wParam);
      continue;
    }
    if (m == WM_KEYUP || m == WM_SYSKEYUP)
      continue;
    if (m == WM_CHAR || m == WM_SYSCHAR) {
      character((UINT)msg.wParam);
      continue;
    }
    if ((m >= WM_MOUSEMOVE && m <= 0x20d) ||
        (m >= WM_NCMOUSEMOVE && m <= 0xad)) {
      if (m == WM_MOUSEMOVE || m == WM_NCMOUSEMOVE) {
        for (int at = (int)T.depth - 1; at >= 0; at--) {
          int i = hit((UINT)at, msg.pt);
          if (i >= 0) {
            if (T.levels[at].window || (T.bar && T.depth > 1)) {
              if (T.levels[at].selected != i) {
                close_from((UINT)at + 1);
                select_item((UINT)at, i);
                struct menu *mm = menu(T.levels[at].menu);
                if (mm && mm->items[i].sub && (!T.levels[at].window || 1))
                  open_selected((UINT)at, 0);
              }
            }
            break;
          }
        }
        continue;
      }
      if (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_RBUTTONDOWN ||
          m == WM_RBUTTONUP || m == WM_NCLBUTTONDOWN || m == WM_NCLBUTTONUP ||
          m == WM_NCRBUTTONDOWN || m == WM_NCRBUTTONUP)
        mouse(m, msg.pt);
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  if (GetCapture() == capture)
    ReleaseCapture();
  close_from(0);
  UINT result = T.done ? T.result : 0;
  HWND owner = T.owner;
  BOOL notify = !(T.flags & TPM_NONOTIFY);
  if (owner && notify && IsWindow(owner)) {
    SendMessageA(owner, WM_MENUSELECT, 0xffff0000u, 0);
    SendMessageA(owner, WM_EXITMENULOOP, !T.bar, 0);
  }
  return result;
}
static BOOL begin(HWND owner, UINT flags) {
  if (T.active)
    return 0;
  w32_fill(&T, 0, sizeof(T));
  T.active = 1;
  T.owner = owner;
  T.flags = flags;
  for (UINT i = 0; i < DEPTH; i++)
    T.levels[i].selected = -1;
  return 1;
}
static void end(void) {
  struct u32_window *w = T.bar ? u32_window(T.owner) : 0;
  T.active = 0;
  T.depth = 0;
  if (w)
    menu_bar_paint(w);
}
BOOL W32API TrackPopupMenuEx(HMENU hm, UINT flags, int x, int y, HWND owner,
                             const TPMPARAMS *params) {
  U32_GUARD;
  if (!menu(hm) || !IsWindow(owner))
    return u32_fail(1401);
  if (!begin(owner, flags))
    return u32_fail(1403);
  RECT exclude = params ? params->rcExclude : (RECT){x, y, x, y};
  UINT result = 0;
  if (open_popup(hm, (POINT){x, y}, exclude, 0, 0))
    result = run();
  BOOL system = T.system;
  end();
  if (flags & TPM_RETURNCMD)
    return (BOOL)result;
  if (result)
    PostMessageA(owner, system ? WM_SYSCOMMAND : WM_COMMAND, result, 0);
  return 1;
}
BOOL W32API TrackPopupMenu(HMENU hm, UINT flags, int x, int y, int reserved,
                           HWND owner, const RECT *rect) {
  (void)reserved, (void)rect;
  return TrackPopupMenuEx(hm, flags, x, y, owner, 0);
}
BOOL W32API EndMenu(void) {
  U32_GUARD;
  if (T.active) {
    T.done = 1;
    T.result = 0;
    PostMessageA(T.owner, 0, 0, 0);
  }
  return 1;
}
/* Menu bar tracking from SC_KEYMENU (keyboard) or SC_MOUSEMENU. */
static void track_bar(HWND h, int initial, BOOL keyboard, UINT ch) {
  struct u32_window *w = u32_window(h);
  if (!w || !menu(w->menu) || !begin(h, 0))
    return;
  T.bar = 1;
  T.keyboard = keyboard;
  T.levels[0] = (struct level){w->menu, 0, -1, 0};
  T.depth = 1;
  struct menu *m = menu(w->menu);
  if (ch) {
    initial = find_mnemonic(m, ch);
    if (initial < 0) {
      end();
      MessageBeep(0);
      return;
    }
  }
  if (initial < 0)
    for (UINT i = 0; i < m->count; i++)
      if (selectable(&m->items[i])) {
        initial = (int)i;
        break;
      }
  if (T.owner)
    select_item(0, initial);
  if (!keyboard || ch) {
    if (ch && !m->items[initial].sub) {
      T.result = m->items[initial].id;
      T.done = 1;
    } else
      open_selected(0, ch != 0);
  }
  UINT result = T.done ? T.result : run();
  end();
  if (result)
    PostMessageA(h, WM_COMMAND, result, 0);
}
/* ---- system menu -------------------------------------------------------- */
HMENU W32API GetSystemMenu(HWND h, BOOL revert) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return (HMENU)(uintptr_t)u32_fail(1400);
  if (revert) {
    if (w->system_menu)
      DestroyMenu(w->system_menu);
    w->system_menu = 0;
    return 0;
  }
  if (!w->system_menu) {
    HMENU s = CreatePopupMenu();
    if (!s)
      return 0;
    struct menu *m = menu(s);
    m->system = 1;
    AppendMenuA(s, MF_STRING, SC_RESTORE, "&Restore");
    AppendMenuA(s, MF_STRING, SC_MOVE, "&Move");
    AppendMenuA(s, MF_STRING, SC_SIZE, "&Size");
    AppendMenuA(s, MF_STRING, SC_MINIMIZE, "Mi&nimize");
    AppendMenuA(s, MF_STRING, SC_MAXIMIZE, "Ma&ximize");
    AppendMenuA(s, MF_SEPARATOR, 0, 0);
    AppendMenuA(s, MF_STRING | MF_DEFAULT, SC_CLOSE, "&Close\tAlt+F4");
    w->system_menu = s;
  }
  return w->system_menu;
}
static void track_system(HWND h) {
  struct u32_window *w = u32_window(h);
  if (!w || (w->style & WS_CHILD) || !(w->style & 0x80000u))
    return;
  HMENU s = GetSystemMenu(h, 0);
  if (!s)
    return;
  BOOL minimized = !!(w->style & WS_MINIMIZE),
       maximized = !!(w->style & WS_MAXIMIZE);
  BOOL sizable = !!(w->style & WS_THICKFRAME);
  EnableMenuItem(s, SC_RESTORE, minimized || maximized ? 0 : MF_GRAYED);
  EnableMenuItem(s, SC_MOVE, MF_GRAYED);
  EnableMenuItem(s, SC_SIZE, MF_GRAYED);
  EnableMenuItem(s, SC_MINIMIZE,
                 !minimized && (w->style & 0x20000u) ? 0 : MF_GRAYED);
  EnableMenuItem(s, SC_MAXIMIZE,
                 !maximized && sizable && (w->style & 0x10000u) ? 0 : MF_GRAYED);
  POINT o = u32_surface_origin(w);
  int bx, by;
  u32_frame(w->style, w->exstyle, &bx, &by);
  if (!begin(h, 0))
    return;
  T.system = 1;
  UINT result = 0;
  if (open_popup(s, (POINT){o.x - bx, o.y}, (RECT){0, 0, 0, 0}, 0, 0)) {
    move(0, 1);
    result = run();
  }
  end();
  if (result)
    PostMessageA(h, WM_SYSCOMMAND, result, 0);
}
HWND u32_menu_owner_tracking(void) { return T.active ? T.owner : 0; }
/* DefWindowProc hooks: Alt/F10, Alt+mnemonic, Alt+Space, menu bar clicks. */
LRESULT u32_menu_command(HWND h, WPARAM wp, LPARAM lp) {
  UINT command = (UINT)wp & 0xfff0u;
  if (command == SC_KEYMENU) {
    struct u32_window *w = u32_window(h);
    while (w && (w->style & WS_CHILD))
      w = u32_window(w->parent);
    if (!w)
      return 0;
    if (lp == ' ')
      track_system(w->handle);
    else if (w->menu)
      track_bar(w->handle, -1, 1, (UINT)lp);
    return 0;
  }
  if (command == SC_MOUSEMENU) {
    struct u32_window *w = u32_window(h);
    if (!w || !w->menu)
      return 0;
    POINT p = {(int16_t)lp, (int16_t)(lp >> 16)};
    if (!begin(h, 0))
      return 0;
    T.bar = 1;
    T.levels[0] = (struct level){w->menu, 0, -1, 0};
    T.depth = 1;
    int i = hit(0, p);
    end();
    if (i >= 0)
      track_bar(h, i, 0, 0);
    return 0;
  }
  return 0;
}
LRESULT u32_menu_key(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)lp;
  static HWND alt_window;
  if (m == WM_SYSKEYDOWN) {
    if (wp == VK_MENU) {
      alt_window = h;
      return 0;
    }
    alt_window = 0;
    if (wp == VK_F10)
      SendMessageA(h, WM_SYSCOMMAND, SC_KEYMENU, 0);
    else if (wp == VK_F4 && GetKeyState(VK_MENU) < 0) {
      struct u32_window *w = u32_window(h);
      while (w && (w->style & WS_CHILD))
        w = u32_window(w->parent);
      if (w)
        PostMessageA(w->handle, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
    return 0;
  }
  if (m == WM_SYSKEYUP) {
    if (wp == VK_MENU && alt_window == h)
      SendMessageA(h, WM_SYSCOMMAND, SC_KEYMENU, 0);
    alt_window = 0;
    return 0;
  }
  if (m == WM_SYSCHAR) {
    alt_window = 0;
    if (wp == VK_RETURN)
      return 0;
    SendMessageA(h, WM_SYSCOMMAND, SC_KEYMENU, (LPARAM)wp);
    return 0;
  }
  return 0;
}
void u32_menu_destroy_window(HWND h) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return;
  if (w->system_menu)
    DestroyMenu(w->system_menu);
  w->system_menu = 0;
  struct menu *m = menu(w->menu);
  /* Window menus are destroyed with their window (Win32 DestroyWindow). */
  if (m && !(w->style & WS_CHILD))
    DestroyMenu(w->menu);
  w->menu = 0;
  if (T.active && T.owner == h) {
    T.done = 1;
    T.result = 0;
  }
}

/* ---- accelerators ---------------------------------------------------------- */
struct accel_table {
  BOOL used;
  UINT generation, count;
  ACCEL *entries;
};
static struct accel_table accels[ACCELS];
static struct accel_table *accel(HACCEL h) {
  uintptr_t v = (uintptr_t)h;
  UINT i = (UINT)(v & 0xff);
  if ((v & 0xf0000000u) != 0x30000000u || i >= ACCELS || !accels[i].used ||
      ((v >> 8) & 0xfffffu) != accels[i].generation)
    return 0;
  return &accels[i];
}
static HACCEL new_table(const ACCEL *entries, UINT n) {
  if (!n || n > 4096)
    return (HACCEL)(uintptr_t)u32_fail(87);
  for (UINT i = 0; i < ACCELS; i++)
    if (!accels[i].used) {
      ACCEL *copy = u32_alloc(n * sizeof(ACCEL));
      if (!copy)
        return (HACCEL)(uintptr_t)u32_fail(8);
      w32_copy(copy, entries, n * sizeof(ACCEL));
      UINT generation = ((accels[i].generation + 1) & 0xfffffu) ? (accels[i].generation + 1) & 0xfffffu : 1;
      accels[i] = (struct accel_table){1, generation, n, copy};
      return (HACCEL)(uintptr_t)(0x30000000u | (generation << 8) | i);
    }
  return (HACCEL)(uintptr_t)u32_fail(8);
}
HACCEL W32API CreateAcceleratorTableA(ACCEL *entries, int n) {
  U32_GUARD;
  if (!entries || n <= 0)
    return (HACCEL)(uintptr_t)u32_fail(87);
  return new_table(entries, (UINT)n);
}
HACCEL W32API LoadAcceleratorsA(HINSTANCE instance, const char *name) {
  U32_GUARD;
  DWORD size = 0;
  const uint8_t *data = u32_resource(instance, name, 9, &size);
  if (!data || size < 8)
    return (HACCEL)(uintptr_t)u32_fail(1814);
  UINT n = 0;
  ACCEL entries[512];
  for (DWORD at = 0; at + 8 <= size && n < 512; at += 8) {
    UINT flags = w32_u16(data + at);
    entries[n].fVirt = (uint8_t)(flags & 0x7f);
    entries[n].key = (uint16_t)w32_u16(data + at + 2);
    entries[n].cmd = (uint16_t)w32_u16(data + at + 4);
    n++;
    if (flags & 0x80)
      break;
  }
  return new_table(entries, n);
}
int W32API CopyAcceleratorTableA(HACCEL h, ACCEL *out, int n) {
  U32_GUARD;
  struct accel_table *t = accel(h);
  if (!t)
    return u32_fail(1403);
  if (!out)
    return (int)t->count;
  UINT k = (UINT)n < t->count ? (UINT)n : t->count;
  w32_copy(out, t->entries, k * sizeof(ACCEL));
  return (int)k;
}
BOOL W32API DestroyAcceleratorTable(HACCEL h) {
  U32_GUARD;
  struct accel_table *t = accel(h);
  if (!t)
    return u32_fail(1403);
  u32_free(t->entries);
  UINT generation = t->generation;
  *t = (struct accel_table){0, generation, 0, 0};
  return 1;
}
int W32API TranslateAcceleratorA(HWND h, HACCEL table, MSG *msg) {
  U32_GUARD;
  struct accel_table *t = accel(table);
  if (!t || !msg || !IsWindow(h))
    return u32_fail(1403);
  UINT m = msg->message;
  if (m != WM_KEYDOWN && m != WM_SYSKEYDOWN && m != WM_CHAR && m != WM_SYSCHAR)
    return 0;
  BOOL shift = GetKeyState(VK_SHIFT) < 0, control = GetKeyState(VK_CONTROL) < 0,
       alt = GetKeyState(VK_MENU) < 0 || m == WM_SYSKEYDOWN || m == WM_SYSCHAR;
  for (UINT i = 0; i < t->count; i++) {
    ACCEL a = t->entries[i];
    BOOL virt = !!(a.fVirt & FVIRTKEY);
    if (virt) {
      if ((m != WM_KEYDOWN && m != WM_SYSKEYDOWN) || a.key != (msg->wParam & 0xffff))
        continue;
      if (!!(a.fVirt & FSHIFT) != shift || !!(a.fVirt & FCONTROL) != control ||
          !!(a.fVirt & FALT) != alt)
        continue;
    } else {
      if ((m != WM_CHAR && m != WM_SYSCHAR) || a.key != (msg->wParam & 0xffff))
        continue;
      if (!!(a.fVirt & FALT) != alt)
        continue;
    }
    struct u32_window *w = u32_window(h);
    HMENU bar = w && !(w->style & WS_CHILD) ? w->menu : 0;
    struct mitem *it = bar ? find(bar, a.cmd, 0, 0, 0) : 0;
    struct mitem *sys = !it && w && w->system_menu
                            ? find(w->system_menu, a.cmd, 0, 0, 0)
                            : 0;
    if ((it && (it->state & (MF_GRAYED | MF_DISABLED))) ||
        (sys && (sys->state & (MF_GRAYED | MF_DISABLED))) ||
        (w && (w->style & WS_DISABLED)))
      return 1;
    if (bar && !(a.fVirt & FNOINVERT))
      SendMessageA(h, WM_INITMENU, (WPARAM)bar, 0);
    SendMessageA(h, sys ? WM_SYSCOMMAND : WM_COMMAND, a.cmd | (1u << 16), 0);
    return 1;
  }
  return 0;
}

/* ---- f5-20B: MENUINFO and item rectangles ------------------------------ */
/* The MENUINFO fields are kept and returned; menus keep drawing with the
 * classic COLOR_MENU look and no scrolling (MIM_BACKGROUND, MIM_MAXHEIGHT
 * and the MNS_ styles do not change rendering: limited). */
static void menu_info_set(struct menu *m, const MENUINFO *i, UINT depth) {
  if (i->fMask & MIM_STYLE)
    m->info_style = i->dwStyle;
  if (i->fMask & MIM_MAXHEIGHT)
    m->max_height = i->cyMax;
  if (i->fMask & MIM_BACKGROUND)
    m->background = i->hbrBack;
  if (i->fMask & MIM_HELPID)
    m->help_id = i->dwContextHelpID;
  if (i->fMask & MIM_MENUDATA)
    m->menu_data = i->dwMenuData;
  if ((i->fMask & MIM_APPLYTOSUBMENUS) && depth < DEPTH)
    for (UINT k = 0; k < m->count; k++) {
      struct menu *s = menu(m->items[k].sub);
      if (s && s != m)
        menu_info_set(s, i, depth + 1);
    }
}
BOOL u32_menu_info(HMENU h, MENUINFO *i, BOOL set) {
  struct menu *m = menu(h);
  if (!m)
    return u32_fail(1401);
  if (!i || i->cbSize != sizeof(*i) || (i->fMask & ~(0x1fu | MIM_APPLYTOSUBMENUS)))
    return u32_fail(87);
  if (set) {
    menu_info_set(m, i, 0);
    return 1;
  }
  if (i->fMask & MIM_STYLE)
    i->dwStyle = m->info_style;
  if (i->fMask & MIM_MAXHEIGHT)
    i->cyMax = m->max_height;
  if (i->fMask & MIM_BACKGROUND)
    i->hbrBack = m->background;
  if (i->fMask & MIM_HELPID)
    i->dwContextHelpID = m->help_id;
  if (i->fMask & MIM_MENUDATA)
    i->dwMenuData = m->menu_data;
  return 1;
}
BOOL W32API GetMenuInfo(HMENU h, MENUINFO *i) {
  U32_GUARD;
  return u32_menu_info(h, i, 0);
}
BOOL W32API SetMenuInfo(HMENU h, const MENUINFO *i) {
  U32_GUARD;
  return u32_menu_info(h, (MENUINFO *)i, 1);
}
/* The screen rectangle of a displayed item: the menu bar of hwnd, or an
 * open popup level. A menu that is not displayed has no rectangle. */
BOOL u32_menu_item_rect(HWND h, HMENU hm, UINT item, RECT *out) {
  struct menu *m = menu(hm);
  if (!m || !out)
    return u32_fail(m ? 87 : 1401);
  if (item >= m->count)
    return u32_fail(1456);
  struct u32_window *w = u32_window(h);
  if (w && w->menu == hm && !(w->style & WS_CHILD) && u32_menu_height(w)) {
    POINT o = u32_surface_origin(w);
    *out = m->items[item].rect;
    OffsetRect(out, o.x, o.y);
    return 1;
  }
  for (UINT i = 0; T.active && i < T.depth; i++)
    if (T.levels[i].menu == hm && T.levels[i].window) {
      RECT r;
      if (!GetWindowRect(T.levels[i].window, &r))
        break;
      *out = m->items[item].rect;
      OffsetRect(out, r.left, r.top);
      return 1;
    }
  return u32_fail(1401);
}
BOOL W32API GetMenuItemRect(HWND h, HMENU hm, UINT item, RECT *out) {
  U32_GUARD;
  return u32_menu_item_rect(h, hm, item, out);
}
