/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
static DWORD colors[29] = {0xc0c0c0, 0x808000, 0x800000, 0x808080, 0xc0c0c0,
                           0xffffff, 0,        0,        0,        0xffffff,
                           0xc0c0c0, 0xc0c0c0, 0x808080, 0x800000, 0xffffff,
                           0xc0c0c0, 0x808080, 0x808080, 0,        0xc0c0c0,
                           0xffffff, 0x000000, 0xdfdfdf, 0x000000, 0xe1ffff,
                           0x000000, 0x000000, 0xd0d0d0, 0x000000};
static DWORD settings[128];
int u32_spi_metric(int, int);
static uint8_t setting_written[128];
DWORD W32API GetSysColor(int n) {
  U32_GUARD;
  return n >= 0 && n < 29 ? colors[n] : 0;
}
BOOL W32API SetSysColors(int n, const int *index, const DWORD *values) {
  U32_GUARD;
  if (n < 0 || (!index && n) || (!values && n))
    return u32_fail(87);
  for (int i = 0; i < n; i++)
    if (index[i] < 0 || index[i] >= 29)
      return u32_fail(87);
  for (int i = 0; i < n; i++)
    colors[index[i]] = values[i] & 0xffffff;
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (u32_windows[i].handle && !u32_windows[i].parent)
      SendMessageA(u32_windows[i].handle, 21, 0, 0);
  return 1;
}
int W32API GetSystemMetrics(int n) {
  U32_GUARD;
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
  default:
    return fallback;
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
    *(int *)out = (int)(action == 13 ? spi.icon_x : spi.icon_y);
    return 1;
  case 25: /* SPI_GETICONTITLEWRAP */
    return put_bool(out, spi.title_wrap);
  case 27: /* SPI_GETMENUDROPALIGNMENT */
    return put_bool(out, spi.menu_drop);
  case 31: /* SPI_GETICONTITLELOGFONT */
    if (!out)
      return u32_fail(87);
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
    w32_put32((uint8_t *)out + 4, spi.icon_x);
    w32_put32((uint8_t *)out + 8, spi.icon_y);
    w32_put32((uint8_t *)out + 12, spi.title_wrap);
    logfont((uint8_t *)out + 16, 0);
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
    (void)param;
    return -1;
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
    return -1;
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
  }
  if (action==14 || action==15 || action==16 || action==17 || action==97 ||
      action==114 || action==118 || action==119) return saver_spi(action,param,out,flags);
  static const struct {
    UINT get, set, value;
  } values[] = {{5, 6, 1},   {10, 11, 31},
                {22, 23, 1}, {38, 37, 0},     {70, 71, 0},   {74, 75, 0},
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
BOOL W32API MessageBeep(UINT type) {
  U32_GUARD;
  (void)type;
  static BOOL once;
  if (!once) {
    once = 1;
    w32_diag("stub", "user32.dll", "MessageBeep");
  }
  return u32_fail(120);
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
