/* CiukiOS welcome and About page. The portrait icon is the deterministic
 * runtime conversion of the owner's approved Ciuki portrait. */
#include "app.h"

static const char welcome_path[] = "\\SYSTEM\\UI\\WELCOME.CFG";
static int dont_show_at_startup;
static int page;
static int focus = 4;
static int X, Y, W, H;
static char status[80];

static void load_preference(void)
{
    char value[2];
    int h = dos_open(welcome_path, 0), n;
    dont_show_at_startup = 0;
    if (h < 0) return;
    n = dos_read(h, value, sizeof value);
    dos_close(h);
    if (n == 1 && (value[0] == '0' || value[0] == '1'))
        /* Keep WELCOME.CFG compatible: its byte records show (1) or hide (0). */
        dont_show_at_startup = value[0] == '0';
}

static int save_preference(void)
{
    char value = dont_show_at_startup ? '0' : '1';
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
    static const char *tabs[] = { "About", "Credits" };
    int check_y, button_y, i, tab_x, tab_y;
    layout();
    ui_rect(X, Y, W, H, C_FACE);

    /* Keep the approved portrait and project identity in the shared header. */
    ui_bevel(X + 12, Y + 8, W - 24, 72, C_TITLE);
    ui_rect(X + 14, Y + 10, W - 28, 68, C_TITLE);
    ui_icon(X + 22, Y + 28, ICON_ABOUT);
    ui_text(X + 64, Y + 15, "Welcome to CiukiOS", C_PAPER | BOLD);
    ui_text(X + 64, Y + 33, "Version 0.8.3", C_PAPER);
    ui_text(X + 64, Y + 51, "A modern Retro OS", C_PAPER);

    /* Two related, labeled pages keep the dedication distinct from credits. */
    tab_y = Y + 88;
    for (i = 0; i < 2; ++i) {
        tab_x = X + 8 + i * 112;
        ui_bevel(tab_x, tab_y + (i == page ? 0 : 3), 108,
                 i == page ? 27 : 24, C_FACE);
        ui_text(tab_x + 18, tab_y + 8, tabs[i], C_INK | (i == page ? BOLD : 0));
        ui_hit(tab_x, tab_y, 108, 27, 4 + i);
        if (focus == i) draw_focus(tab_x, tab_y, 108, 27);
    }
    ui_rect(X + 8, tab_y + 27, W - 16, 1, C_PAPER);

    if (page == 0) {
        draw_frame_text(X + 20, Y + 128, W - 40,
                        "Dedicated to Ciuki", C_INK | BOLD);
        draw_frame_text(X + 20, Y + 156, W - 40,
                        "In loving memory of Ciuk (Ciuki),", C_INK);
        draw_frame_text(X + 20, Y + 172, W - 40,
                        "the beloved dog behind CiukiOS. Though", C_INK);
        draw_frame_text(X + 20, Y + 188, W - 40,
                        "you are no longer here, your memory lives", C_INK);
        draw_frame_text(X + 20, Y + 204, W - 40,
                        "on every time this system starts.", C_INK);
    } else {
        draw_frame_text(X + 20, Y + 128, W - 40,
                        "System and credits", C_INK | BOLD);
        draw_frame_text(X + 20, Y + 148, W - 40,
                        "Core: CiukiDOS kernel and desktop", C_INK);
        draw_frame_text(X + 20, Y + 164, W - 40,
                        "DOS VMs: Jemm386, CVSESSION and HDPMI", C_INK);
        draw_frame_text(X + 20, Y + 180, W - 40,
                        "Input: CuteMouse | Audio: SBEMU and VSBHDA", C_INK);
        draw_frame_text(X + 20, Y + 196, W - 40,
                        "Graphics: TinyGL software OpenGL | Build: Open Watcom", C_INK);
        draw_frame_text(X + 20, Y + 224, W - 40,
                        "Copyright (c) 2026 Alberto Lopez / Alcybercloud.it", C_INK);
        draw_frame_text(X + 20, Y + 240, W - 40,
                        "Tango Icon Theme 0.8.90: Public Domain", C_INK);
        draw_frame_text(X + 20, Y + 256, W - 40,
                        "CiukiOS code: GNU GPL v2. See LICENSE and README.", C_INK);
    }

    draw_frame_text(X + 20, Y + H - 98, W - 40,
                    status[0] ? status : "F3 Run   Ctrl+Esc Programs   F4 Full-screen DOS", C_SHADOW);
    check_y = Y + H - 78;
    draw_check(X + 20, check_y + 4, dont_show_at_startup);
    ui_text(X + 42, check_y + 3, "Don't show this at startup", C_INK);
    ui_hit(X + 16, check_y, 250, 24, 1);
    if (focus == 2) draw_focus(X + 16, check_y, 250, 24);

    button_y = Y + H - 40;
    ui_button(X + W - 259, button_y, 155, 28, "Application Library", 2);
    ui_button(X + W - 96, button_y, 78, 28, "Continue", 3);
    if (focus == 3) draw_focus(X + W - 261, button_y - 1, 159, 30);
    if (focus == 4) draw_focus(X + W - 98, button_y - 1, 82, 30);
}

static void toggle_startup(void)
{
    int old = dont_show_at_startup;
    dont_show_at_startup = !dont_show_at_startup;
    if (save_preference()) {
        dont_show_at_startup = old;
        str_copy(status, "Could not save the startup preference.");
        app_sound(5);
        app_log("[ABOUT] preference error", "WELCOME.CFG");
        return;
    }
    str_copy(status, dont_show_at_startup ? "This page will not appear at startup." :
                                           "This page will appear at startup.");
    app_log("[ABOUT] show at startup", dont_show_at_startup ? "0" : "1");
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
        else if (a == 4 || a == 5) { page = a - 4; focus = page; }
        return 1;
    case EV_KEY:
        ch = KEY_CHAR(a);
        if (ch == 27) { app_close(); return 1; }
        if (ch == '1' || ch == '2') { page = ch - '1'; focus = page; return 1; }
        if (ch == 9) { focus = (focus + 1) % 5; return 1; }
        if (ch == ' ' && focus == 2) { toggle_startup(); return 1; }
        if (ch == 13) {
            if (focus == 0 || focus == 1) page = focus;
            else if (focus == 2) toggle_startup();
            else if (focus == 3) shell_action(1);
            else app_close();
            return 1;
        }
        if (KEY_SCAN(a) == K_LEFT || KEY_SCAN(a) == K_UP) {
            if (focus < 2) { page = 0; focus = 0; }
            else focus = focus ? focus - 1 : 4;
            return 1;
        }
        if (KEY_SCAN(a) == K_RIGHT || KEY_SCAN(a) == K_DOWN) {
            if (focus < 2) { page = 1; focus = 1; }
            else focus = (focus + 1) % 5;
            return 1;
        }
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
