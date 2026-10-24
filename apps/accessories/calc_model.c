/* SPDX-License-Identifier: MIT
 * Calculator engine: Windows 98 standard mode semantics (immediate
 * execution: 2 + 3 * 4 = 20; "=" repeats the last operation; a op b %
 * gives a*b/100 as the right operand). The Scientific view (G27) follows the
 * Windows 98 Scientific Calculator: operator precedence with parentheses,
 * Dec/Hex/Oct/Bin integers (64-bit two's complement), Inv/Hyp, angle units. */
#include "calc.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void format_base(char *out, size_t cap, double v, int base)
{
    /* Hex, Octal and Binary show the integer part as 64-bit two's complement. */
    int64_t n = v >= 9.2233720368547758e18 ? INT64_MAX : v <= -9.2233720368547758e18 ? INT64_MIN : (int64_t)v;
    uint64_t u = (uint64_t)n; char tmp[72]; int k = 0;
    do { tmp[k++] = "0123456789ABCDEF"[u % (unsigned)base]; u /= (unsigned)base; } while (u && k < 70);
    size_t i = 0; while (k && i + 1 < cap) out[i++] = tmp[--k];
    out[i] = 0;
}
static void format_fe(char *out, size_t cap, double v)
{
    /* F-E: 1.2345e+2, mantissa trailing zeros and exponent leading zeros removed. */
    char t[48]; snprintf(t, sizeof(t), "%.14e", v);
    char *e = strchr(t, 'e');
    if (!e) { snprintf(out, cap, "%s", t); return; }
    int ex = atoi(e + 1); *e = 0;
    char *m = t + strlen(t);
    while (m > t && m[-1] == '0') m--;
    *m = 0;
    snprintf(out, cap, "%s%se%c%d", t, strchr(t, '.') ? "" : ".", ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
}
static void format(const struct calc *c, char *out, size_t cap, double v)
{
    if (v == 0) v = 0;   /* no "-0" */
    if (c->base != 10) { format_base(out, cap, v, c->base); return; }
    if (c->fe) { format_fe(out, cap, v); return; }
    snprintf(out, cap, "%.15g", v);
}
static void show_text(struct calc *c, const char *text)
{
    /* Windows calculators show a trailing decimal point on whole numbers (decimal only). */
    snprintf(c->display, sizeof(c->display), "%s%s", text,
             c->base != 10 || strchr(text, '.') || strchr(text, 'e') ? "" : ".");
}
static void fail(struct calc *c, const char *message)
{
    c->error = 1; c->entering = c->typed = 0; c->op = c->last_op = CALC_NONE;
    snprintf(c->display, sizeof(c->display), "%s", message);
}
static void show_value(struct calc *c)
{
    if (isnan(c->value) || isinf(c->value)) { fail(c, "Result of function is undefined."); return; }
    char text[72]; format(c, text, sizeof(text), c->value); show_text(c, text);
}
static void show_entry(struct calc *c) { show_text(c, c->entry[0] && strcmp(c->entry, "-") ? c->entry : "0"); }
static double entry_value(const struct calc *c)
{
    if (c->base == 10) return strtod(c->entry, NULL);
    return (double)(int64_t)strtoull(c->entry, NULL, c->base);   /* two's complement round trip */
}
double calc_current(const struct calc *c)
{
    if (!c->entering) return c->value;
    return c->use_exact && !c->typed ? c->exact : entry_value(c);
}
void calc_init(struct calc *c) { memset(c, 0, sizeof(*c)); c->base = 10; show_value(c); }
void calc_clear(struct calc *c)
{
    struct calc keep = *c;   /* C keeps the memory, the view, the base, the angle unit and the statistics data */
    calc_init(c);
    c->memory = keep.memory; c->has_memory = keep.has_memory;
    c->scientific = keep.scientific; c->angle = keep.angle; c->fe = keep.fe;
    c->base = keep.base; c->nstat = keep.nstat; memcpy(c->stat, keep.stat, sizeof(c->stat));
    show_value(c);
}
static int digits_in(const char *s)
{
    int n = 0;
    for (; *s; s++) { if (*s == 'e') break; n += (*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'F'); }
    return n;
}
static int digit_limit(const struct calc *c) { return c->base == 2 ? 64 : c->base == 8 ? 22 : c->base == 16 ? 16 : CALC_DIGITS; }
static void begin_typing(struct calc *c)
{
    if (c->error) calc_clear(c);
    if (!c->entering || !c->typed) { c->entry[0] = 0; c->entering = c->typed = 1; c->use_exact = 0; }
}
void calc_digit(struct calc *c, int d)
{
    if (d < 0 || d >= c->base) return;
    begin_typing(c);
    if (digits_in(c->entry) >= digit_limit(c)) return;
    size_t n = strlen(c->entry);
    if (!strcmp(c->entry + (c->entry[0] == '-'), "0")) n--;   /* replace a lone leading zero */
    c->entry[n] = "0123456789ABCDEF"[d]; c->entry[n + 1] = 0;
    show_entry(c);
}
void calc_point(struct calc *c)
{
    if (c->base != 10) return;
    begin_typing(c);
    if (strchr(c->entry, '.') || strchr(c->entry, 'e')) return;
    if (!c->entry[0] || !strcmp(c->entry, "-")) strcat(c->entry, "0");
    strcat(c->entry, ".");
    show_entry(c);
}
/* Reduces "left op right"; reports an error through fail(). */
static int compute(struct calc *c, enum calc_op op, double l, double r, double *out)
{
    int64_t a = (int64_t)l, b = (int64_t)r;
    switch (op) {
    case CALC_ADD: *out = l + r; break;
    case CALC_SUB: *out = l - r; break;
    case CALC_MUL: *out = l * r; break;
    case CALC_DIV:
        if (r == 0) { fail(c, l == 0 ? "Result of function is undefined." : "Cannot divide by zero."); return -1; }
        *out = l / r; break;
    case CALC_MOD:
        if (r == 0) { fail(c, "Cannot divide by zero."); return -1; }
        *out = fmod(l, r); break;
    case CALC_POW: *out = pow(l, r); break;
    case CALC_ROOT:
        if (r == 0) { fail(c, "Invalid input for function."); return -1; }
        *out = l < 0 && r == floor(r) && fmod(r, 2) != 0 ? -pow(-l, 1.0 / r) : pow(l, 1.0 / r); break;
    case CALC_AND: *out = (double)(a & b); break;
    case CALC_OR: *out = (double)(a | b); break;
    case CALC_XOR: *out = (double)(a ^ b); break;
    case CALC_LSH: *out = r < 0 || r > 63 ? 0 : (double)(int64_t)((uint64_t)a << (int)b); break;
    case CALC_RSH: *out = r < 0 || r > 63 ? (a < 0 ? -1 : 0) : (double)(a >> (int)b); break;
    case CALC_NONE: *out = r; break;
    }
    return 0;
}
static int apply(struct calc *c, enum calc_op op, double right)
{
    double v;
    if (compute(c, op, c->value, right, &v)) return -1;
    c->value = v;
    show_value(c);
    return c->error ? -1 : 0;
}
static int precedence(int op)
{
    switch (op) {
    case CALC_OR: case CALC_XOR: return 1;
    case CALC_AND: return 2;
    case CALC_LSH: case CALC_RSH: return 3;
    case CALC_ADD: case CALC_SUB: return 4;
    case CALC_MUL: case CALC_DIV: case CALC_MOD: return 5;
    default: return 6;
    }
}
/* Applies the pending operators whose precedence is at least min_prec, stopping at a parenthesis. */
static int reduce(struct calc *c, double *x, int min_prec, enum calc_op *last, double *right)
{
    while (c->depth > 0 && c->stack[c->depth - 1].op != -1 && precedence(c->stack[c->depth - 1].op) >= min_prec) {
        struct calc_pending p = c->stack[--c->depth]; double v;
        if (last) { *last = (enum calc_op)p.op; *right = *x; }
        if (compute(c, (enum calc_op)p.op, p.value, *x, &v)) return -1;
        *x = v;
        if (isnan(v) || isinf(v)) { fail(c, "Result of function is undefined."); return -1; }
    }
    return 0;
}
static void sci_operator(struct calc *c, enum calc_op op)
{
    double x;
    if (c->entering) x = calc_current(c);
    else {
        x = c->value;   /* an operator replaces the one just pressed */
        if (c->op != CALC_NONE && c->depth && c->stack[c->depth - 1].op != -1) x = c->stack[--c->depth].value;
    }
    if (reduce(c, &x, precedence(op), NULL, NULL)) return;
    if (c->depth >= CALC_STACK) { fail(c, "Invalid input for function."); return; }
    c->value = x; show_value(c);
    if (c->error) return;
    c->stack[c->depth].value = x; c->stack[c->depth].op = (int)op; c->depth++;
    c->op = op; c->last_op = CALC_NONE; c->entering = c->typed = 0;
}
static void sci_equals(struct calc *c)
{
    double x = c->entering ? calc_current(c) : c->value;
    enum calc_op last = CALC_NONE; double right = 0;
    int had = c->depth > 0 && c->op != CALC_NONE;
    if (c->depth == 0 && c->last_op != CALC_NONE) {          /* "=" repeats the last operation */
        double v;
        if (compute(c, c->last_op, x, c->operand, &v)) return;
        c->value = v; show_value(c); c->entering = c->typed = 0; return;
    }
    while (c->depth > 0) {
        if (c->stack[c->depth - 1].op == -1) { c->depth--; continue; }
        if (reduce(c, &x, 0, &last, &right)) return;
    }
    c->value = x; c->op = CALC_NONE;
    if (had && last != CALC_NONE) { c->last_op = last; c->operand = right; }
    show_value(c);
    c->entering = c->typed = 0;
}
void calc_operator(struct calc *c, enum calc_op op)
{
    if (c->error) return;
    if (c->scientific) { sci_operator(c, op); return; }
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
    if (c->scientific) { sci_equals(c); return; }
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
    c->entry[0] = 0; c->entering = 1; c->typed = 0; c->use_exact = 0; show_entry(c);
}
void calc_backspace(struct calc *c)
{
    if (c->error || !c->entering || !c->typed) return;
    size_t n = strlen(c->entry);
    if (n) c->entry[n - 1] = 0;
    if (!strcmp(c->entry, "-")) c->entry[0] = 0;
    show_entry(c);
}
/* An entry holding a number: the text is what the display shows, `exact` what later operations use. */
static void put_entry(struct calc *c, double v)
{
    if (c->base != 10) v = trunc(v);
    format(c, c->entry, sizeof(c->entry), v);
    c->exact = v; c->use_exact = 1; c->entering = 1; c->typed = 0;
    show_entry(c);
}
/* A unary function's result replaces the displayed operand; the next digit starts a new number. */
static void set_result(struct calc *c, double v)
{
    if (isnan(v) || isinf(v)) { fail(c, "Invalid input for function."); return; }
    put_entry(c, v);
}
void calc_negate(struct calc *c)
{
    if (c->error) return;
    char *ex = c->base == 10 ? strchr(c->entry, 'e') : NULL;
    if (c->entering && c->typed && ex) { ex[1] = ex[1] == '-' ? '+' : '-'; show_entry(c); }   /* sign of the exponent */
    else if (c->entering && c->typed && c->base == 10) {
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

/* ---- Scientific view ---- */
void calc_scientific(struct calc *c, int on)
{
    if (c->scientific == !!on) return;
    double v = calc_current(c);
    c->scientific = !!on; c->depth = 0; c->inv = c->hyp = 0;
    if (!on && c->base != 10) c->base = 10;
    c->op = c->last_op = CALC_NONE;
    if (!c->error) { c->value = v; show_value(c); c->entering = c->typed = 0; }
}
void calc_set_base(struct calc *c, int base)
{
    if (!c->scientific || (base != 2 && base != 8 && base != 10 && base != 16) || c->base == base) return;
    double v = c->error ? 0 : calc_current(c);
    if (c->error) calc_clear(c);
    c->base = base;
    if (base != 10) v = trunc(v);
    c->value = v;
    c->entering = c->typed = 0;
    show_value(c);
}
void calc_set_angle(struct calc *c, enum calc_angle a) { c->angle = a; }
void calc_toggle_inv(struct calc *c) { c->inv = !c->inv; }
void calc_toggle_hyp(struct calc *c) { c->hyp = !c->hyp; }
void calc_toggle_fe(struct calc *c)
{
    if (c->base != 10) return;
    c->fe = !c->fe;
    if (c->error) return;
    if (c->entering) put_entry(c, calc_current(c));
    else show_value(c);
}
void calc_binary(struct calc *c, enum calc_op op)
{
    if (c->error) return;
    if (c->inv) { if (op == CALC_POW) op = CALC_ROOT; else if (op == CALC_LSH) op = CALC_RSH; }
    c->inv = 0;
    calc_operator(c, op);
}
void calc_open_paren(struct calc *c)
{
    if (c->error || !c->scientific) return;
    if (c->depth >= CALC_STACK) { fail(c, "Invalid input for function."); return; }
    c->stack[c->depth].value = 0; c->stack[c->depth].op = -1; c->depth++;
    c->op = CALC_NONE; c->last_op = CALC_NONE; c->value = 0; c->entering = c->typed = 0;
    show_value(c);
}
int calc_paren_depth(const struct calc *c)
{
    int n = 0;
    for (int i = 0; i < c->depth; i++) n += c->stack[i].op == -1;
    return n;
}
void calc_close_paren(struct calc *c)
{
    if (c->error || !c->scientific || !calc_paren_depth(c)) return;
    double x = c->entering ? calc_current(c) : c->value;
    if (reduce(c, &x, 0, NULL, NULL)) return;
    c->depth--;                       /* the parenthesis */
    c->value = x; c->op = CALC_NONE; c->entering = c->typed = 0;
    show_value(c);
}
static double to_rad(const struct calc *c, double v)
{
    return c->angle == CALC_DEG ? v * M_PI / 180.0 : c->angle == CALC_GRAD ? v * M_PI / 200.0 : v;
}
static double from_rad(const struct calc *c, double v)
{
    return c->angle == CALC_DEG ? v * 180.0 / M_PI : c->angle == CALC_GRAD ? v * 200.0 / M_PI : v;
}
static double snap(double v) { return fabs(v) < 1e-15 ? 0 : v; }
static void unary_done(struct calc *c, double v)
{
    c->inv = c->hyp = 0;
    set_result(c, v);
}
static void trig(struct calc *c, int which)   /* 0 sin, 1 cos, 2 tan */
{
    if (c->error) return;
    double v = calc_current(c), r;
    if (c->hyp) {
        static double (*const direct[3])(double) = {sinh, cosh, tanh}, (*const inverse[3])(double) = {asinh, acosh, atanh};
        if (c->inv && ((which == 1 && v < 1) || (which == 2 && fabs(v) >= 1))) { c->inv = c->hyp = 0; fail(c, "Invalid input for function."); return; }
        r = (c->inv ? inverse : direct)[which](v);
    } else if (c->inv) {
        if (which != 2 && fabs(v) > 1) { c->inv = c->hyp = 0; fail(c, "Invalid input for function."); return; }
        r = from_rad(c, which == 0 ? asin(v) : which == 1 ? acos(v) : atan(v));
    } else {
        double a = to_rad(c, v);
        r = which == 0 ? sin(a) : which == 1 ? cos(a) : tan(a);
        r = snap(r);
    }
    unary_done(c, r);
}
void calc_sin(struct calc *c) { trig(c, 0); }
void calc_cos(struct calc *c) { trig(c, 1); }
void calc_tan(struct calc *c) { trig(c, 2); }
void calc_log(struct calc *c)
{
    if (c->error) return;
    double v = calc_current(c);
    if (!c->inv && v <= 0) { c->inv = c->hyp = 0; fail(c, "Invalid input for function."); return; }
    unary_done(c, c->inv ? pow(10, v) : log10(v));
}
void calc_ln(struct calc *c)
{
    if (c->error) return;
    double v = calc_current(c);
    if (!c->inv && v <= 0) { c->inv = c->hyp = 0; fail(c, "Invalid input for function."); return; }
    unary_done(c, c->inv ? exp(v) : log(v));
}
void calc_square(struct calc *c)
{
    if (c->error) return;
    double v = calc_current(c);
    if (c->inv && v < 0) { c->inv = c->hyp = 0; fail(c, "Invalid input for function."); return; }
    unary_done(c, c->inv ? sqrt(v) : v * v);
}
void calc_cube(struct calc *c) { if (!c->error) { double v = calc_current(c); unary_done(c, c->inv ? cbrt(v) : v * v * v); } }
void calc_factorial(struct calc *c)
{
    if (c->error) return;
    double v = calc_current(c), r = 1;
    if (v < 0 && v == floor(v)) { c->inv = c->hyp = 0; fail(c, "Invalid input for function."); return; }
    if (v > 170) { c->inv = c->hyp = 0; fail(c, "Result of function is undefined."); return; }
    if (v == floor(v)) for (int i = 2; i <= (int)v; i++) r *= i;
    else r = tgamma(v + 1);
    unary_done(c, r);
}
void calc_pi(struct calc *c) { if (!c->error) unary_done(c, c->inv ? 2 * M_PI : M_PI); }
void calc_exp(struct calc *c)
{
    if (c->error || c->base != 10) return;
    if (!(c->entering && c->typed)) { begin_typing(c); strcpy(c->entry, "0"); }
    if (strchr(c->entry, 'e')) return;
    if (strlen(c->entry) + 2 >= sizeof(c->entry)) return;
    strcat(c->entry, strchr(c->entry, '.') ? "e+" : ".e+");
    show_entry(c);
}
void calc_dms(struct calc *c)
{
    if (c->error) return;
    double v = calc_current(c), sign = v < 0 ? -1 : 1, a = fabs(v), r;
    if (!c->inv) {          /* d.dddd to d.mmss */
        double d = floor(a), m = floor((a - d) * 60 + 1e-9), s = ((a - d) * 60 - m) * 60;
        if (s > 59.9999999) { s = 0; m += 1; }
        r = d + m / 100.0 + s / 10000.0;
    } else {                /* d.mmss to d.dddd */
        double d = floor(a); long n = lround((a - d) * 10000.0);   /* mmss.ss as an integer of 1/10000 */
        r = d + (double)(n / 100) / 60.0 + (double)(n % 100) / 3600.0;
    }
    unary_done(c, sign * r);
}
void calc_int(struct calc *c) { if (!c->error) { double v = calc_current(c); unary_done(c, c->inv ? v - trunc(v) : trunc(v)); } }
void calc_not(struct calc *c) { if (!c->error) unary_done(c, (double)~(int64_t)calc_current(c)); }
void calc_stat_clear(struct calc *c) { c->nstat = 0; }
int calc_stat_load(struct calc *c, unsigned index)
{
    if (index >= (unsigned)c->nstat) return 0;
    c->error = 0;
    unary_done(c, c->stat[index]);
    return 1;
}
int calc_stat_delete(struct calc *c, unsigned index)
{
    if (index >= (unsigned)c->nstat) return 0;
    memmove(c->stat + index, c->stat + index + 1, ((unsigned)c->nstat - index - 1) * sizeof(c->stat[0]));
    c->nstat--;
    return 1;
}
void calc_stat_data(struct calc *c)
{
    if (c->error) return;
    if (c->nstat < CALC_STAT) c->stat[c->nstat++] = calc_current(c);
    put_entry(c, c->stat[c->nstat ? c->nstat - 1 : 0]);   /* Dat leaves the entry */
}
static void stat_result(struct calc *c, int kind)   /* 0 ave, 1 sum, 2 s */
{
    if (c->error) return;
    int n = c->nstat; double sum = 0, sq = 0;
    for (int i = 0; i < n; i++) { sum += c->stat[i]; sq += c->stat[i] * c->stat[i]; }
    if (!n || (kind == 2 && !c->inv && n < 2)) { c->inv = c->hyp = 0; fail(c, "Invalid input for function."); return; }
    double r;
    if (kind == 0) r = (c->inv ? sq : sum) / n;
    else if (kind == 1) r = c->inv ? sq : sum;
    else {
        double mean = sum / n, ss = sq - n * mean * mean;
        if (ss < 0) ss = 0;
        r = sqrt(ss / (c->inv ? n : n - 1));
    }
    unary_done(c, r);
}
void calc_stat_ave(struct calc *c) { stat_result(c, 0); }
void calc_stat_sum(struct calc *c) { stat_result(c, 1); }
void calc_stat_dev(struct calc *c) { stat_result(c, 2); }

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
    if (c->scientific && !(modifiers & (DU_CTRL | DU_ALT))) {   /* F2-F4 angle unit, F5-F8 base */
        switch (code) {
        case 0x3c: calc_set_angle(c, CALC_DEG); return 1;
        case 0x3d: calc_set_angle(c, CALC_RAD); return 1;
        case 0x3e: calc_set_angle(c, CALC_GRAD); return 1;
        case 0x3f: calc_set_base(c, 16); return 1;
        case 0x40: calc_set_base(c, 10); return 1;
        case 0x41: calc_set_base(c, 8); return 1;
        case 0x42: calc_set_base(c, 2); return 1;
        default: break;
        }
    }
    if (modifiers & DU_CTRL) {                       /* Windows: Ctrl+L MC, Ctrl+R MR, Ctrl+M MS, Ctrl+P M+ */
        if (code == 0x26) { calc_memory_clear(c); return 1; }
        if (code == 0x13) { calc_memory_recall(c); return 1; }
        if (code == 0x32) { calc_memory_store(c); return 1; }
        if (code == 0x19) { calc_memory_add(c); return 1; }
        if (c->scientific) {                          /* Ctrl+A Ave, Ctrl+S Sum, Ctrl+T s, Ctrl+D Dat */
            if (code == 0x1e) { calc_stat_ave(c); return 1; }
            if (code == 0x1f) { calc_stat_sum(c); return 1; }
            if (code == 0x14) { calc_stat_dev(c); return 1; }
            if (code == 0x20) { calc_stat_data(c); return 1; }
        }
        return 0;
    }
    if (c->base == 16 && ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) { calc_digit(c, (int)((ch | 0x20) - 'a' + 10)); return 1; }
    if (ch >= '0' && ch <= '9') { calc_digit(c, (int)(ch - '0')); return 1; }
    if (c->scientific) {
        switch (ch) {
        case '%': calc_binary(c, CALC_MOD); return 1;
        case '&': calc_binary(c, CALC_AND); return 1;
        case '|': calc_binary(c, CALC_OR); return 1;
        case '^': calc_binary(c, CALC_XOR); return 1;
        case '<': calc_binary(c, CALC_LSH); return 1;
        case '~': calc_not(c); return 1;
        case 'y': case 'Y': calc_binary(c, CALC_POW); return 1;
        case 's': case 'S': calc_sin(c); return 1;
        case 'o': case 'O': calc_cos(c); return 1;
        case 't': case 'T': calc_tan(c); return 1;
        case 'q': case 'Q': calc_square(c); return 1;
        case '#': calc_cube(c); return 1;
        case 'l': case 'L': calc_log(c); return 1;
        case 'n': case 'N': calc_ln(c); return 1;
        case '!': calc_factorial(c); return 1;
        case 'p': case 'P': calc_pi(c); return 1;
        case 'x': case 'X': calc_exp(c); return 1;
        case 'm': case 'M': calc_dms(c); return 1;
        case ';': calc_int(c); return 1;
        case 'i': case 'I': calc_toggle_inv(c); return 1;
        case 'h': case 'H': calc_toggle_hyp(c); return 1;
        case 'v': case 'V': calc_toggle_fe(c); return 1;
        case '(': calc_open_paren(c); return 1;
        case ')': calc_close_paren(c); return 1;
        default: break;
        }
    }
    switch (ch) {
    case '.': case ',': calc_point(c); return 1;
    case '+': calc_operator(c, CALC_ADD); return 1;
    case '-': calc_operator(c, CALC_SUB); return 1;
    case '*': calc_operator(c, CALC_MUL); return 1;
    case '/': calc_operator(c, CALC_DIV); return 1;
    case '=': calc_equals(c); return 1;
    case '%': calc_percent(c); return 1;
    case '@': if (c->scientific) return 0; calc_sqrt(c); return 1;
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
