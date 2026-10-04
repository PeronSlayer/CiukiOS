/* CiukiOS welcome and About page. The portrait icon is the deterministic
 * runtime conversion of the owner's approved Ciuki portrait. */
#include "app.h"

static const char welcome_path[] = "\\SYSTEM\\UI\\WELCOME.CFG";
static int show_at_startup = 1;
static int focus = 2;
static int X, Y, W, H;
static char status[80];

static void load_preference(void)
{
    char value[2];
    int h = dos_open(welcome_path, 0), n;
    show_at_startup = 1;
    if (h < 0) return;
    n = dos_read(h, value, sizeof value);
    dos_close(h);
    if (n == 1 && (value[0] == '0' || value[0] == '1'))
        show_at_startup = value[0] == '1';
}

static int save_preference(void)
{
    char value = show_at_startup ? '1' : '0';
    int h = dos_create(welcome_path), n;
    if (h < 0) return h;
    n = dos_write(h, &value, 1);
    dos_close(h);
    return n == 1 ? 0 : -1;
}

static void layout(void)
{
    X = HOST.x + 6;
    Y = HOST.y + TITLE_H;
    W = HOST.w - 12;
    H = HOST.h - TITLE_H - 6;
}

static void paint(void)
{
    int check_y, button_y;
    layout();
    ui_rect(X, Y, W, H, C_FACE);

    /* Compact welcome banner; the following 16px credit rows end before
     * H-100 even in the minimum-height 640x480 window. */
    ui_bevel(X + 12, Y + 8, W - 24, 72, C_TITLE);
    ui_rect(X + 14, Y + 10, W - 28, 68, C_TITLE);
    ui_icon(X + 22, Y + 28, ICON_ABOUT);
    ui_text(X + 64, Y + 15, "Welcome to CiukiOS", C_PAPER | BOLD);
    ui_text(X + 64, Y + 33, "Version 0.8.3", C_PAPER);
    ui_text(X + 64, Y + 51, "A modern Retro OS", C_PAPER);

    draw_frame_text(X + 20, Y + 88, W - 40,
                    "Core: CiukiDOS kernel and desktop", C_INK);
    draw_frame_text(X + 20, Y + 104, W - 40,
                    "DOS VMs: Jemm386, CVSESSION and HDPMI", C_INK);
    draw_frame_text(X + 20, Y + 120, W - 40,
                    "Input: CuteMouse | Audio: SBEMU and VSBHDA", C_INK);
    draw_frame_text(X + 20, Y + 136, W - 40,
                    "Graphics: TinyGL software OpenGL subset", C_INK);
    draw_frame_text(X + 20, Y + 152, W - 40,
                    "Icons: Tango Icon Theme 0.8.90 (Public Domain)", C_INK);
    draw_frame_text(X + 20, Y + 168, W - 40,
                    "Build: Open Watcom", C_INK);
    draw_frame_text(X + 20, Y + 184, W - 40,
                    "Copyright (c) 2026 Alberto Lopez / Alcybercloud.it", C_INK);
    draw_frame_text(X + 20, Y + 200, W - 40,
                    "In loving memory of Ciuk (Ciuki), the beloved dog", C_INK);
    draw_frame_text(X + 20, Y + 216, W - 40,
                    "behind CiukiOS. Though you are no longer here,", C_INK);
    draw_frame_text(X + 20, Y + 232, W - 40,
                    "your memory lives on every time this system starts.", C_INK);
    draw_frame_text(X + 20, Y + 248, W - 40,
                    "CiukiOS code: GNU GPL v2. See LICENSE and README.", C_INK);

    draw_frame_text(X + 20, Y + H - 98, W - 40,
                    status[0] ? status : "F3 Run   Ctrl+Esc Programs   F4 Full-screen DOS", C_SHADOW);
    check_y = Y + H - 78;
    draw_check(X + 20, check_y + 4, show_at_startup);
    ui_text(X + 42, check_y + 3, "Show this page at startup", C_INK);
    ui_hit(X + 16, check_y, 250, 24, 1);
    if (focus == 0) draw_focus(X + 16, check_y, 250, 24);

    button_y = Y + H - 40;
    ui_button(X + W - 259, button_y, 155, 28, "Application Library", 2);
    ui_button(X + W - 96, button_y, 78, 28, "Continue", 3);
    if (focus == 1) draw_focus(X + W - 261, button_y - 1, 159, 30);
    if (focus == 2) draw_focus(X + W - 98, button_y - 1, 82, 30);
}

static void toggle_startup(void)
{
    int old = show_at_startup;
    show_at_startup = !show_at_startup;
    if (save_preference()) {
        show_at_startup = old;
        str_copy(status, "Could not save the startup preference.");
        app_sound(5);
        app_log("[ABOUT] preference error", "WELCOME.CFG");
        return;
    }
    str_copy(status, show_at_startup ? "This page will appear at startup." :
                                      "This page will not appear at startup.");
    app_log("[ABOUT] show at startup", show_at_startup ? "1" : "0");
}

static int event(int ev, int a)
{
    int ch;
    switch (ev) {
    case EV_OPEN:
        str_copy(app_title, "About CiukiOS");
        if (!HDR_WIDTH) {
            HDR_WIDTH = HOST.screen_w - 48;
            if (HDR_WIDTH > 620) HDR_WIDTH = 620;
            if (HDR_WIDTH < 420) HDR_WIDTH = 420;
            HDR_HEIGHT = HOST.screen_h - 40;
            if (HDR_HEIGHT > 440) HDR_HEIGHT = 440;
            if (HDR_HEIGHT < 400) HDR_HEIGHT = 400;
        }
        load_preference();
        status[0] = 0;
        app_log("[ABOUT] open", "CiukiOS 0.8.3");
        return 1;
    case EV_PAINT:
        paint();
        return 0;
    case EV_ACTION:
        if (a == 1) toggle_startup();
        else if (a == 2) shell_action(1);
        else if (a == 3) app_close();
        return 1;
    case EV_KEY:
        ch = KEY_CHAR(a);
        if (ch == 27) { app_close(); return 1; }
        if (ch == 9) { focus = (focus + 1) % 3; return 1; }
        if (ch == ' ' && focus == 0) { toggle_startup(); return 1; }
        if (ch == 13) {
            if (focus == 0) toggle_startup();
            else if (focus == 1) shell_action(1);
            else app_close();
            return 1;
        }
        if (KEY_SCAN(a) == K_LEFT || KEY_SCAN(a) == K_UP) { focus = focus ? focus - 1 : 2; return 1; }
        if (KEY_SCAN(a) == K_RIGHT || KEY_SCAN(a) == K_DOWN) { focus = (focus + 1) % 3; return 1; }
        return 0;
    case EV_POLL:
        return 0;
    case EV_CLOSE:
        return 0;
    case EV_SUSPEND:
        return 0;
    }
    return 0;
}

int app_event(int ev, int a, int b, int c)
{
    (void)b;
    (void)c;
    return event(ev, a);
}
