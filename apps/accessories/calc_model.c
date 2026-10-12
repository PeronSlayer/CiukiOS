/* SPDX-License-Identifier: MIT
 * Calculator engine: Windows 98 standard mode semantics (immediate
 * execution: 2 + 3 * 4 = 20; "=" repeats the last operation; a op b %
 * gives a*b/100 as the right operand). */
#include "calc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void format(char *out, size_t cap, double v)
{
    if (v == 0) v = 0;   /* no "-0" */
    snprintf(out, cap, "%.15g", v);
}
static void show_text(struct calc *c, const char *text)
{
    /* Windows calculators show a trailing decimal point on whole numbers. */
    snprintf(c->display, sizeof(c->display), "%s%s", text, strchr(text, '.') || strchr(text, 'e') ? "" : ".");
}
static void fail(struct calc *c, const char *message)
{
    c->error = 1; c->entering = c->typed = 0; c->op = c->last_op = CALC_NONE;
    snprintf(c->display, sizeof(c->display), "%s", message);
}
static void show_value(struct calc *c)
{
    if (isnan(c->value) || isinf(c->value)) { fail(c, "Result of function is undefined."); return; }
    char text[48]; format(text, sizeof(text), c->value); show_text(c, text);
}
static void show_entry(struct calc *c) { show_text(c, c->entry[0] && strcmp(c->entry, "-") ? c->entry : "0"); }
double calc_current(const struct calc *c) { return c->entering ? strtod(c->entry, NULL) : c->value; }
void calc_init(struct calc *c) { memset(c, 0, sizeof(*c)); show_value(c); }
void calc_clear(struct calc *c) { double m = c->memory; int hm = c->has_memory; calc_init(c); c->memory = m; c->has_memory = hm; }
static int digits_in(const char *s) { int n = 0; for (; *s; s++) n += *s >= '0' && *s <= '9'; return n; }
static void begin_typing(struct calc *c)
{
    if (c->error) calc_clear(c);
    if (!c->entering || !c->typed) { c->entry[0] = 0; c->entering = c->typed = 1; }
}
void calc_digit(struct calc *c, int d)
{
    if (d < 0 || d > 9) return;
    begin_typing(c);
    if (digits_in(c->entry) >= CALC_DIGITS) return;
    size_t n = strlen(c->entry);
    if (!strcmp(c->entry + (c->entry[0] == '-'), "0")) n--;   /* replace a lone leading zero */
    c->entry[n] = (char)('0' + d); c->entry[n + 1] = 0;
    show_entry(c);
}
void calc_point(struct calc *c)
{
    begin_typing(c);
    if (strchr(c->entry, '.')) return;
    if (!c->entry[0] || !strcmp(c->entry, "-")) strcat(c->entry, "0");
    strcat(c->entry, ".");
    show_entry(c);
}
static int apply(struct calc *c, enum calc_op op, double right)
{
    switch (op) {
    case CALC_ADD: c->value += right; break;
    case CALC_SUB: c->value -= right; break;
    case CALC_MUL: c->value *= right; break;
    case CALC_DIV:
        if (right == 0) { fail(c, c->value == 0 ? "Result of function is undefined." : "Cannot divide by zero."); return -1; }
        c->value /= right; break;
    case CALC_NONE: c->value = right; break;
    }
    show_value(c);
    return c->error ? -1 : 0;
}
void calc_operator(struct calc *c, enum calc_op op)
{
    if (c->error) return;
    if (c->entering) {
        double right = calc_current(c);
        if (c->op != CALC_NONE) { c->operand = right; if (apply(c, c->op, right)) return; }
        else { c->value = right; show_value(c); }
    }
    c->op = op; c->last_op = CALC_NONE; c->entering = c->typed = 0;
}
void calc_equals(struct calc *c)
{
    if (c->error) return;
    if (c->op != CALC_NONE) {
        double right = c->entering ? calc_current(c) : c->value;   /* "2 + =" gives 4 */
        c->operand = right; c->last_op = c->op; c->op = CALC_NONE;
        apply(c, c->last_op, right);
    } else if (c->last_op != CALC_NONE) {
        if (c->entering) c->value = calc_current(c);
        apply(c, c->last_op, c->operand);                         /* "=" repeats the last operation */
    } else if (c->entering) { c->value = calc_current(c); show_value(c); }
    c->entering = c->typed = 0;
}
void calc_clear_entry(struct calc *c)
{
    if (c->error) { calc_clear(c); return; }
    c->entry[0] = 0; c->entering = 1; c->typed = 0; show_entry(c);
}
void calc_backspace(struct calc *c)
{
    if (c->error || !c->entering || !c->typed) return;
    size_t n = strlen(c->entry);
    if (n) c->entry[n - 1] = 0;
    if (!strcmp(c->entry, "-")) c->entry[0] = 0;
    show_entry(c);
}
/* A unary function's result replaces the displayed operand; the next digit starts a new number. */
static void set_result(struct calc *c, double v)
{
    if (isnan(v) || isinf(v)) { fail(c, "Invalid input for function."); return; }
    format(c->entry, sizeof(c->entry), v);
    c->entering = 1; c->typed = 0;
    show_entry(c);
}
void calc_negate(struct calc *c)
{
    if (c->error) return;
    if (c->entering && c->typed) {
        if (c->entry[0] == '-') memmove(c->entry, c->entry + 1, strlen(c->entry));
        else if (c->entry[0] && strtod(c->entry, NULL) != 0) { memmove(c->entry + 1, c->entry, strlen(c->entry) + 1); c->entry[0] = '-'; }
        show_entry(c);
    } else set_result(c, -calc_current(c));
}
void calc_percent(struct calc *c)
{
    if (c->error) return;
    double b = calc_current(c);
    set_result(c, c->op != CALC_NONE ? c->value * b / 100.0 : 0.0);
}
void calc_sqrt(struct calc *c)
{
    if (c->error) return;
    double v = calc_current(c);
    if (v < 0) { fail(c, "Invalid input for function."); return; }
    set_result(c, sqrt(v));
}
void calc_reciprocal(struct calc *c)
{
    if (c->error) return;
    double v = calc_current(c);
    if (v == 0) { fail(c, "Cannot divide by zero."); return; }
    set_result(c, 1.0 / v);
}
void calc_memory_clear(struct calc *c) { c->memory = 0; c->has_memory = 0; }
void calc_memory_recall(struct calc *c) { if (!c->error) set_result(c, c->memory); }
void calc_memory_store(struct calc *c) { if (!c->error) { c->memory = calc_current(c); c->has_memory = c->memory != 0; c->typed = 0; } }
void calc_memory_add(struct calc *c) { if (!c->error) { c->memory += calc_current(c); c->has_memory = c->memory != 0; c->typed = 0; } }

