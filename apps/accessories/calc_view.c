/* SPDX-License-Identifier: MIT
 * Calculator window on libdeskui: the Windows 98 standard layout (display,
 * memory indicator, Backspace/CE/C, memory column and 5x4 key grid) and the
 * Scientific layout (G27): base and angle radio buttons, Inv and Hyp check
 * boxes, a parenthesis indicator and 62 keys. The window is created at the
 * Scientific size (protocol v2 has no client-requested resize), so the
 * standard keys are stretched to the window. */
#include "calc.h"
#include <stdio.h>
#include <string.h>

enum calc_action { A_DIGIT, A_POINT, A_OP, A_EQUALS, A_SIGN, A_PERCENT, A_SQRT, A_RECIP,
                   A_MC, A_MR, A_MS, A_MPLUS, A_BACK, A_CE, A_C,
                   A_BIN, A_SIN, A_COS, A_TAN, A_LOG, A_LN, A_SQUARE, A_CUBE, A_FACT, A_PI, A_EXP, A_DMS, A_INT,
                   A_NOT, A_FE, A_OPEN, A_CLOSE, A_STA, A_AVE, A_SUM, A_DEV, A_DAT };
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
static struct du_rect display_box = {8, 26, 424, 24}, memory_box = {8, 58, 36, 26}, paren_box = {48, 58, 52, 22};

/* Scientific keys: column 0 holds Sta Ave Sum s Dat (A-F in Hex), then F-E ( ) Exp dms; sin cos tan log ln;
 * x^2 x^3 x^y n! 1/x; Mod And Or Xor Lsh; MC MR MS M+ pi; and the digit block with the operators.
 * The six keys of column 0 (indices 3-8 of sci_keys) become A-F in Hex, where the statistics keys do not apply. */
#define SCI_SLOT0 3
#define SCOL(n) (8 + 42 * (n))
#define SROW(n) (136 + 30 * (n))
#define SKEY(label, action, arg, col, row) {label, action, arg, SCOL(col), SROW(row), 38}
static const struct calc_key sci_keys[] = {
    {"Backspace", A_BACK, 0, 232, 108, 64}, {"CE", A_CE, 0, 300, 108, 64}, {"C", A_C, 0, 368, 108, 64},
    SKEY("Sta", A_STA, 0, 0, 0), SKEY("Ave", A_AVE, 0, 0, 1), SKEY("Sum", A_SUM, 0, 0, 2), SKEY("s", A_DEV, 0, 0, 3), SKEY("Dat", A_DAT, 0, 0, 4),
    SKEY("F", A_DIGIT, 15, 0, 5),
    SKEY("F-E", A_FE, 0, 1, 0), SKEY("(", A_OPEN, 0, 1, 1), SKEY(")", A_CLOSE, 0, 1, 2), SKEY("Exp", A_EXP, 0, 1, 3), SKEY("dms", A_DMS, 0, 1, 4),
    SKEY("sin", A_SIN, 0, 2, 0), SKEY("cos", A_COS, 0, 2, 1), SKEY("tan", A_TAN, 0, 2, 2), SKEY("log", A_LOG, 0, 2, 3), SKEY("ln", A_LN, 0, 2, 4),
    SKEY("x^2", A_SQUARE, 0, 3, 0), SKEY("x^3", A_CUBE, 0, 3, 1), SKEY("x^y", A_BIN, CALC_POW, 3, 2), SKEY("n!", A_FACT, 0, 3, 3), SKEY("1/x", A_RECIP, 0, 3, 4),
    SKEY("Mod", A_BIN, CALC_MOD, 4, 0), SKEY("And", A_BIN, CALC_AND, 4, 1), SKEY("Or", A_BIN, CALC_OR, 4, 2), SKEY("Xor", A_BIN, CALC_XOR, 4, 3), SKEY("Lsh", A_BIN, CALC_LSH, 4, 4),
    SKEY("MC", A_MC, 0, 5, 0), SKEY("MR", A_MR, 0, 5, 1), SKEY("MS", A_MS, 0, 5, 2), SKEY("M+", A_MPLUS, 0, 5, 3), SKEY("pi", A_PI, 0, 5, 4),
    SKEY("7", A_DIGIT, 7, 6, 0), SKEY("4", A_DIGIT, 4, 6, 1), SKEY("1", A_DIGIT, 1, 6, 2), SKEY("0", A_DIGIT, 0, 6, 3), SKEY("Not", A_NOT, 0, 6, 4),
    SKEY("8", A_DIGIT, 8, 7, 0), SKEY("5", A_DIGIT, 5, 7, 1), SKEY("2", A_DIGIT, 2, 7, 2), SKEY("+/-", A_SIGN, 0, 7, 3), SKEY("Int", A_INT, 0, 7, 4),
    SKEY("9", A_DIGIT, 9, 8, 0), SKEY("6", A_DIGIT, 6, 8, 1), SKEY("3", A_DIGIT, 3, 8, 2), SKEY(".", A_POINT, 0, 8, 3),
    SKEY("/", A_OP, CALC_DIV, 9, 0), SKEY("*", A_OP, CALC_MUL, 9, 1), SKEY("-", A_OP, CALC_SUB, 9, 2), SKEY("+", A_OP, CALC_ADD, 9, 3), SKEY("=", A_EQUALS, 0, 9, 4),
};
#define SCI_KEY_COUNT (sizeof(sci_keys) / sizeof(sci_keys[0]))

