/* SPDX-License-Identifier: MIT
 * Calculator window on libdeskui: the Windows 98 standard layout (display,
 * memory indicator, Backspace/CE/C, memory column and 5x4 key grid). */
#include "calc.h"
#include <stdio.h>
#include <string.h>

enum calc_action { A_DIGIT, A_POINT, A_OP, A_EQUALS, A_SIGN, A_PERCENT, A_SQRT, A_RECIP,
                   A_MC, A_MR, A_MS, A_MPLUS, A_BACK, A_CE, A_C };
struct calc_key { const char *label; enum calc_action action; int arg; int x, y, w; };
#define ROW(n) (92 + 32 * (n))
#define COL(n) (52 + 40 * (n))
static const struct calc_key keys[] = {
    {"Backspace", A_BACK, 0, 52, 58, 64}, {"CE", A_CE, 0, 120, 58, 64}, {"C", A_C, 0, 188, 58, 64},
    {"MC", A_MC, 0, 8, ROW(0), 36}, {"7", A_DIGIT, 7, COL(0), ROW(0), 36}, {"8", A_DIGIT, 8, COL(1), ROW(0), 36},
    {"9", A_DIGIT, 9, COL(2), ROW(0), 36}, {"/", A_OP, CALC_DIV, COL(3), ROW(0), 36}, {"sqrt", A_SQRT, 0, COL(4), ROW(0), 36},
    {"MR", A_MR, 0, 8, ROW(1), 36}, {"4", A_DIGIT, 4, COL(0), ROW(1), 36}, {"5", A_DIGIT, 5, COL(1), ROW(1), 36},
    {"6", A_DIGIT, 6, COL(2), ROW(1), 36}, {"*", A_OP, CALC_MUL, COL(3), ROW(1), 36}, {"%", A_PERCENT, 0, COL(4), ROW(1), 36},
    {"MS", A_MS, 0, 8, ROW(2), 36}, {"1", A_DIGIT, 1, COL(0), ROW(2), 36}, {"2", A_DIGIT, 2, COL(1), ROW(2), 36},
    {"3", A_DIGIT, 3, COL(2), ROW(2), 36}, {"-", A_OP, CALC_SUB, COL(3), ROW(2), 36}, {"1/x", A_RECIP, 0, COL(4), ROW(2), 36},
    {"M+", A_MPLUS, 0, 8, ROW(3), 36}, {"0", A_DIGIT, 0, COL(0), ROW(3), 36}, {"+/-", A_SIGN, 0, COL(1), ROW(3), 36},
    {".", A_POINT, 0, COL(2), ROW(3), 36}, {"+", A_OP, CALC_ADD, COL(3), ROW(3), 36}, {"=", A_EQUALS, 0, COL(4), ROW(3), 36},
};
#define KEY_COUNT (sizeof(keys) / sizeof(keys[0]))
static const struct du_rect display_box = {8, 26, 244, 24}, memory_box = {8, 58, 36, 26};

int calc_button_id(const char *label)
{
    for (unsigned i = 0; i < KEY_COUNT; i++) if (!strcmp(keys[i].label, label)) return CALC_B_FIRST + (int)i;
    return -1;
}

static void run(struct calc *c, const struct calc_key *k)
{
    switch (k->action) {
    case A_DIGIT: calc_digit(c, k->arg); break;
    case A_POINT: calc_point(c); break;
    case A_OP: calc_operator(c, (enum calc_op)k->arg); break;
    case A_EQUALS: calc_equals(c); break;
    case A_SIGN: calc_negate(c); break;
    case A_PERCENT: calc_percent(c); break;
    case A_SQRT: calc_sqrt(c); break;
    case A_RECIP: calc_reciprocal(c); break;
    case A_MC: calc_memory_clear(c); break;
    case A_MR: calc_memory_recall(c); break;
    case A_MS: calc_memory_store(c); break;
    case A_MPLUS: calc_memory_add(c); break;
    case A_BACK: calc_backspace(c); break;
    case A_CE: calc_clear_entry(c); break;
    case A_C: calc_clear(c); break;
    }
}

/* Digit grouping ("1,234,567.") for the display only, as Windows' View menu. */
static void grouped(const char *in, char *out, size_t cap)
{
    const char *start = in + (*in == '-'), *end = start;
    while (*end >= '0' && *end <= '9') end++;
    size_t n = 0, digits = (size_t)(end - start);
    if (*in == '-' && n + 1 < cap) out[n++] = '-';
    for (size_t i = 0; i < digits && n + 2 < cap; i++) {
        out[n++] = start[i];
        if ((digits - i - 1) % 3 == 0 && i + 1 < digits) out[n++] = ',';
    }
    snprintf(out + n, cap - n, "%s", end);
}

