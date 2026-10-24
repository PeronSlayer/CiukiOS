/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include "../../../apps/desktop/sounds.h"
static void appearance_reset_metrics(void);
static struct appearance appearance;
static BOOL appearance_set;
static void appearance_changed(void) {
    appearance_reset_metrics();
    for(UINT i=0;i<U32_WINDOWS;i++) if(u32_windows[i].handle && !u32_windows[i].parent) {
      HWND h=u32_windows[i].handle;
      PostMessageA(h,21,0,0); PostMessageA(h,0x1a,42,0); InvalidateRect(h,0,1);
    }
}
void u32_appearance_load(const char *value) {
    struct appearance next;
    if(!*value) {
      BOOL changed=appearance_set; appearance_set=0;
      if(changed) appearance_changed();
      return;
    }
    if(ap_decode(&next,value)) {
      int changed=!appearance_set;
      const uint8_t *old=(const uint8_t *)&appearance,*fresh=(const uint8_t *)&next;
      for(unsigned i=0;i<sizeof(next);i++) if(old[i]!=fresh[i]) changed=1;
      appearance=next; appearance_set=1;
      if(changed) appearance_changed();
    }
}
static int appearance_metric(int item,int fallback) {
    return appearance_set ? ap_scale(&appearance,appearance.item[item].size) : fallback;
}
static void appearance_font(uint8_t *out,int item) {
    const struct ap_item *v=&appearance.item[item];
    static const char *const names[]={"Inter","Inter","Inter","Inconsolata","Inconsolata","Inter","Inter"};
    w32_fill(out,0,60);
    w32_put32(out,(DWORD)-(int)((v->points*appearance.dpi+36)/72));
    w32_put32(out+16,(v->style&1) ? 700 : 400); out[20]=!!(v->style&2); out[23]=1;
    const char *name=names[v->font]; w32_copy(out+28,name,w32_strlen(name)+1);
}
static unsigned appearance_unscale(uint32_t pixels) {
    return (unsigned)((pixels*96+appearance.dpi/2)/appearance.dpi);
}
static BOOL appearance_set_ncm(const uint8_t *p) {
    struct appearance next=appearance;
    if(w32_u32(p+4)>20 || w32_u32(p+8)>320 || w32_u32(p+20)>320 || w32_u32(p+156)>320) return u32_fail(87);
    static const unsigned metric_offsets[]={4,8,20,156};
    static const int metric_items[]={AP_ACTIVE_BORDER,AP_SCROLLBAR,AP_ACTIVE_TITLE,AP_MENU};
    for(unsigned i=0;i<4;i++) {
      unsigned size=appearance_unscale(w32_u32(p+metric_offsets[i]));
      if(size<(unsigned)ap_size_min(metric_items[i]) || size>(unsigned)ap_size_max(metric_items[i])) return u32_fail(87);
    }
    next.item[AP_ACTIVE_BORDER].size=appearance_unscale(w32_u32(p+4));
    next.item[AP_INACTIVE_BORDER].size=next.item[AP_ACTIVE_BORDER].size;
    next.item[AP_SCROLLBAR].size=appearance_unscale(w32_u32(p+8));
    next.item[AP_ACTIVE_TITLE].size=appearance_unscale(w32_u32(p+20));
    next.item[AP_INACTIVE_TITLE].size=next.item[AP_ACTIVE_TITLE].size;
    next.item[AP_MENU].size=appearance_unscale(w32_u32(p+156));
    static const unsigned offsets[]={24,92,160,220,280};
    static const unsigned items[]={AP_ACTIVE_TITLE,AP_SMALL_TITLE,AP_MENU,AP_TOOLTIP,AP_MESSAGE};
    for(unsigned i=0;i<5;i++) {
      const uint8_t *font=p+offsets[i]; int32_t height=(int32_t)w32_u32(font);
      if(height==INT32_MIN || height>240 || height<-240) return u32_fail(87);
      if(height<0) height=-height;
      unsigned points=((unsigned)height*72+next.dpi/2)/next.dpi;
      if(points<6 || points>36) return u32_fail(87);
      struct ap_item *v=&next.item[items[i]];
      v->points=(uint8_t)points; v->style=(w32_u32(font+16)>=700 ? 1 : 0)|(font[20] ? 2 : 0);
      v->font=font[28]=='I' && font[29]=='n' && font[30]=='c' ? 3 : 0;
    }
    if(!ap_valid(&next)) return u32_fail(87);
    if(!u32_appearance_commit(&next)) return 0;
    appearance=next; return 1;
}
static DWORD colors[29] = {0xc0c0c0, 0x808000, 0x800000, 0x808080, 0xc0c0c0,
                           0xffffff, 0,        0,        0,        0xffffff,
                           0xc0c0c0, 0xc0c0c0, 0x808080, 0x800000, 0xffffff,
                           0xc0c0c0, 0x808080, 0x808080, 0,        0xc0c0c0,
                           0xffffff, 0x000000, 0xdfdfdf, 0x000000, 0xe1ffff,
                           0x000000, 0x000000, 0xd0d0d0, 0x000000};