static const struct calc_key *table(const struct calc_app *a) { return a->calc.scientific ? sci_keys : keys; }
static unsigned table_count(const struct calc_app *a) { return a->calc.scientific ? SCI_KEY_COUNT : KEY_COUNT; }

int calc_button_id(const char *label)
{
    for (unsigned i = 0; i < KEY_COUNT; i++) if (!strcmp(keys[i].label, label)) return CALC_B_FIRST + (int)i;
    return -1;
}
int calc_sci_button_id(const char *label)
{
    for (unsigned i = 0; i < SCI_KEY_COUNT; i++) if (!strcmp(sci_keys[i].label, label)) return CALC_B_FIRST + (int)i;
    return -1;
}

static void run(struct calc *c, const struct calc_key *k, int slot)
{
    if (c->scientific && c->base == 16 && slot >= 0) { calc_digit(c, 10 + slot); return; }
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
    case A_BIN: calc_binary(c, (enum calc_op)k->arg); break;
    case A_SIN: calc_sin(c); break;
    case A_COS: calc_cos(c); break;
    case A_TAN: calc_tan(c); break;
    case A_LOG: calc_log(c); break;
    case A_LN: calc_ln(c); break;
    case A_SQUARE: calc_square(c); break;
    case A_CUBE: calc_cube(c); break;
    case A_FACT: calc_factorial(c); break;
    case A_PI: calc_pi(c); break;
    case A_EXP: calc_exp(c); break;
    case A_DMS: calc_dms(c); break;
    case A_INT: calc_int(c); break;
    case A_NOT: calc_not(c); break;
    case A_FE: calc_toggle_fe(c); break;
    case A_OPEN: calc_open_paren(c); break;
    case A_CLOSE: calc_close_paren(c); break;
    case A_STA: calc_stat_clear(c); break;
    case A_AVE: calc_stat_ave(c); break;
    case A_SUM: calc_stat_sum(c); break;
    case A_DEV: calc_stat_dev(c); break;
    case A_DAT: calc_stat_data(c); break;
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
    char text[128];
    if (a->digit_grouping && !a->calc.error && a->calc.base == 10) grouped(a->calc.display, text, sizeof(text));
    else snprintf(text, sizeof(text), "%s", a->calc.display);
    int w = du_text_width(text);
    if (w > display_box.w - 12) du_text_fit(c, display_box.x + 6, display_box.y + 9, display_box.w - 12, text, DU_INK);
    else du_text(c, display_box.x + display_box.w - 6 - w, display_box.y + 9, text, DU_INK);
    du_fill(c, memory_box, DU_FACE);
    du_edge(c, memory_box, DU_SUNKEN_THIN);
    if (a->calc.has_memory) du_text(c, memory_box.x + 15, memory_box.y + 10, "M", DU_INK);
    if (a->calc.scientific) {
        du_fill(c, paren_box, DU_FACE);
        du_edge(c, paren_box, DU_SUNKEN_THIN);
        int depth = calc_paren_depth(&a->calc);
        if (depth) { char p[16]; snprintf(p, sizeof(p), "(=%d", depth); du_text(c, paren_box.x + 8, paren_box.y + 7, p, DU_INK); }
    }
}