void calc_paint(struct du_ui *u, void *user)
{
    struct calc_app *a = user; struct du_canvas *c = &u->canvas;
    du_fill(c, display_box, DU_WINDOW);
    du_edge(c, display_box, DU_SUNKEN);
    char text[96];
    if (a->digit_grouping && !a->calc.error) grouped(a->calc.display, text, sizeof(text));
    else snprintf(text, sizeof(text), "%s", a->calc.display);
    int w = du_text_width(text);
    if (w > display_box.w - 12) du_text_fit(c, display_box.x + 6, display_box.y + 9, display_box.w - 12, text, DU_INK);
    else du_text(c, display_box.x + display_box.w - 6 - w, display_box.y + 9, text, DU_INK);
    du_fill(c, memory_box, DU_FACE);
    du_edge(c, memory_box, DU_SUNKEN_THIN);
    if (a->calc.has_memory) du_text(c, memory_box.x + 15, memory_box.y + 10, "M", DU_INK);
}

static const struct du_menu_item edit_template[] = {
    {"&Copy", CALC_M_COPY, 0x2e, DU_CTRL, 0}, {"&Paste", CALC_M_PASTE, 0x2f, DU_CTRL, 0} };
static const struct du_menu_item view_template[] = {
    {"&Standard", CALC_M_STANDARD, 0, 0, DU_ITEM_RADIO | DU_ITEM_CHECKED}, {"&Digit grouping", CALC_M_GROUPING, 0, 0, 0} };
static const struct du_menu_item help_template[] = {
    {"&Help Topics", CALC_M_HELP, DU_KEY_F1, 0, 0}, {"&About Calculator", CALC_M_ABOUT, 0, 0, 0} };

void calc_attach(struct calc_app *a, struct du_ui *u)
{
    a->ui = u; u->event = calc_event; u->user = a; u->paint = calc_paint;
    calc_init(&a->calc);
    memcpy(a->edit_items, edit_template, sizeof(edit_template));
    memcpy(a->view_items, view_template, sizeof(view_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Edit", a->edit_items, 2};
    a->menus[1] = (struct du_menu){"&View", a->view_items, 2};
    a->menus[2] = (struct du_menu){"&Help", a->help_items, 2};
    u->menus = a->menus; u->menu_count = 3;
    for (unsigned i = 0; i < KEY_COUNT; i++)
        du_add(u, DU_BUTTON, CALC_B_FIRST + (int)i, (struct du_rect){keys[i].x, keys[i].y, keys[i].w, i < 3 ? 26 : 28}, keys[i].label);
    u->focus = -1;   /* keys go to the calculator, not to a focused button */
    u->dirty = 1;
}

void calc_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct calc_app *a=user;
    char display[sizeof(a->calc.display)];memcpy(display,a->calc.display,sizeof(display));
    int memory=a->calc.has_memory,grouping=a->digit_grouping;
    switch (e->type) {
    case DU_EV_ACTIVATE:
        if (e->id >= CALC_B_FIRST && e->id < CALC_B_FIRST + (int)KEY_COUNT) { run(&a->calc, &keys[e->id - CALC_B_FIRST]); u->focus = -1; }
        else if (e->id == CALC_M_COPY) {
            char text[64]; snprintf(text, sizeof(text), "%s", a->calc.display);
            size_t n = strlen(text);
            if (n && text[n - 1] == '.') text[n - 1] = 0;
            du_clipboard_set(text);
        } else if (e->id == CALC_M_PASTE) calc_paste(&a->calc, du_clipboard_get());
        else if (e->id == CALC_M_GROUPING) {
            a->digit_grouping = !a->digit_grouping;
            a->view_items[1].flags = a->digit_grouping ? DU_ITEM_CHECKED : 0;
        } else if (e->id == CALC_M_HELP) {
            /* G15: the Calculator topic in CiukiOS Help; the box when Help cannot start. */
            if (du_help(u, NULL)) du_message(u, 1, "Calculator Help",
                       "Type numbers and operators on the keyboard or click the keys.\n"
                       "Enter or = : result     Esc : C     Del : CE     Backspace\n"
                       "@ : square root     r : 1/x     % : percent     F9 : +/-\n"
                       "Ctrl+L MC   Ctrl+R MR   Ctrl+M MS   Ctrl+P M+", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        } else if (e->id == CALC_M_ABOUT)
            du_message(u, 2, "About Calculator", "CiukiOS Calculator - Classic computing, one woof at a time.\nStandard mode, double precision.\nCode: MIT.",
                       DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        break;
    case DU_EV_KEY:
        if (!calc_key(&a->calc, e->key, e->ch, u->modifiers)) return;
        break;
    default: return;
    }
    if (strcmp(display,a->calc.display) || grouping!=a->digit_grouping) du_invalidate(u,display_box);
    if (memory!=a->calc.has_memory) du_invalidate(u,memory_box);
}
