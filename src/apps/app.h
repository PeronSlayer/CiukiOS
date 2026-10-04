/* CiukiOS desktop application modules (Files, Tasks, CiukNote, CiukPaint).
 *
 * A module is a flat 16-bit image loaded by SHELL.COM at offset 100h of its
 * own memory block (src/com/shell_apps.inc). CS = DS = SS = that block while
 * it runs: app_start.asm switches to the module's own stack. The shell calls
 * app_event() for each event and the module draws through one far service
 * entry (app_svc). Coordinates are screen pixels; HOST gives the window.
 * No C library: rt.c provides what the modules use.
 */
#ifndef CIUKIOS_APP_H
#define CIUKIOS_APP_H

#define EV_OPEN    1   /* a = 1 reloaded after a DOS program, 2 it stayed
                          (its windows other than the main one closed) */
#define EV_PAINT   2
#define EV_KEY     3   /* a = BIOS key (AH scan, AL char)            */
#define EV_ACTION  4   /* a = control id 0..49                        */
#define EV_MOUSE   5   /* a = kind, b = x, c = y in the client area   */
#define EV_POLL    6   /* 1 whole-window repaint, 2 busy, 3 queued damage only */
#define EV_CLOSE   7   /* return 1 to keep the window open            */
#define EV_SUSPEND 8   /* save state in APP_ARG; return 1 to stay     */
#define EV_PAINT_TOPBAR 9 /* desktop indicators only, for VGA repaint */
#define EV_WHEEL   11  /* a: signed steps (down positive), b/c: client coordinates */
#define EV_TOPBAR  10  /* desktop: a = 1 the CiukiOS menu, 2 the volume  */

#define MOUSE_DOWN  1
#define MOUSE_MOVE  2
#define MOUSE_UP    3
#define MOUSE_RIGHT 4
#define MOUSE_HOVER 6   /* the pointer moved without a button */

#define TITLE_H 30     /* client area starts below the title rail     */

/* Window ids of the desktop (shell_gui_windows.inc). */
#define WIN_PROGRAMS 0
#define WIN_RUN      1
#define WIN_ABOUT    2
#define WIN_FILES    8
#define WIN_TASKS    9
#define WIN_DOS      11
#define WIN_NOTEPAD  12
#define WIN_CONTROL  13
#define WIN_DEVICES  14
#define WIN_RECYCLE  15
#define WIN_PAINT    16
#define WIN_BROWSER  17
#define WIN_DISPLAY  18
#define WIN_DESKTOP  0xFE   /* the desktop module's surface      */
#define WIN_OVERLAY  0xFD   /* its menus, above every window     */

/* Palette: 0 ink, 1 active title, 3 desktop, 7 face, 8 shadow, 15 paper. */
#define C_INK    0
#define C_TITLE  1
#define C_GREEN  2
#define C_TEAL   3
#define C_RED    4
#define C_PURPLE 5
#define C_BROWN  6
#define C_FACE   7
#define C_SHADOW 8
#define C_BLUE   9
#define C_CYAN   10
#define C_LIGHT  11
#define C_ROSE   12
#define C_LILAC  13
#define C_YELLOW 14
#define C_PAPER  15
#define BOLD     0x100
#define ALTFONT  0x200   /* the alternate font (app_font slot 1) */

struct host {
    int x, y, w, h;          /* window, frame included */
    int mx, my, buttons;     /* pointer (screen) and buttons */
    int shift;               /* BIOS shift flags (INT 16h AH=2) */
    int active;              /* 1 when this window has the focus */
    unsigned ticks;          /* BIOS tick count, low word */
    int screen_w, screen_h;
    int window;              /* the window of this event (a WIN_* id or one
                                from win_open)                            */
    int context;             /* desktop module, right click: 0 background,
                                1xxh title bar of window xx, 2xxh shell
                                window xx, else the shell's hit code      */
    int dblclick;            /* double-click time in BIOS ticks */
};
#define HOST (*(struct host *)0x116)
#define APP_ARG ((char *)0x140)
#define APP_ARG_BYTES 128
#define HDR_WIDTH  (*(int *)0x10E)
#define HDR_HEIGHT (*(int *)0x110)

#define SH_RSHIFT 1
#define SH_LSHIFT 2
#define SH_SHIFT  3
#define SH_CTRL   4
#define SH_ALT    8

extern char app_title[64];
int app_event(int ev, int a, int b, int c);