static const struct du_menu_item edit_template[] = {
    {"&Copy", CALC_M_COPY, 0x2e, DU_CTRL, 0}, {"&Paste", CALC_M_PASTE, 0x2f, DU_CTRL, 0} };
static const struct du_menu_item view_template[] = {
    {"&Standard", CALC_M_STANDARD, 0, 0, DU_ITEM_RADIO | DU_ITEM_CHECKED}, {"S&cientific", CALC_M_SCIENTIFIC, 0, 0, DU_ITEM_RADIO},
    {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"&Digit grouping", CALC_M_GROUPING, 0, 0, 0} };
static const struct du_menu_item help_template[] = {
    {"&Help Topics", CALC_M_HELP, DU_KEY_F1, 0, 0}, {"&About Calculator", CALC_M_ABOUT, 0, 0, 0} };

static int scaled_x(int x) { return x * CALC_W / CALC_STD_W; }
static int scaled_y(int y) { return y * CALC_H / CALC_STD_H; }

/* Keeps labels, enabled state and the check/radio values in step with the engine. */
static void sync(struct calc_app *a)
{
    struct du_ui *u = a->ui; struct calc *c = &a->calc;
    a->view_items[0].flags = DU_ITEM_RADIO | (c->scientific ? 0 : DU_ITEM_CHECKED);
    a->view_items[1].flags = DU_ITEM_RADIO | (c->scientific ? DU_ITEM_CHECKED : 0);
    a->view_items[3].flags = a->digit_grouping ? DU_ITEM_CHECKED : 0;
    if (!c->scientific) return;
    static const int base_ids[] = {CALC_R_HEX, CALC_R_DEC, CALC_R_OCT, CALC_R_BIN};
    static const int base_values[] = {16, 10, 8, 2};
    for (int i = 0; i < 4; i++) { struct du_widget *w = du_get(u, base_ids[i]); if (w) w->value = c->base == base_values[i]; }
    for (int i = 0; i < 3; i++) { struct du_widget *w = du_get(u, CALC_R_DEG + i); if (w) { w->value = c->angle == i; w->disabled = c->base != 10; } }
    struct du_widget *w = du_get(u, CALC_C_INV); if (w) { w->value = c->inv; w->disabled = c->base != 10; }
    w = du_get(u, CALC_C_HYP); if (w) { w->value = c->hyp; w->disabled = c->base != 10; }
    for (unsigned i = 0; i < SCI_KEY_COUNT; i++) {
        struct du_widget *k = du_get(u, CALC_B_FIRST + (int)i);
        if (!k) continue;
        const struct calc_key *key = &sci_keys[i];
        int slot = (int)i - SCI_SLOT0;
        k->hidden = 0; k->disabled = 0;
        if (slot >= 0 && slot < 6) {                   /* column 0: statistics or A-F */
            if (c->base == 16) { char l[2] = {(char)('A' + slot), 0}; du_set_text(k, l); }
            else {
                du_set_text(k, key->label);
                if (slot == 5) k->hidden = 1;
                else k->disabled = c->base != 10;
            }
            continue;
        }
        switch (key->action) {
        case A_DIGIT: k->disabled = key->arg >= c->base; break;
        case A_POINT: case A_FE: case A_EXP: case A_DMS: case A_SIN: case A_COS: case A_TAN: case A_LOG: case A_LN:
        case A_BIN:
            k->disabled = c->base != 10 && !(key->action == A_BIN && key->arg != CALC_POW);
            break;
        case A_CUBE: case A_FACT: case A_PI: case A_RECIP: k->disabled = c->base != 10; break;
        default: break;
        }
    }
}