static DWORD color_overrides;
static DWORD settings[128];
int u32_spi_metric(int, int);
static uint8_t setting_written[128];
DWORD W32API GetSysColor(int n) {
  U32_GUARD;
  u32_settings();
  if(n>=0 && n<29 && (color_overrides&(1u<<n))) return colors[n];
  if(appearance_set && n>=0 && n<29) {
    static const uint8_t items[29]={AP_SCROLLBAR,AP_DESKTOP,AP_ACTIVE_TITLE,AP_INACTIVE_TITLE,AP_MENU,
      AP_WINDOW,AP_WINDOW,AP_MENU,AP_WINDOW,AP_ACTIVE_TITLE,AP_ACTIVE_BORDER,AP_INACTIVE_BORDER,
      AP_WORKSPACE,AP_SELECTED,AP_SELECTED,AP_3D,AP_3D,AP_DISABLED,AP_3D,AP_INACTIVE_TITLE,
      AP_3D,AP_3D,AP_3D,AP_TOOLTIP,AP_TOOLTIP,AP_WINDOW,AP_HOT,AP_ACTIVE_TITLE,AP_INACTIVE_TITLE};
    uint32_t rgb;
    const struct ap_item *v=&appearance.item[items[n]];
    if(n==6 || n==7 || n==8 || n==9 || n==14 || n==17 || n==18 || n==19 || n==23 || n==26) rgb=v->text;
    else if(n==27 || n==28) rgb=v->gradient;
    else if(n==16 || n==21) rgb=n==21 ? 0 : 0x808080;
    else if(n==20 || n==22) rgb=n==20 ? 0xffffff : 0xdfdfdf;
    else rgb=v->colour;
    return ((rgb&255)<<16)|(rgb&0xff00)|((rgb>>16)&255);
  }
  return n >= 0 && n < 29 ? colors[n] : 0;
}
BOOL W32API SetSysColors(int n, const int *index, const DWORD *values) {
  U32_GUARD;
  if (n < 0 || (!index && n) || (!values && n))
    return u32_fail(87);
  for (int i = 0; i < n; i++)
    if (index[i] < 0 || index[i] >= 29)
      return u32_fail(87);
  for (int i = 0; i < n; i++) {
    colors[index[i]] = values[i] & 0xffffff; color_overrides|=1u<<index[i];
  }
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (u32_windows[i].handle && !u32_windows[i].parent)
      SendMessageA(u32_windows[i].handle, 21, 0, 0);
  return 1;
}
int W32API GetSystemMetrics(int n) {
  U32_GUARD;
  u32_settings();
  if(appearance_set) switch(n) {
    case 2: case 3: case 9: case 10: case 20: case 21: return appearance_metric(AP_SCROLLBAR,16);
    case 4: return appearance_metric(AP_ACTIVE_TITLE,24);
    case 30: case 31: return ap_scale(&appearance,appearance.item[AP_ACTIVE_TITLE].size-6);
    case 49: case 50: return ap_scale(&appearance,16);
    case 51: case 52: case 53: return appearance_metric(AP_SMALL_TITLE,18);
    case 54: case 55: return appearance_metric(AP_MENU,18);
    case 5: case 6: case 7: case 8: case 32: case 33: return appearance_metric(AP_ACTIVE_BORDER,3);
    case 11: case 12: return ap_scale(&appearance,(appearance.effects&AP_LARGE_ICONS) ? 48 : appearance.item[AP_ICON].size);
    case 15: return appearance_metric(AP_MENU,18);
    case 38: return appearance_metric(AP_ICON_X,96);
    case 39: return appearance_metric(AP_ICON_Y,94);
    default: break;
  }
  struct ciuki_display_info d = {.version = 1, .size = sizeof(d)};
  if ((int32_t)u32_backend.gate(CIUKI_SYS_DISPLAY, CIUKI_DISPLAY_QUERY,
                                U32_PTR(&d), 0) == 0 &&
      d.width && d.height) {
    u32_screen_w = d.width;
    u32_screen_h = d.height;
  }
  switch (n) {
  case 0:
  case 16:
  case 59:
  case 61:
    return u32_screen_w;
  case 1:
  case 17:
  case 60:
  case 62:
    return n == 17 ? (int)u32_screen_h - 32 : (int)u32_screen_h;
  case 2:
  case 3:
  case 9:
  case 10:
  case 20:
  case 21:
    return 16;
  case 4:
    return 21;
  case 5:
  case 6:
    return 1;
  case 7:
  case 8:
    return 3;
  case 11:
  case 12:
  case 13:
  case 14:
    return 32;
  case 15:
    return 18;
  case 19:
    return 1;
  case 28:
    return 112;
  case 29:
    return 27;
  case 30:
  case 31:
    return 18;
  case 32:
  case 33:
    return 3;
  case 34:
  case 35:
    return 112;
  case 23:
  case 36:
  case 37:
  case 38:
  case 39:
    return u32_spi_metric(n, 0);
  case 40:
    return 0;
  case 43:
    return 3;
  case 45:
    return 2;
  case 49:
  case 50:
    return 16;
  case 51:
  case 52:
    return 18;
  case 63:
  case 64:
    return 0;
  case 67:
  case 68:
    return 4;
  case 69:
    return 0;
  case 75:
    return 1;
  case 70: /* SM_SHOWSOUNDS */
    return u32_spi_metric(n, 0);
  case 80:
    return 1;
  default:
    return 0;
  }
}
static void logfont(uint8_t *out, BOOL bold) {
  w32_fill(out, 0, 60);
  w32_put32(out, (DWORD)-11);
  w32_put32(out + 16, bold ? 700 : 400);
  out[23] = 1;
  out[27] = 0x22;
  w32_copy(out + 28, "MS Sans Serif", 14);
}
/* Except for the shared G10 saver path below, SPI state is process-global
 * for every GUI thread and never persisted: SPIF_UPDATEINIFILE is accepted without writing native
 * desktop settings; SPIF_SENDCHANGE notifies this process's top-level windows
 * with WM_SETTINGCHANGE (wParam = action). Each setter follows the argument
 * convention Microsoft documents for its action (uiParam value, pvParam
 * value, or pvParam structure). Windows 98 SE defaults. */