/* ---- shell services ---- */
void ui_rect(int x, int y, int w, int h, int color);
void ui_text(int x, int y, const char *s, int color);
void ui_mono(int x, int y, const char *s, int color, int cell);  /* fixed pitch */
void ui_bevel(int x, int y, int w, int h, int fill);
void ui_inset(int x, int y, int w, int h);
void ui_button(int x, int y, int w, int h, const char *label, int id);
void ui_hit(int x, int y, int w, int h, int id);
void ui_icon(int x, int y, int icon);                   /* 32x32 */
int  ui_measure(const char *s);
void ui_repaint(void);
void ui_repaint_win(int window);                        /* another window */
int app_helper(const char *command);                    /* trusted console utility, keep graphics */
void app_command(const char *command);                  /* run a program */
void app_open(int window, const char *arg);             /* open a module */
void app_close(void);
void app_sound(int event);                              /* 4 notice, 5 error */
int  app_windows(char *buffer);                         /* 25 bytes each */
void app_window_cmd(int window, int command);           /* 1 raise 2 close 3 min
                                                           4 max/restore 5 hide */
int  app_idle(void);                                    /* idle % last second */
/* Windows of their own: x, y (-1: centred), size with the frame, title kept
 * in the module's memory, style WS_*. Returns the window id, 0 if none. */
#define WS_DIALOG 1                                     /* close box only */
int  win_open(int x, int y, int w, int h, const char *title, int style);
void win_close(int window);
void ui_damage(int x, int y, int w, int h);             /* paint this again */
void shell_action(int code);                            /* a desktop action */
void ui_overlay(int on);                                /* desktop menus */
int  app_font(const char *path, int slot);              /* 0 ok; slot 1 alt */
void app_settings(void);                                /* DESKTOP.CFG again */
/* 8-bit palette indices at seg:off (rows of stride bytes), w x h pixels
 * drawn at x, y, each zoom x zoom (1-15). Indices 0-15 are the desktop
 * colours, 80-143 the icon colours (VBE; VGA shows the nearest of 16). */
void ui_bitmap(int x, int y, int w, int h, unsigned seg, unsigned off, int stride, int zoom);
unsigned fs_changes(void);                               /* files changed: a count */
void ui_palette(const unsigned char *colors48);                  /* show 16 colours now */
/* Current compositor band during EV_PAINT. Returns 0 when no VBE band is
 * active. The VM presenter writes directly into this owned band. */
int app_band_info(void *out19);
int app_desktop_focus(void);
/* Display module only: queue mode, or mode=0 query after deferred switch. */
int app_display_mode(unsigned mode);

#define ICON_COMPUTER 0
#define ICON_FOLDER   1
#define ICON_EDITOR   2
#define ICON_DISK     3
#define ICON_DOS      4
#define ICON_PROGRAM  5
#define ICON_GAMES    7
#define ICON_FLOPPY   10
#define ICON_ABOUT    11
#define ICON_USB      15
#define ICON_CD       16
#define ICON_DISPLAY  8
#define ICON_SOUND    9
#define ICON_POWER    12
#define ICON_WINDOWS  13
#define ICON_DESKTOP  14
#define ICON_INSTALL  17
#define ICON_BIN      18
#define ICON_BIN_FULL 19
#define ICON_CONTROL  20
#define ICON_THEME    21
#define ICON_FONTS    22
#define ICON_MOUSE    23
#define ICON_KEYBOARD 24
#define ICON_CALENDAR 25
#define ICON_DEVICES  26
#define ICON_PACKAGE  27
#define ICON_WALLPAPER 28
#define ICON_TEXT     29
#define ICON_AUDIO    30
#define ICON_FONTFILE 31
#define ICON_NETWORK  32
#define ICON_MONITOR  33
#define ICON_PAINT    34

/* ---- runtime (rt.c) ---- */
typedef unsigned char u8;
typedef unsigned int u16;
typedef unsigned long u32;

int  str_len(const char *s);
void str_copy(char *d, const char *s);
void str_ncopy(char *d, const char *s, int n);          /* n includes NUL */
void str_cat(char *d, const char *s);
int  str_cmp(const char *a, const char *b);
int  str_icmp(const char *a, const char *b);
int  str_nicmp(const char *a, const char *b, int n);
char to_upper(char c);
char to_lower(char c);
void mem_copy(void *d, const void *s, int n);
void mem_move(void *d, const void *s, int n);
void mem_set(void *d, int v, int n);
int  mem_cmp(const void *a, const void *b, int n);
void fmt_u32(char *d, u32 v);                           /* decimal */
void fmt_u32_group(char *d, u32 v);                     /* 1,234,567 */
void fmt_hex4(char *d, u16 v);
void fmt_2(char *d, int v);                             /* two digits */
u16  parse_u16(const char *s);
void fmt_size(char *d, u32 bytes);                      /* "12 KB" */
void fmt_date(char *d, u16 fdate);                      /* DD/MM/YYYY */
void fmt_time(char *d, u16 ftime);                      /* HH:MM */