static void build(struct calc_app *a)
{
    struct du_ui *u = a->ui;
    u->count = 0; u->focus = -1;
    display_box = (struct du_rect){8, 26, CALC_W - 16, 24};
    if (!a->calc.scientific) {
        memory_box = (struct du_rect){8, scaled_y(58), 36, scaled_y(26)};
        for (unsigned i = 0; i < KEY_COUNT; i++)
            du_add(u, DU_BUTTON, CALC_B_FIRST + (int)i,
                   (struct du_rect){scaled_x(keys[i].x), scaled_y(keys[i].y), scaled_x(keys[i].w), scaled_y(i < 3 ? 26 : 28)}, keys[i].label);
    } else {
        memory_box = (struct du_rect){8, 58, 36, 22};
        paren_box = (struct du_rect){48, 58, 52, 22};
        static const char *const base_names[] = {"Hex", "Dec", "Oct", "Bin"};
        for (int i = 0; i < 4; i++) { struct du_widget *w = du_add(u, DU_RADIO, CALC_R_HEX + i, (struct du_rect){112 + 56 * i, 58, 52, 18}, base_names[i]); if (w) w->total = 1; }
        static const char *const angle_names[] = {"Degrees", "Radians", "Grads"};
        static const int angle_x[] = {112, 192, 272};
        for (int i = 0; i < 3; i++) { struct du_widget *w = du_add(u, DU_RADIO, CALC_R_DEG + i, (struct du_rect){angle_x[i], 82, i == 2 ? 60 : 76, 18}, angle_names[i]); if (w) w->total = 2; }
        du_add(u, DU_CHECKBOX, CALC_C_INV, (struct du_rect){8, 82, 48, 18}, "Inv");
        du_add(u, DU_CHECKBOX, CALC_C_HYP, (struct du_rect){58, 82, 48, 18}, "Hyp");
        for (unsigned i = 0; i < SCI_KEY_COUNT; i++)
            du_add(u, DU_BUTTON, CALC_B_FIRST + (int)i, (struct du_rect){sci_keys[i].x, sci_keys[i].y, sci_keys[i].w, i < 3 ? 26 : 26}, sci_keys[i].label);
    }
    sync(a);
    u->dirty = 1;
}

void calc_view_set(struct calc_app *a, int scientific)
{
    if (a->calc.scientific == !!scientific) return;
    calc_scientific(&a->calc, scientific);
    build(a);
    du_invalidate(a->ui, (struct du_rect){0, 0, CALC_W, CALC_H});
}