static struct {
  int mouse[3];
  UINT mouse_speed, icon_x, icon_y, title_wrap, menu_drop, click_w, click_h,
      swap, beep, keyboard_pref, show_sounds, min_animate;
  RECT work;
  BOOL work_set, ncm_set, font_set, minimized_set, icons_set;
  uint8_t ncm[340], icon_font[60], minimized[20], icon_metrics[76];
} spi = {.mouse = {6, 10, 1}, .mouse_speed = 10, .icon_x = 75, .icon_y = 75,
         .title_wrap = 1, .click_w = 4, .click_h = 4, .beep = 1,
         .min_animate = 1};
static UINT input_values[DESK_INPUT_VALUES]={0,1,31,530,0,0,3,0,10,500,1};
void u32_input_snapshot(const uint32_t *values)
{
  BOOL layout_changed=u32_layout!=values[0];
  if(layout_changed) {
    struct u32_thread *t=u32_thread();if(t)t->dead_accent=0;
  }
  BOOL caret_changed=input_values[3]!=values[3];
  for(UINT k=0;k<DESK_INPUT_VALUES;k++)input_values[k]=values[k];
  if(caret_changed)u32_caret_preferences();
  u32_layout=values[0];spi.swap=values[4];spi.mouse_speed=values[8];
  if(layout_changed)for(UINT i=0;i<U32_WINDOWS;i++)if(u32_windows[i].handle && !u32_windows[i].parent)
    SendNotifyMessageA(u32_windows[i].handle,0x51,0,u32_layout ? 0x04100410u : 0x04090409u);
}
UINT u32_input_value(UINT key) {return key && key<=DESK_INPUT_VALUES ? input_values[key-1] : 0;}
BOOL u32_input_setting(UINT key, UINT value, UINT flags)
{
  uint8_t data[12];desk_put32(data,key);desk_put32(data+4,value);desk_put32(data+8,flags&3);
  struct u32_window context={0};
  return u32_wire(&context,DESK_INPUT_SETTINGS,data,sizeof(data),-1);
}
void u32_input_refresh(void)
{
  static BOOL busy,read;static DWORD last;
  DWORD now=u32_ticks();
  if(busy || (read && now-last<1000))return;
  busy=1;read=1;last=now;
  DWORD error=GetLastError();(void)u32_input_setting(0,0,0);SetLastError(error);busy=0;
static void appearance_reset_metrics(void) {
    spi.ncm_set=spi.font_set=spi.icons_set=0; color_overrides=0;
}
int u32_spi_metric(int n, int fallback) {
  u32_input_refresh();
  switch (n) {
  case 23: /* SM_SWAPBUTTON */
    return (int)spi.swap;
  case 36: /* SM_CXDOUBLECLK */
    return (int)spi.click_w;
  case 37:
    return (int)spi.click_h;
  case 38: /* SM_CXICONSPACING */
    return (int)spi.icon_x;
  case 39:
    return (int)spi.icon_y;
  case 70: /* SM_SHOWSOUNDS */
    return (int)spi.show_sounds;
  default:
    return fallback;
  }
}
/* G25: the accessibility records of SystemParametersInfo (STICKYKEYS, FILTERKEYS, TOGGLEKEYS, MOUSEKEYS,
 * SOUNDSENTRY, HIGHCONTRAST, ACCESSTIMEOUT). Like every value above they are process-global and never persisted;
 * the defaults are those of the desktop's settings (apps/desktop/access.h access_defaults): the flag words are the
 * Win32 values, so a bridge to the desktop is a field copy. The latched and locked bits of STICKYKEYS and the
 * button and mode bits of MOUSEKEYS are state, ignored on SET and zero on GET (the desktop owns the keys).
 * SerialKeys (SPI 62, 63) is not provided. docs/design/accessibility.md. */
static struct {
  DWORD sticky, filter, wait, delay, repeat, bounce, toggle, mouse, mouse_max,
      mouse_time, mouse_ctrl, sentry, sentry_window, sentry_ms, contrast,
      timeout, timeout_ms;
  char scheme[32];
} acc = {.sticky = 0x1fe, .filter = 0x7e, .wait = 1000, .delay = 1000,
         .repeat = 500, .toggle = 0x3e, .mouse = 0x7e, .mouse_max = 80,
         .mouse_time = 3000, .mouse_ctrl = 8, .sentry = 2, .sentry_ms = 200,
         .contrast = 0x7e, .timeout_ms = 300000,
         .scheme = "High Contrast Black"};
#define ACC_AVAILABLE 2u
/* HIGHCONTRASTA: its pointer is as wide as the build's (12 bytes on the 32-bit guest). */
struct acc_contrast { UINT cb; DWORD flags; char *scheme; };
static int acc_copy_name(char *to, UINT size, const char *from) {
  UINT n = 0;
  while (from[n])
    n++;
  if (n >= size)
    return 0;
  w32_copy(to, from, n + 1);
  return 1;
}
static BOOL acc_size(const void *p, UINT param, UINT size) {
  if (!p || param != size || w32_u32(p) != size)
    return u32_fail(87);
  return 1;
}
static BOOL acc_get(UINT action, UINT param, void *out) {
  uint8_t *o = out;
  switch (action) {
  case 58: /* SPI_GETSTICKYKEYS */
    if (!acc_size(out, param, 8))
      return 0;
    w32_put32(o + 4, (acc.sticky | ACC_AVAILABLE) & 0x1ffu);
    return 1;
  case 50: /* SPI_GETFILTERKEYS */
    if (!acc_size(out, param, 24))
      return 0;
    w32_put32(o + 4, (acc.filter | ACC_AVAILABLE) & 0x7fu);
    w32_put32(o + 8, acc.wait);
    w32_put32(o + 12, acc.delay);
    w32_put32(o + 16, acc.repeat);
    w32_put32(o + 20, acc.bounce);
    return 1;
  case 52: /* SPI_GETTOGGLEKEYS */
    if (!acc_size(out, param, 8))
      return 0;
    w32_put32(o + 4, (acc.toggle | ACC_AVAILABLE) & 0x3fu);
    return 1;
  case 54: /* SPI_GETMOUSEKEYS */
    if (!acc_size(out, param, 28))
      return 0;
    w32_put32(o + 4, (acc.mouse | ACC_AVAILABLE) & 0xffu);
    w32_put32(o + 8, acc.mouse_max);
    w32_put32(o + 12, acc.mouse_time);
    w32_put32(o + 16, acc.mouse_ctrl);
    w32_put32(o + 20, 0);
    w32_put32(o + 24, 0);
    return 1;
  case 64: /* SPI_GETSOUNDSENTRY */
    if (!acc_size(out, param, 48))
      return 0;
    w32_fill(o + 4, 0, 44);
    w32_put32(o + 4, (acc.sentry | ACC_AVAILABLE) & 0x7u);
    w32_put32(o + 32, acc.sentry_window);
    w32_put32(o + 36, acc.sentry_ms);
    return 1;
  case 66: { /* SPI_GETHIGHCONTRAST: lpszDefaultScheme receives the name when the caller supplied a buffer */
    if (!acc_size(out, param, sizeof(struct acc_contrast)))
      return 0;
    struct acc_contrast *hc = out;
    hc->flags = acc.contrast & 0x7fu;
    if (hc->scheme)
      acc_copy_name(hc->scheme, sizeof(acc.scheme), acc.scheme);
    return 1;
  }
  case 60: /* SPI_GETACCESSTIMEOUT */
    if (!acc_size(out, param, 12))
      return 0;
    w32_put32(o + 4, acc.timeout & 3u);
    w32_put32(o + 8, acc.timeout_ms);
    return 1;
  default:
    return -1;
  }
}
static BOOL acc_set(UINT action, UINT param, const void *in) {
  const uint8_t *i = in;
  switch (action) {
  case 59: /* SPI_SETSTICKYKEYS */
    if (!acc_size(in, param, 8))
      return 0;
    acc.sticky = w32_u32(i + 4) & 0x1ffu;
    return 1;
  case 51: { /* SPI_SETFILTERKEYS: BounceKeys excludes the other three (FILTERKEYS remarks), 20 s at most */
    if (!acc_size(in, param, 24))
      return 0;
    DWORD wait = w32_u32(i + 8), delay = w32_u32(i + 12),
          repeat = w32_u32(i + 16), bounce = w32_u32(i + 20);
    if (wait > 20000 || delay > 20000 || repeat > 20000 || bounce > 20000 ||
        (bounce ? (wait || delay || repeat) : (!wait || !delay || !repeat)))
      return u32_fail(87);
    acc.filter = w32_u32(i + 4) & 0x107fu;
    acc.wait = wait; acc.delay = delay; acc.repeat = repeat; acc.bounce = bounce;
    return 1;
  }
  case 53: /* SPI_SETTOGGLEKEYS */
    if (!acc_size(in, param, 8))
      return 0;
    acc.toggle = w32_u32(i + 4) & 0x3fu;
    return 1;
  case 55: { /* SPI_SETMOUSEKEYS: the Windows NT ranges, reserved fields zero */
    if (!acc_size(in, param, 28))
      return 0;
    DWORD max = w32_u32(i + 8), time = w32_u32(i + 12), ctrl = w32_u32(i + 16);
    if (max < 10 || max > 360 || time < 1000 || time > 5000 || !ctrl ||
        ctrl > 32 || w32_u32(i + 20) || w32_u32(i + 24))
      return u32_fail(87);
    acc.mouse = w32_u32(i + 4) & 0xffu;
    acc.mouse_max = max; acc.mouse_time = time; acc.mouse_ctrl = ctrl;
    return 1;
  }
  case 65: { /* SPI_SETSOUNDSENTRY: cbSize, dwFlags and iWindowsEffect; no custom effect DLLs */
    if (!acc_size(in, param, 48))
      return 0;
    DWORD effect = w32_u32(i + 32);
    if (effect > 3)
      return u32_fail(87);
    acc.sentry = w32_u32(i + 4) & 0x7u;
    acc.sentry_window = effect;
    if (w32_u32(i + 36))
      acc.sentry_ms = w32_u32(i + 36);
    return 1;
  }
  case 67: { /* SPI_SETHIGHCONTRAST */
    if (!acc_size(in, param, sizeof(struct acc_contrast)))
      return 0;
    const struct acc_contrast *hc = in;
    const char *name = hc->scheme;
    char scheme[32];
    if (name) {
      if (!acc_copy_name(scheme, sizeof(scheme), name) || !scheme[0])
        return u32_fail(87);
      acc_copy_name(acc.scheme, sizeof(acc.scheme), scheme);
    }
    acc.contrast = hc->flags & 0x7fu;
    return 1;
  }
  case 61: /* SPI_SETACCESSTIMEOUT */
    if (!acc_size(in, param, 12))
      return 0;
    acc.timeout = w32_u32(i + 4) & 3u;
    acc.timeout_ms = w32_u32(i + 8);
    return 1;
  default:
    return -1;
  }
}
static BOOL put_bool(void *out, UINT value) {
  if (!out)
    return u32_fail(87);
  *(BOOL *)out = (BOOL)value;
  return 1;
}
static BOOL spi_get(UINT action, UINT param, void *out) {
  switch (action) {
  case 1: /* SPI_GETBEEP */
    return put_bool(out, spi.beep);
  case 3: /* SPI_GETMOUSE */
    if (!out)
      return u32_fail(87);
    w32_copy(out, spi.mouse, sizeof(spi.mouse));
    return 1;
  case 13: /* SPI_ICONHORIZONTALSPACING / 24 VERTICAL: get when pvParam */
  case 24:
    if (!out)
      return -1;
    *(int *)out = appearance_set ? appearance_metric(action==13 ? AP_ICON_X : AP_ICON_Y,75) : (int)(action == 13 ? spi.icon_x : spi.icon_y);
    return 1;
  case 25: /* SPI_GETICONTITLEWRAP */
    return put_bool(out, spi.title_wrap);
  case 27: /* SPI_GETMENUDROPALIGNMENT */
    return put_bool(out, spi.menu_drop);
  case 31: /* SPI_GETICONTITLELOGFONT */
    if (!out)
      return u32_fail(87);
    if(appearance_set) { appearance_font(out,AP_ICON); return 1; }
    if (spi.font_set)
      w32_copy(out, spi.icon_font, 60);
    else
      logfont(out, 0);
    return 1;
  case 41: { /* SPI_GETNONCLIENTMETRICS */
    if (!out || w32_u32(out) != 340)
      return u32_fail(87);
    uint8_t *p = out;
    if (spi.ncm_set) {
      w32_copy(p + 4, spi.ncm + 4, 336);
      return 1;
    }
    if(appearance_set) {
      w32_fill(p+4,0,336);
      w32_put32(p+4,appearance_metric(AP_ACTIVE_BORDER,3));
      w32_put32(p+8,appearance_metric(AP_SCROLLBAR,16)); w32_put32(p+12,appearance_metric(AP_SCROLLBAR,16));
      w32_put32(p+16,ap_scale(&appearance,appearance.item[AP_ACTIVE_TITLE].size-6)); w32_put32(p+20,appearance_metric(AP_ACTIVE_TITLE,24));
      appearance_font(p+24,AP_ACTIVE_TITLE);
      w32_put32(p+84,appearance_metric(AP_SMALL_TITLE,18)); w32_put32(p+88,appearance_metric(AP_SMALL_TITLE,18));
      appearance_font(p+92,AP_SMALL_TITLE);
      w32_put32(p+152,appearance_metric(AP_MENU,18)); w32_put32(p+156,appearance_metric(AP_MENU,18));
      appearance_font(p+160,AP_MENU); appearance_font(p+220,AP_TOOLTIP); appearance_font(p+280,AP_MESSAGE);
      return 1;
    }
    w32_fill(p + 4, 0, 336);
    w32_put32(p + 4, 1);
    w32_put32(p + 8, 16);
    w32_put32(p + 12, 16);
    w32_put32(p + 16, 18);
    w32_put32(p + 20, 21);
    logfont(p + 24, 1);
    w32_put32(p + 84, 16);
    w32_put32(p + 88, 21);
    logfont(p + 92, 0);
    w32_put32(p + 152, 18);
    w32_put32(p + 156, 18);
    logfont(p + 160, 0);
    logfont(p + 220, 0);
    logfont(p + 280, 0);
    return 1;
  }
  case 43: /* SPI_GETMINIMIZEDMETRICS */
    if (!out || w32_u32(out) != 20)
      return u32_fail(87);
    if (spi.minimized_set) {
      w32_copy((uint8_t *)out + 4, spi.minimized + 4, 16);
      return 1;
    }
    w32_put32((uint8_t *)out + 4, 160);
    w32_put32((uint8_t *)out + 8, 0);
    w32_put32((uint8_t *)out + 12, 0);
    w32_put32((uint8_t *)out + 16, 8);
    return 1;
  case 45: /* SPI_GETICONMETRICS */
    if (!out || w32_u32(out) != 76)
      return u32_fail(87);
    if (spi.icons_set) {
      w32_copy((uint8_t *)out + 4, spi.icon_metrics + 4, 72);
      return 1;
    }
    w32_put32((uint8_t *)out + 4, appearance_set ? appearance_metric(AP_ICON_X,75) : spi.icon_x);
    w32_put32((uint8_t *)out + 8, appearance_set ? appearance_metric(AP_ICON_Y,75) : spi.icon_y);
    w32_put32((uint8_t *)out + 12, spi.title_wrap);
    if(appearance_set) appearance_font((uint8_t *)out+16,AP_ICON);
    else logfont((uint8_t *)out + 16, 0);
    return 1;
  case 48: /* SPI_GETWORKAREA */
    if (!out)
      return u32_fail(87);
    GetSystemMetrics(0);
    *(RECT *)out =
        spi.work_set ? spi.work : (RECT){0, 32, u32_screen_w, u32_screen_h};
    return 1;
  case 56: /* SPI_GETSHOWSOUNDS */
    return put_bool(out, spi.show_sounds);
  case 68: /* SPI_GETKEYBOARDPREF */
    return put_bool(out, spi.keyboard_pref);
  case 72: /* SPI_GETANIMATION */
    if (!out || w32_u32(out) != 8)
      return u32_fail(87);
    w32_put32((uint8_t *)out + 4, spi.min_animate);
    return 1;
  case 112: /* SPI_GETMOUSESPEED */
    if (!out)
      return u32_fail(87);
    *(int *)out = (int)spi.mouse_speed;
    return 1;
  default:
    return acc_get(action, param, out);
  }
}
static BOOL spi_set(UINT action, UINT param, void *in) {
  switch (action) {
  case 2: /* SPI_SETBEEP: uiParam */
    spi.beep = !!param;
    return 1;
  case 4: /* SPI_SETMOUSE: pvParam int[3] */
    if (!in)
      return u32_fail(87);
    w32_copy(spi.mouse, in, sizeof(spi.mouse));
    return 1;
  case 13: /* pvParam NULL: uiParam is the new spacing */
  case 24:
    if (param < 32 || param > 4096)
      return u32_fail(87);
    *(action == 13 ? &spi.icon_x : &spi.icon_y) = param;
    return 1;
  case 20: /* SPI_SETDESKWALLPAPER, 21 SETDESKPATTERN: the desktop owns these */
  case 21:
  case 87: /* SPI_SETCURSORS / 88 SPI_SETICONS: reload from the registry */
  case 88:
    return 1;
  case 26: /* SPI_SETICONTITLEWRAP: uiParam */
    spi.title_wrap = !!param;
    return 1;
  case 28: /* SPI_SETMENUDROPALIGNMENT: uiParam */
    spi.menu_drop = !!param;
    return 1;
  case 29: /* SPI_SETDOUBLECLKWIDTH / 30 HEIGHT: uiParam */
  case 30:
    if (param > 1024)
      return u32_fail(87);
    *(action == 29 ? &spi.click_w : &spi.click_h) = param;
    return 1;
  case 32: /* SPI_SETDOUBLECLICKTIME: uiParam */
    return SetDoubleClickTime(param);
  case 33: /* SPI_SETMOUSEBUTTONSWAP: uiParam */
    spi.swap = !!param;
    return 1;
  case 34: /* SPI_SETICONTITLELOGFONT: pvParam LOGFONTA, uiParam its size */
    if (!in || (param && param != 60))
      return u32_fail(87);
    w32_copy(spi.icon_font, in, 60);
    spi.font_set = 1;
    return 1;
  case 42: /* SPI_SETNONCLIENTMETRICS */
    if (!in || w32_u32(in) != 340)
      return u32_fail(87);
    if(appearance_set) return appearance_set_ncm(in);
    w32_copy(spi.ncm, in, 340);
    spi.ncm_set = 1;
    return 1;
  case 44: /* SPI_SETMINIMIZEDMETRICS */
    if (!in || w32_u32(in) != 20)
      return u32_fail(87);
    w32_copy(spi.minimized, in, 20);
    spi.minimized_set = 1;
    return 1;
  case 46: /* SPI_SETICONMETRICS */
    if (!in || w32_u32(in) != 76)
      return u32_fail(87);
    w32_copy(spi.icon_metrics, in, 76);
    spi.icon_x = w32_u32(spi.icon_metrics + 4);
    spi.icon_y = w32_u32(spi.icon_metrics + 8);
    spi.title_wrap = !!w32_u32(spi.icon_metrics + 12);
    spi.icons_set = 1;
    return 1;
  case 47: { /* SPI_SETWORKAREA: pvParam RECT, NULL = the full screen */
    GetSystemMetrics(0);
    RECT screen = {0, 0, u32_screen_w, u32_screen_h}, r;
    if (!in) {
      spi.work = screen;
      spi.work_set = 1;
      return 1;
    }
    r = *(const RECT *)in;
    if (IsRectEmpty(&r) || r.left < 0 || r.top < 0 ||
        r.right > screen.right || r.bottom > screen.bottom)
      return u32_fail(87);
    spi.work = r;
    spi.work_set = 1;
    return 1;
  }
  case 57: /* SPI_SETSHOWSOUNDS: uiParam */
    spi.show_sounds = !!param;
    return 1;
  case 69: /* SPI_SETKEYBOARDPREF: uiParam */
    spi.keyboard_pref = !!param;
    return 1;
  case 73: /* SPI_SETANIMATION: pvParam ANIMATIONINFO */
    if (!in || w32_u32(in) != 8)
      return u32_fail(87);
    spi.min_animate = !!w32_u32((const uint8_t *)in + 4);
    return 1;
  case 113: /* SPI_SETMOUSESPEED: the value is pvParam itself (1..20) */
    if ((uintptr_t)in < 1 || (uintptr_t)in > 20)
      return u32_fail(87);
    spi.mouse_speed = (UINT)(uintptr_t)in;
    return 1;
  default:
    return acc_set(action, param, in);
  }
}
/* G10: share native desktop policy across all Win32 processes. */
BOOL u32_saver_command(UINT command, int value) {
  struct ciuki_screen_control q = {.version=CIUKI_SCREEN_VERSION,.size=sizeof(q),
                                  .action=command,.value=value};
  int32_t e=(int32_t)u32_backend.gate(CIUKI_SYS_DISPLAY,CIUKI_DISPLAY_SCREEN,U32_PTR(&q),0);
  return e<0 ? u32_fail(e==-22 ? 87 : 50) : 1;
}
static BOOL saver_spi(UINT action, UINT param, void *out, UINT flags) {
  BOOL get=action==14 || action==16 || action==114 || action==118;
  if (get && !out) return u32_fail(87);
  /* G32 owns passwords; never report a successful secure lock setter. */
  if (action==119) return u32_fail(120);
  if (action==118) { *(BOOL *)out=0; return 1; }
  struct ciuki_screen_control q={.version=CIUKI_SCREEN_VERSION,.size=sizeof(q),
      .action=get ? 0 : action,.value=(int32_t)param,
      .flags=(flags&1) ? CIUKI_SCREEN_PERSIST : 0};
  if (action==15 && param>INT32_MAX) return u32_fail(87);
  int32_t e=(int32_t)u32_backend.gate(CIUKI_SYS_DISPLAY,CIUKI_DISPLAY_SCREEN,U32_PTR(&q),0);
  if (e<0) return u32_fail(e==-22 ? 87 : 50);
  if (get) *(UINT *)out=action==14 ? q.timeout : action==16 ? q.enabled : q.running;
  if (action==97 && out) *(BOOL *)out=q.value;
  if (!get && (flags&2))
    for (UINT i=0;i<U32_WINDOWS;i++)
      if (u32_windows[i].handle && !u32_windows[i].parent)
        SendNotifyMessageA(u32_windows[i].handle,0x1a,action,0);
  return 1;
}
BOOL W32API SystemParametersInfoA(UINT action, UINT param, void *out,
                                  UINT flags) {
  U32_GUARD;
  u32_input_refresh();
  UINT key=action==10 || action==11 ? DESK_INPUT_SPEED : action==22 || action==23 ? DESK_INPUT_DELAY :
      action==94 || action==93 ? DESK_INPUT_TRAILS : action==104 || action==105 ? DESK_INPUT_WHEEL :
      action==112 || action==113 ? DESK_INPUT_MOUSE_SPEED : action==33 ? DESK_INPUT_SWAP :
      action==32 ? DESK_INPUT_DOUBLE_CLICK : 0;
  if(key) {
    BOOL get=action==10 || action==22 || action==94 || action==104 || action==112;
    if(get) {
      if(!out)return u32_fail(87);
      UINT value=u32_input_value(key);
      if(key==DESK_INPUT_WHEEL && value==101)value=0xffffffffu;
      *(UINT *)out=value;return 1;
    }
    UINT value=action==113 ? (UINT)(uintptr_t)out : param;
    if(key==DESK_INPUT_SPEED && value>31)value=31;
    if(key==DESK_INPUT_WHEEL && value==0xffffffffu)value=101;
    if(key==DESK_INPUT_MOUSE_SPEED) {if(value<1 || value>20)return u32_fail(87);}
    if(key==DESK_INPUT_SWAP)value=!!value;
    if(key==DESK_INPUT_DOUBLE_CLICK)value=value ? (value>5000 ? 5000 : value) : 500;
    BOOL result=u32_input_setting(key,value,flags);
    return result;
  u32_settings();
  if(appearance_set && (action==38 || action==74 || action==0x1002)) {
    if(!out) return u32_fail(87);
    *(BOOL *)out=!!(appearance.effects&(action==38 ? AP_FULL_DRAG : action==74 ? AP_SMOOTH : AP_MENU_ANIMATION)); return 1;
  }
  if(!appearance_set && action==0x1002) return put_bool(out,1);
  if(action==37 || action==75 || action==0x1003) {
    unsigned bit=action==37 ? AP_FULL_DRAG : action==75 ? AP_SMOOTH : AP_MENU_ANIMATION;
    unsigned enabled=action==0x1003 ? !!(uintptr_t)out : !!param;
    struct appearance next=appearance;
    if(!appearance_set) ap_default(&next,5);
    next.effects=(uint16_t)((next.effects&~bit)|(enabled ? bit : 0));
    if(!u32_appearance_commit(&next)) return 0;
    appearance=next; appearance_set=1;
    if(flags&2) for(UINT i=0;i<U32_WINDOWS;i++)
      if(u32_windows[i].handle && !u32_windows[i].parent) SendNotifyMessageA(u32_windows[i].handle,0x1a,action,0);
    return 1;
  }
  if (action==14 || action==15 || action==16 || action==17 || action==97 ||
      action==114 || action==118 || action==119) return saver_spi(action,param,out,flags);
  static const struct {
    UINT get, set, value;
  } values[] = {{5, 6, 1},   {10, 11, 31},
                {22, 23, 1}, {38, 37, 1},     {70, 71, 0},   {74, 75, 1},
                {94, 93, 0}, {102, 103, 400}, {104, 105, 3}, {106, 107, 400}};
  BOOL setter = 0, result = (BOOL)-1;
  for (UINT i = 0; i < sizeof(values) / sizeof(*values); i++) {
    UINT key = values[i].get;
    if (action == key) {
      if (!out)
        return u32_fail(87);
      *(DWORD *)out = setting_written[key] ? settings[key] : values[i].value;
      return 1;
    }
    if (action == values[i].set) {
      settings[key] = param;
      setting_written[key] = 1;
      setter = 1;
      result = 1;
    }
  }
  if (result == (BOOL)-1) {
    result = spi_get(action, param, out);
    if (result == (BOOL)-1) {
      result = spi_set(action, param, out);
      setter = result == 1;
    }
  }
  if (result == (BOOL)-1)
    return u32_fail(120);
  if (setter && (flags & 2))
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle && !u32_windows[i].parent)
        SendNotifyMessageA(u32_windows[i].handle, 0x1a, action, 0);
  return result;
}
/* MessageBeep plays the user's system sound for the icon (the desktop owns the
 * scheme, volume and mute): MB_ICONERROR/HAND/STOP -> Error, MB_ICONEXCLAMATION
 * and MB_ICONWARNING -> Error (the scheme has one alert sound), MB_ICONQUESTION
 * -> Question, MB_ICONASTERISK/INFORMATION and MB_OK or any other value (and
 * 0xFFFFFFFF, the simple beep) -> Information. The sound is queued and the call
 * returns; with SPI_SETBEEP off, or before the process has a window to carry the
 * request, nothing is played and the call still succeeds. */
BOOL W32API MessageBeep(UINT type) {
  U32_GUARD;
  if (!spi.beep)
    return 1;
  UINT event = type == 0x10 || type == 0x30 ? SOUND_ERROR
               : type == 0x20               ? SOUND_QUESTION
                                            : SOUND_INFORMATION;
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    if (!w->handle || !w->wire)
      continue;
    uint8_t data[4];
    desk_put32(data, event);
    u32_wire(w, DESK_V3_SOUND, data, 4, -1);
    return 1;
  }
  return 1;
}
BOOL W32API ExitWindowsEx(UINT flags, DWORD reserved) {
  U32_GUARD;
  (void)reserved;
  if (flags & ~0x1fu)
    return u32_fail(87);
  struct {struct w32r_request r;char empty[2];} a={.r={.version=1,.op=W32R_SHUTDOWN,
    .index=W32R_SHUTDOWN_REQUEST,.flags=flags,.path_bytes=1,.name_bytes=1}};
  struct w32r_reply reply;DWORD used=0;
  DWORD e=u32_backend.rpc(&a,sizeof(a.r)+2,&reply,sizeof(reply),&used);
  if(!e)e=used==sizeof(reply)?reply.error:13;
  return e?u32_fail(e):1;
}