int calc_key(struct calc *c, unsigned code, unsigned ch, unsigned modifiers)
{
    /* Numeric keypad (Num Lock on) and the extended keypad keys. */
    static const char pad[] = "789-456+1230.";
    if (!ch && code >= 0x47 && code <= 0x53 && !(modifiers & (DU_CTRL | DU_ALT))) ch = (unsigned char)pad[code - 0x47];
    if (!ch && code == 0x37) ch = '*';
    if (!ch && code == 0x135) ch = '/';
    if (code == DU_KEY_ENTER || code == 0x11c) ch = '=';
    if (code == DU_KEY_ESC) { calc_clear(c); return 1; }
    if (code == DU_KEY_DELETE) { calc_clear_entry(c); return 1; }
    if (code == DU_KEY_BACKSPACE) { calc_backspace(c); return 1; }
    if (code == 0x43) { calc_negate(c); return 1; }   /* F9 */
    if (modifiers & DU_CTRL) {                       /* Windows: Ctrl+L MC, Ctrl+R MR, Ctrl+M MS, Ctrl+P M+ */
        if (code == 0x26) { calc_memory_clear(c); return 1; }
        if (code == 0x13) { calc_memory_recall(c); return 1; }
        if (code == 0x32) { calc_memory_store(c); return 1; }
        if (code == 0x19) { calc_memory_add(c); return 1; }
        return 0;
    }
    if (ch >= '0' && ch <= '9') { calc_digit(c, (int)(ch - '0')); return 1; }
    switch (ch) {
    case '.': case ',': calc_point(c); return 1;
    case '+': calc_operator(c, CALC_ADD); return 1;
    case '-': calc_operator(c, CALC_SUB); return 1;
    case '*': calc_operator(c, CALC_MUL); return 1;
    case '/': calc_operator(c, CALC_DIV); return 1;
    case '=': calc_equals(c); return 1;
    case '%': calc_percent(c); return 1;
    case '@': calc_sqrt(c); return 1;
    case 'r': case 'R': calc_reciprocal(c); return 1;
    default: return 0;
    }
}

int calc_paste(struct calc *c, const char *text)
{
    int used = 0;
    for (; text && *text; text++) {
        unsigned ch = (unsigned char)*text;
        if (ch == '\n' || ch == '\r') ch = '=';
        if (ch == ' ' || ch == '\t') continue;
        if (!((ch >= '0' && ch <= '9') || strchr(".,+-*/=%@", (int)ch))) break;
        calc_key(c, 0, ch, 0); used++;
    }
    return used;
}
