/* Run uses the shared editor, including range selection and word selection. */
#include "app.h"

char run_command[97];
struct field run_input;
static int focus;
static unsigned caret_tick;
static const struct alias { const char *name; int window; } aliases[] = {
    {"CIUKNOTE", WIN_NOTEPAD}, {"NOTEPAD", WIN_NOTEPAD},
    {"CIUKPAINT", WIN_PAINT}, {"MSPAINT", WIN_PAINT}, {"PAINT", WIN_PAINT},
    {"EXPLORER", WIN_FILES}, {"FILES", WIN_FILES}, {"TASKMGR", WIN_TASKS},
    {"CONTROL", WIN_CONTROL}, {"DEVMGMT", WIN_DEVICES}, {"DISPLAY", WIN_DISPLAY},
    {"CIUKWEB", WIN_BROWSER}, {"BROWSER", WIN_BROWSER}, {"ABOUT", WIN_ABOUT},
    {"VIEWER", WIN_VIEWER}, {"IMAGE", WIN_VIEWER},
    {"PLAYER", WIN_PLAYER}, {"MUSIC", WIN_PLAYER}
};

static void launch(int fullscreen)
{
    char *p = run_command;
    int i, n;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { focus = 0; return; }
    app_log(fullscreen ? "[RUN] full screen" : "[RUN] launch", p);
    app_window_cmd(WIN_RUN, 2);
    if (fullscreen) { app_command(p); return; }
    for (i = 0; i < (int)(sizeof aliases / sizeof aliases[0]); i++) {
        n = 0;
        while (aliases[i].name[n] && to_upper(p[n]) == aliases[i].name[n]) n++;
        if (aliases[i].name[n] || (p[n] && p[n] != ' ' && p[n] != '\t')) continue;
        p += n;
        while (*p == ' ' || *p == '\t') p++;
        app_open(aliases[i].window, p);
        return;
    }
    app_open(WIN_DOS, p);
}

static void paint(void)
{
    int x = HOST.x, y = HOST.y;
    ui_rect(x + 3, y + TITLE_H, HOST.w - 6, HOST.h - TITLE_H - 3, C_FACE);
    ui_text(x + 22, y + 45, "Enter a program name or a DOS command.", C_INK);
    field_draw(&run_input, x + 22, y + 76, HOST.w - 44, focus == 0 && HOST.active);
    ui_text(x + 22, y + 118, "Examples: dir   |   notepad notes.txt", C_SHADOW);
    ui_text(x + 22, y + 140, "Run: DOS window.  Full screen: the whole screen.", C_INK);
    ui_button(x + 159, y + 188, 96, 26, "Full screen", 42);
    ui_button(x + 263, y + 188, 84, 26, "Run", 40);
    ui_button(x + 355, y + 188, 88, 26, "Cancel", 41);
    if (focus) draw_focus(x + (focus == 1 ? 162 : focus == 2 ? 266 : 358),
                           y + 191, focus == 1 ? 90 : focus == 2 ? 78 : 82, 20);
}

int app_event(int ev, int a, int b, int c)
{
    int x = HOST.x + b, y = HOST.y + TITLE_H + c;
    if (ev == EV_OPEN) {
        if (a == 2) return 1;
        str_copy(app_title, "Run"); HDR_WIDTH = 466; HDR_HEIGHT = 230;
        field_set(&run_input, run_command, sizeof run_command, APP_ARG);
        focus = 0; caret_tick = HOST.ticks / 9; return 1;
    }
    if (ev == EV_PAINT) { paint(); return 0; }
    if (ev == EV_ACTION) {
        if (a == 40) launch(0);
        else if (a == 42) launch(1);
        else if (a == 41) app_close();
        return 1;
    }
    if (ev == EV_KEY) {
        int ch = KEY_CHAR(a);
        if (ch == 27) { app_close(); return 1; }
        if (KEY_SCAN(a) == 0x0F) {
            focus = (focus + ((HOST.shift & SH_SHIFT) ? 3 : 1)) % 4;
            run_input.drag = run_input.click_valid = 0;
            if (!focus) run_input.sel = 1;
            return 1;
        }
        if (ch == 13 || (ch == ' ' && focus)) {
            if (focus == 3) app_close(); else launch(focus == 1);
            return 1;
        }
        return focus == 0 ? field_key(&run_input, a, HOST.shift) : 0;
    }
    if (ev == EV_MOUSE) {
        if ((a == MOUSE_MOVE || a == MOUSE_UP) && run_input.drag)
            return field_mouse(&run_input, a, HOST.x + 22, HOST.w - 44, x, HOST.shift);
        if (x >= HOST.x + 22 && x < HOST.x + HOST.w - 22 &&
            y >= HOST.y + 76 && y < HOST.y + 98) {
            ui_cursor(CURSOR_IBEAM);
            if (a == MOUSE_DOWN) {
                focus = 0;
                return field_mouse(&run_input, a, HOST.x + 22, HOST.w - 44, x, HOST.shift);
            }
        } else ui_cursor(CURSOR_ARROW);
        return 0;
    }
    if (ev == EV_POLL && focus == 0 && HOST.active && caret_tick != HOST.ticks / 9) {
        caret_tick = HOST.ticks / 9; return 1;
    }
    if (ev == EV_SUSPEND) { str_copy(APP_ARG, run_command); return 1; }
    return 0;
}