/* Registers for intr (a software interrupt) and far_regs (a far call, as
 * CVSESSION's entry): in and out; cxh/dxh are the high words of ECX/EDX
 * after far_regs. Both return the carry flag. */
struct regs { u16 ax, bx, cx, dx, si, di, ds, es, flags, cxh, dxh, axh; };
int intr(int n, struct regs *r);
int far_regs(u16 seg, u16 off, struct regs *r);

/* DOS (INT 21h). Results < 0 are -error codes. */
#pragma pack(push, 1)
struct dos_find {
    u8 reserved[21];
    u8 attr;
    u16 time, date;
    u32 size;
    char name[13];
};
#pragma pack(pop)
#define A_RDONLY 1
#define A_HIDDEN 2
#define A_SYSTEM 4
#define A_VOLUME 8
#define A_DIR    16
#define A_ARCH   32
void dos_set_dta(void *dta);
int  dos_find_first(const char *pattern, int attr);      /* 8.3 names */
int  dos_find_next(void);
/* Long file names (Windows 95 INT 21h AX=71xxh, served by \SYSTEM\LFN.COM).
 * The dos_* calls on paths use them when LFN.COM is resident and fall back
 * to the 8.3 calls otherwise; paths may then hold long names anywhere. */
#define LFN_NAME 256                                    /* 255 + NUL */
int  dos_lfn(void);                                     /* 1: long names */
int  dos_short_path(const char *path, char *out);       /* out: SYS_PATH */
int  dos_long_path(const char *path, char *out);
int  valid_file_name(const char *name);                 /* Windows' rules */
/* A folder listing: dir_first opens a search (long names, and they nest),
 * dir_next goes on (it closes the search at its end), dir_close ends it
 * early. name is the long name (the 8.3 one without LFN.COM), alias the
 * 8.3 name when it differs, else "". */
struct dir_ent {
    u8 attr;
    u16 time, date;
    u32 size;
    int h;
    char alias[13];
    char name[LFN_NAME];
};
int  dir_first(const char *pattern, int attr, struct dir_ent *e);   /* 0 or -error */
int  dir_next(struct dir_ent *e);
void dir_close(struct dir_ent *e);
int  dos_open(const char *path, int mode);
int  dos_create(const char *path);
int  dos_create_new(const char *path);                  /* fails if exists */
int  dos_close(int h);
int  dos_read(int h, void *buf, int n);
int  dos_write(int h, const void *buf, int n);
int  dos_read_far(int h, u16 seg, u16 off, int n);
int  dos_write_far(int h, u16 seg, u16 off, int n);
long dos_seek(int h, long pos, int whence);
int  dos_mkdir(const char *path);
int  dos_rmdir(const char *path);
int  dos_delete(const char *path);
int  dos_rename(const char *from, const char *to);
int  dos_get_attr(const char *path);
int  dos_set_attr(const char *path, int attr);
int  dos_get_drive(void);                               /* 0 = A */
void dos_set_drive(int drive);
int  dos_chdir(const char *path);
int  dos_getcwd(int drive, char *buf);                  /* drive 0 = current */
int  dos_file_time(int h, u16 *time, u16 *date);
int  dos_set_file_time(int h, u16 time, u16 date);
long dos_disk_free(int drive, long *total);             /* drive 1 = A */
void dos_get_date(int *y, int *m, int *d, int *wd);
void dos_get_time(int *h, int *m, int *s);
u16  dos_alloc(u16 paras);                              /* 0 on failure */
void dos_free(u16 seg);
u16  dos_largest(void);
u16  dos_psp(void);
int  bios_printer(int port, int c);                     /* status */
u8   peek8(u16 seg, u16 off);
u16  peek16(u16 seg, u16 off);
void poke8(u16 seg, u16 off, u8 v);
void far_copy(u16 dseg, u16 doff, u16 sseg, u16 soff, u16 n);
void app_log(const char *what, const char *detail);     /* COM1 line */
u16  app_seg(void);                                     /* this module's segment */
int  cpu_vendor(char *out13);                           /* CPUID: family */
void far_call_req(u16 seg, u16 off, void *req);         /* ES:DI = req */
const char *dos_error_text(int error);