void calc_attach(struct calc_app *a, struct du_ui *u)
{
    a->ui = u; u->event = calc_event; u->user = a; u->paint = calc_paint;
    calc_init(&a->calc);
    memcpy(a->edit_items, edit_template, sizeof(edit_template));
    memcpy(a->view_items, view_template, sizeof(view_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Edit", a->edit_items, 2};
    a->menus[1] = (struct du_menu){"&View", a->view_items, 4};
    a->menus[2] = (struct du_menu){"&Help", a->help_items, 2};
    u->menus = a->menus; u->menu_count = 3;
    build(a);   /* u->focus = -1: keys go to the calculator, not to a focused button */
}

void calc_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct calc_app *a=user;
    char display[sizeof(a->calc.display)];memcpy(display,a->calc.display,sizeof(display));
    int memory=a->calc.has_memory,grouping=a->digit_grouping,depth=calc_paren_depth(&a->calc);
    int scientific=a->calc.scientific;
    switch (e->type) {
    case DU_EV_ACTIVATE:
        if (e->id >= CALC_B_FIRST && e->id < CALC_B_FIRST + (int)table_count(a)) {
            int slot = a->calc.scientific ? e->id - CALC_B_FIRST - SCI_SLOT0 : -1;
            run(&a->calc, &table(a)[e->id - CALC_B_FIRST], slot >= 0 && slot < 6 ? slot : -1);
            sync(a); u->focus = -1; du_invalidate(u, (struct du_rect){0, 58, CALC_W, 100});
        }
        else if (e->id == CALC_M_COPY) {
            char text[96]; snprintf(text, sizeof(text), "%s", a->calc.display);
            size_t n = strlen(text);
            if (n && text[n - 1] == '.') text[n - 1] = 0;
            du_clipboard_set(text);
        } else if (e->id == CALC_M_PASTE) calc_paste(&a->calc, du_clipboard_get());
        else if (e->id == CALC_M_STANDARD) calc_view_set(a, 0);
        else if (e->id == CALC_M_SCIENTIFIC) calc_view_set(a, 1);
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
            du_message(u, 2, "About Calculator", "CiukiOS Calculator - Classic computing, one woof at a time.\nStandard and Scientific views, double precision.\nCode: MIT.\n"
                       "Ctrl+L MC   Ctrl+R MR   Ctrl+M MS   Ctrl+P M+\n"
                       "Scientific: s sin  o cos  t tan  l log  n ln  q x^2  # x^3  y x^y  ! n!\n"
                       "p pi  x Exp  m dms  ; Int  i Inv  h Hyp  v F-E  ( )  % Mod  & And  | Or  ^ Xor  < Lsh  ~ Not\n"
                       "F2 Degrees  F3 Radians  F4 Grads  F5 Hex  F6 Dec  F7 Oct  F8 Bin\n"
                       "Ctrl+A Ave  Ctrl+S Sum  Ctrl+T s  Ctrl+D Dat", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        else if (e->id == CALC_M_ABOUT)
            du_message(u, 2, "About Calculator", "CiukiOS Calculator - Classic computing, one woof at a time.\nStandard and Scientific views, double precision.\nCode: MIT.",
                       DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        break;
    case DU_EV_CHANGE:
        if (!a->calc.scientific) return;
        if (e->id >= CALC_R_HEX && e->id <= CALC_R_BIN) calc_set_base(&a->calc, e->id == CALC_R_HEX ? 16 : e->id == CALC_R_DEC ? 10 : e->id == CALC_R_OCT ? 8 : 2);
        else if (e->id >= CALC_R_DEG && e->id <= CALC_R_GRAD) calc_set_angle(&a->calc, (enum calc_angle)(e->id - CALC_R_DEG));
        else if (e->id == CALC_C_INV) a->calc.inv = e->index ? 1 : 0;
        else if (e->id == CALC_C_HYP) a->calc.hyp = e->index ? 1 : 0;
        else return;
        sync(a); u->focus = -1; du_invalidate(u, (struct du_rect){0, 26, CALC_W, 160});
        break;
    case DU_EV_KEY:
        if (!calc_key(&a->calc, e->key, e->ch, u->modifiers)) return;
        sync(a); du_invalidate(u, (struct du_rect){0, 58, CALC_W, 100});
        break;
    default: return;
    }
    if (strcmp(display,a->calc.display) || grouping!=a->digit_grouping || scientific!=a->calc.scientific) du_invalidate(u,display_box);
    if (memory!=a->calc.has_memory) du_invalidate(u,memory_box);
    if (depth!=calc_paren_depth(&a->calc)) du_invalidate(u,paren_box);
}
