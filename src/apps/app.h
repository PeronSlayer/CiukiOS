/* CiukiOS desktop application modules (Files, Tasks, Notepad).
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

#define EV_OPEN    1   /* a = 1 when reopened after a DOS program */
#define EV_PAINT   2
#define EV_KEY     3   /* a = BIOS key (AH scan, AL char)            */
#define EV_ACTION  4   /* a = control id 0..49                        */
#define EV_MOUSE   5   /* a = kind, b = x, c = y in the client area   */
#define EV_POLL    6   /* return 1 repaint, 2 busy (no HLT)           */
#define EV_CLOSE   7   /* return 1 to keep the window open            */
#define EV_SUSPEND 8   /* save state in APP_ARG; return 1 to stay     */

#define MOUSE_DOWN  1
#define MOUSE_MOVE  2
#define MOUSE_UP    3
#define MOUSE_RIGHT 4

#define TITLE_H 30     /* client area starts below the title rail     */

/* Window ids of the desktop (shell_gui_windows.inc). */
#define WIN_PROGRAMS 0
#define WIN_RUN      1
#define WIN_FILES    8
#define WIN_TASKS    9
#define WIN_DOS      11
#define WIN_NOTEPAD  12

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

struct host {
    int x, y, w, h;          /* window, frame included */
    int mx, my, buttons;     /* pointer (screen) and buttons */
    int shift;               /* BIOS shift flags (INT 16h AH=2) */
    int active;              /* 1 when this window has the focus */
    unsigned ticks;          /* BIOS tick count, low word */
    int screen_w, screen_h;
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
void app_command(const char *command);                  /* run a program */
void app_open(int window, const char *arg);             /* open a module */
void app_close(void);
void app_sound(int event);                              /* 4 notice, 5 error */
int  app_windows(char *buffer);                         /* 25 bytes each */
void app_window_cmd(int window, int command);           /* 1 raise 2 close 3 min */
int  app_idle(void);                                    /* idle % last second */

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
struct regs { u16 ax, bx, cx, dx, si, di, ds, es, flags, cxh, dxh; };
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
int  dos_find_first(const char *pattern, int attr);
int  dos_find_next(void);
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
};
#define DIALOG_TITLE_H 22
void dialog_show(struct dialog *d, const char *title, struct dctl *c, int n,
                 int w, int h, int def_id, int cancel_id);
void dialog_draw(struct dialog *d);
int  dialog_mouse(struct dialog *d, int kind, int sx, int sy);   /* id or -1 */
int  dialog_key(struct dialog *d, int key, int shift);           /* id or -1 */
int  dialog_value(struct dialog *d, int index);

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

#endif