/* ---- toolkit (ui.c) ---- */
struct menu_item { const char *label; const char *key; int id; int flags; };
#define MI_SEP      1
#define MI_DISABLED 2
#define MI_CHECKED  4

struct menu {
    const char *title;
    struct menu_item *items;
    int count;
};

/* A popup (context or pull-down) menu: open at x,y, handled by ui.c. */
struct popup {
    int open, x, y, w, h;
    struct menu_item *items;
    int count, hover;
};
void popup_open(struct popup *p, struct menu_item *items, int count,
                int x, int y, int max_x, int max_y);
void popup_draw(struct popup *p);
/* Mouse/key -> -1 still open, -2 closed, >= 0 chosen id. */
int  popup_mouse(struct popup *p, int kind, int sx, int sy);
int  popup_key(struct popup *p, int key);

struct menubar {
    struct menu *menus;
    int count;
    int x, y, w;             /* screen position of the bar */
    int active;              /* -1 none, else open menu */
    int keyboard;            /* F10/Alt: bar focused without a menu open */
    struct popup pop;
};
#define MENUBAR_H 20
void menubar_draw(struct menubar *m);
int  menubar_mouse(struct menubar *m, int kind, int sx, int sy);
int  menubar_key(struct menubar *m, int key, int shift);
int  menubar_open(struct menubar *m);

void draw_frame_text(int x, int y, int w, const char *s, int color);  /* clipped */
int  text_fit(const char *s, int w, char *out);
void draw_scroll(int x, int y, int h, long pos, long total, long page);
long scroll_hit(int x, int y, int h, int my, long total, long page);
void draw_check(int x, int y, int on);
void draw_radio(int x, int y, int on);
void draw_focus(int x, int y, int w, int h);

/* Single-line edit field. */
struct field {
    char *text;
    int max, len, cursor, sel, scroll;
};
void field_set(struct field *f, char *buffer, int max, const char *init);
int  field_key(struct field *f, int key, int shift);     /* 1 changed */
void field_draw(struct field *f, int x, int y, int w, int focused);
void field_click(struct field *f, int x, int w, int mx);

/* Modal dialogs drawn inside the window. Control coordinates are relative
 * to the dialog's client area (below its title bar). */
#define DC_LABEL  1
#define DC_FIELD  2
#define DC_CHECK  3
#define DC_RADIO  4
#define DC_BUTTON 5
#define DC_GROUP  6   /* etched frame with a caption */
struct dctl {
    int type, x, y, w, h;
    const char *text;          /* label; '&' marks the mnemonic */
    int id;                    /* button: its result; others: free */
    int value;                 /* check/radio state */
    struct field *field;
    int group;                 /* radio group */
    int disabled;
};
struct dialog {
    int open;
    const char *title;
    int x, y, w, h;
    struct dctl *c;
    int n, focus;
    int def_id, cancel_id;     /* Enter and Esc results */
    int win, ww, wh;           /* its own window (dialog_sync) */
    int hot;                   /* the button under the pointer, or -1 */
    int modeless;              /* its owner stays usable (Properties) */
};
#define DIALOG_TITLE_H 22
void dialog_show(struct dialog *d, const char *title, struct dctl *c, int n,
                 int w, int h, int def_id, int cancel_id);
void dialog_draw(struct dialog *d);
int  dialog_mouse(struct dialog *d, int kind, int sx, int sy);   /* id or -1 */
int  dialog_key(struct dialog *d, int key, int shift);           /* id or -1 */
int  dialog_value(struct dialog *d, int index);
/* Each dialog is a window of its own. After every event the module calls
 * dialog_sync: it opens the window of a dialog just shown, and closes the
 * window of one that ended. dialog_mine: this event is for its window (on
 * CLOSE, from the close box, the window is gone: treat it as Esc). */
void dialog_sync(struct dialog *d);
int  dialog_mine(struct dialog *d);
void dialog_place(struct dialog *d);                    /* follow its window */
/* Hover (MOUSE_HOVER): ui_dirty tells whether a highlight changed. */
extern int ui_dirty;
int  dialog_hover(struct dialog *d, int sx, int sy);    /* 1 repaint */
int  dialog_pre(struct dialog *d, int main_win, int *ev, int *a);
int  dialog_post(struct dialog *d, int main_win, int ev, int r);

/* Message box: text lines split at '\n'; buttons "OK", "Yes|No|Cancel"... */
#define MB_OK 1
#define MB_YES 2
#define MB_NO 3
#define MB_CANCEL 4
#define MB_SAVE 5
#define MB_DONTSAVE 6
#define MB_YESALL 7
void msgbox(struct dialog *d, const char *title, const char *text, const char *buttons);

/* Keys (BIOS INT 16h AH=10h). */
#define K_ENTER   0x1C0D
#define K_ESC     0x011B
#define K_TAB     0x0F09
#define K_BACK    0x0E08
#define K_UP      0x48
#define K_DOWN    0x50
#define K_LEFT    0x4B
#define K_RIGHT   0x4D
#define K_HOME    0x47
#define K_END     0x4F
#define K_PGUP    0x49
#define K_PGDN    0x51
#define K_INS     0x52
#define K_DEL     0x53
#define K_F1      0x3B
#define K_F2      0x3C
#define K_F3      0x3D
#define K_F5      0x3F
#define K_F10     0x44
#define K_SHIFT_F10 0x5D
#define KEY_SCAN(k) (((unsigned)(k) >> 8) & 0xFF)
#define KEY_CHAR(k) ((k) & 0xFF)
int key_ctrl_letter(int key);    /* 'A'..'Z' for Ctrl+letter, else 0 */
int key_alt_letter(int key);     /* 'A'..'Z' for Alt+letter, else 0 */

/* ---- system services shared by the desktop modules (sys.c) ---- */
#define SYS_PATH 260                                   /* Windows' MAX_PATH */
extern u32 sys_bytes;
int  tree_delete(const char *path);                     /* file or folder */
int  tree_move(const char *src, const char *dst);       /* same drive */
int  tree_copy(const char *src, const char *dst, char *buf, int n);
void tree_measure(const char *path, int *files, int *dirs, u32 *bytes);
int  make_dirs(const char *path);
/* The file clipboard, \SYSTEM\UI\CLIPBRD.DAT: Files and the desktop. */
int  clip_count(int *cut);
int  clip_get(int i, char *path);                       /* its medium */
void clip_begin(int cut);
void clip_add(const char *path, int media);
void clip_end(void);
void clip_clear(void);
/* The Recycle Bin: X:\RECYCLED (DXn.ext and INFO2.DAT, 320-byte records
 * with long paths; the 128-byte records of an older INFO.DAT are read and
 * moved to INFO2.DAT at the first change). */
#pragma pack(push, 1)
struct binrec {
    char orig[SYS_PATH];       /* where it was (long path) */
    char stored[13];           /* its name in the bin */
    u8 attr;
    u16 date, time;            /* modified */
    u32 size;
    u16 ddate, dtime;          /* deleted */
    u8 used;
    u8 pad[33];
};
#pragma pack(pop)
int  bin_send(const char *path);                        /* 0 or -error */
int  bin_count(void);                                   /* records */
int  bin_get(int i, struct binrec *r);
int  bin_item(int i, struct binrec *r);                 /* 1: in the bin */
int  bin_items(u32 *bytes);
void bin_stored(const struct binrec *r, char *out);
int  bin_restore(int i);                                /* -80: exists */
int  bin_purge(int i);
int  bin_empty(void);
/* \SYSTEM\UI\DESKTOP.CFG (72 bytes), read by the shell at startup. */
#pragma pack(push, 1)
struct deskcfg {
    char magic[4];             /* CUI1 */
    u8 palette[48];            /* 16 colours, 6-bit RGB */
    u8 mouse_speed;            /* 1..4 */
    u8 mouse_swap;
    u8 dblclick;               /* ticks */
    u8 kbd_rate;               /* INT 16h AX=0305h: BL (0 fastest..31) */
    u8 kbd_delay;              /* BH (0..3: 250..1000 ms) */
    char font[13];             /* \SYSTEM\FONTS\name, "" the built-in */
    u16 icons_hidden;          /* desktop icons, bit per DI_* */
};
#pragma pack(pop)
extern const u8 cfg_default_palette[48];
void cfg_defaults(struct deskcfg *c);
int  cfg_load(struct deskcfg *c);                       /* 1 from the file */
int  cfg_save(struct deskcfg *c);                       /* and applies it */
#define DI_COMPUTER 1
#define DI_PROGRAMS 2
#define DI_BIN      4
#define DI_CONTROL  8
#define DI_DOS      16
#define DI_FLOPPY   32
#define DI_USB      64
#define DI_CD       128

#endif
