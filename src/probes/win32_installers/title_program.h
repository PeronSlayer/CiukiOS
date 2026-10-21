/* f5-15C guest operator program for the QEMU `installers` tier.
 *
 * The runner compiles the `guest` block of tests/win32/operators/<id>.json
 * (scripts/win32/operator_program.py) into CIUKI-OP.TXT on the run's own
 * parameter disk; the desktop's installer driver (apps/desktop/
 * installer_title.c) parses it here and executes it through the desktop's
 * input path. Nothing in this file is title-specific and nothing enters the
 * image. Line grammar (ASCII, LF or CRLF, at most TP_LINE bytes):
 *
 *   CIUKIOP 1
 *   workload <id>                 manifest workload id
 *   choices default               the gate installs with default choices only
 *   entry <path>                  installer, relative to the payload medium
 *   arguments <text>              command-line tail (may be empty)
 *   timeout <seconds>             whole install segment
 *   defaults <0|1> idle=<ms> max=<n>   interactive-defaults fallback
 *   restart accept|decline        answer to the virtual-restart prompt
 *   uninstall <0|1> timeout=<s>   run the uninstall segment when registered
 *   shot <ms>                     pause after a screenshot record
 *   step <install|uninstall> <action> key=value ...
 *   end
 *
 * Values are percent-encoded (%XX) where they contain bytes outside
 * [A-Za-z0-9._,:/\\*?+()<>!#-], so a value never contains a space or '='.
 * Actions: default (Enter on the matched window), button (the matched child
 * button: Alt+mnemonic, or Enter when the caption has none), keys (a comma
 * list such as alt+n,tab,space), text (type value=; replace=1 selects the
 * edit's text first), click (x=,y= in the matched window's client area),
 * wait (until the match exists), gone (until it no longer exists), expect
 * (a child whose caption matches text= must exist: explicit answers for
 * directory pages). Match keys: title= (top-level caption glob), class=
 * (top-level class), button= (child Button caption glob), text= (any child
 * caption glob); optional=1 skips the step after its timeout instead of
 * failing. Globs: '*' and '?', case-insensitive, '&' mnemonics removed.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_TITLE_PROGRAM_H
#define CIUKI_TITLE_PROGRAM_H
#include <stdint.h>
#include <string.h>

#define TP_LINE 640u
#define TP_STEPS 64u
#define TP_KEYS 16u
enum { TP_SEG_INSTALL, TP_SEG_UNINSTALL };
enum { TP_DEFAULT = 1, TP_BUTTON, TP_KEYS_ACTION, TP_TEXT, TP_CLICK, TP_WAIT, TP_GONE, TP_EXPECT };
enum { TP_OPTIONAL = 1, TP_REPLACE = 2 };
struct tp_step {
    uint8_t segment, action, flags, key_count;
    uint16_t timeout_s, settle_ms, repeat;
    int16_t x, y;
    uint16_t keys[TP_KEYS]; /* set-1 code | TP_ALT/TP_SHIFT/TP_CTRL */
    char title[96], cls[48], button[64], text[128], value[260];
};
struct tp_program {
    char workload[48], entry[260], arguments[260];
    unsigned choices_default, timeout_s, defaults, idle_ms, max_defaults;
    unsigned restart_accept, uninstall, uninstall_timeout_s, shot_ms;
    unsigned step_count, install_steps, uninstall_steps;
    struct tp_step steps[TP_STEPS];
};
#define TP_ALT 0x1000u
#define TP_SHIFT 0x2000u
#define TP_CTRL 0x4000u
#define TP_CODE(k) ((unsigned)(k) & 0x1ffu)

static inline int tp_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* Percent-decoding in place; returns 0 on a malformed escape or NUL byte. */
static inline int tp_decode(char *s, unsigned cap)
{
    unsigned r = 0, w = 0;
    while (s[r]) {
        unsigned c = (unsigned char)s[r++];
        if (c == '%') {
            int v = 0;
            for (int i = 0; i < 2; i++) {
                int d = s[r];
                d = d >= '0' && d <= '9' ? d - '0' : d >= 'a' && d <= 'f' ? d - 'a' + 10 :
                    d >= 'A' && d <= 'F' ? d - 'A' + 10 : -1;
                if (d < 0) return 0;
                v = v * 16 + d; r++;
            }
            if (!v) return 0;
            c = (unsigned)v;
        }
        if (w + 1 >= cap) return 0;
        s[w++] = (char)c;
    }
    s[w] = 0;
    return 1;
}
static inline int tp_copy(char *dst, unsigned cap, const char *src)
{
    unsigned n = (unsigned)strlen(src);
    if (n >= cap) return 0;
    memcpy(dst, src, n + 1);
    return tp_decode(dst, cap);
}
static inline int tp_number(const char *s, unsigned max, unsigned *out)
{
    unsigned v = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (unsigned)(*s - '0');
        if (v > max) return 0;
    }
    *out = v;
    return 1;
}
static inline int tp_signed(const char *s, int limit, int16_t *out)
{
    unsigned v;
    int negative = *s == '-';
    if (!tp_number(s + negative, (unsigned)limit, &v)) return 0;
    *out = (int16_t)(negative ? -(int)v : (int)v);
    return 1;
}
/* US PC layout, scan code set 1 (the desktop's CIUKI_INPUT_KEY codes; extended
 * keys carry 0x100 as in apps/desktop/input.c). */
static inline unsigned tp_named_key(const char *name)
{
    static const struct { const char *name; uint16_t code; } names[] = {
        {"enter", 0x1c}, {"tab", 0x0f}, {"space", 0x39}, {"escape", 0x01}, {"backspace", 0x0e},
        {"up", 0x148}, {"down", 0x150}, {"left", 0x14b}, {"right", 0x14d}, {"home", 0x147},
        {"end", 0x14f}, {"delete", 0x153}, {"pageup", 0x149}, {"pagedown", 0x151},
        {"f1", 0x3b}, {"f2", 0x3c}, {"f3", 0x3d}, {"f4", 0x3e}, {"f5", 0x3f}, {"f6", 0x40},
        {"f7", 0x41}, {"f8", 0x42}, {"f9", 0x43}, {"f10", 0x44}, {"f11", 0x57}, {"f12", 0x58}};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i].name)) return names[i].code;
    return 0;
}
/* Printable ASCII to code | TP_SHIFT; 0 when the US layout cannot type it. */
static inline unsigned tp_char_key(unsigned c)
{
    static const char row1[] = "1234567890-=", row1s[] = "!@#$%^&*()_+";
    static const char qrow[] = "qwertyuiop[]", arow[] = "asdfghjkl;'", zrow[] = "zxcvbnm,./";
    static const char qrows[] = "QWERTYUIOP{}", arows[] = "ASDFGHJKL:\"", zrows[] = "ZXCVBNM<>?";
    if (c == ' ') return 0x39;
    if (c == '`') return 0x29;
    if (c == '~') return 0x29 | TP_SHIFT;
    if (c == '\\') return 0x2b;
    if (c == '|') return 0x2b | TP_SHIFT;
    for (unsigned i = 0; i < 12; i++) {
        if ((unsigned char)row1[i] == c) return 0x02 + i;
        if ((unsigned char)row1s[i] == c) return (0x02 + i) | TP_SHIFT;
        if ((unsigned char)qrow[i] == c) return 0x10 + i;
        if ((unsigned char)qrows[i] == c) return (0x10 + i) | TP_SHIFT;
    }
    for (unsigned i = 0; i < 11; i++) {
        if ((unsigned char)arow[i] == c) return 0x1e + i;
        if ((unsigned char)arows[i] == c) return (0x1e + i) | TP_SHIFT;
    }
    for (unsigned i = 0; i < 10; i++) {
        if ((unsigned char)zrow[i] == c) return 0x2c + i;
        if ((unsigned char)zrows[i] == c) return (0x2c + i) | TP_SHIFT;
    }
    return 0;
}
/* "alt+n", "shift+tab", "ctrl+a", "enter", "x": one chord. */
static inline unsigned tp_chord(const char *text)
{
    unsigned modifiers = 0;
    for (;;) {
        if (!strncmp(text, "alt+", 4)) { modifiers |= TP_ALT; text += 4; }
        else if (!strncmp(text, "shift+", 6)) { modifiers |= TP_SHIFT; text += 6; }
        else if (!strncmp(text, "ctrl+", 5)) { modifiers |= TP_CTRL; text += 5; }
        else break;
    }
    unsigned code = tp_named_key(text);
    if (!code && text[0] && !text[1]) {
        code = tp_char_key((unsigned char)text[0]);
        if (code & TP_SHIFT) return 0; /* a shifted character is written shift+<base> */
    }
    return code ? code | modifiers : 0;
}
static inline int tp_keys(struct tp_step *s, char *list)
{
    s->key_count = 0;
    while (*list) {
        char *comma = strchr(list, ',');
        if (comma) *comma = 0;
        unsigned key = tp_chord(list);
        if (!key || s->key_count == TP_KEYS) return 0;
        s->keys[s->key_count++] = (uint16_t)key;
        if (!comma) break;
        list = comma + 1;
    }
    return s->key_count != 0;
}
static inline int tp_action(const char *name)
{
    static const char *const names[] = {"", "default", "button", "keys", "text", "click", "wait", "gone", "expect"};
    for (unsigned i = 1; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i])) return (int)i;
    return 0;
}
/* Split one line into at most 24 space-separated words, in place. */
static inline unsigned tp_words(char *line, char **words, unsigned cap)
{
    unsigned n = 0;
    for (char *p = line; *p;) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        if (n == cap) return cap + 1;
        words[n++] = p;
        while (*p && *p != ' ') p++;
    }
    return n;
}
static inline int tp_step_parse(struct tp_program *p, char **w, unsigned n)
{
    if (n < 3 || p->step_count == TP_STEPS) return 0;
    struct tp_step *s = &p->steps[p->step_count];
    memset(s, 0, sizeof(*s));
    if (!strcmp(w[1], "install")) s->segment = TP_SEG_INSTALL;
    else if (!strcmp(w[1], "uninstall")) s->segment = TP_SEG_UNINSTALL;
    else return 0;
    if (!(s->action = (uint8_t)tp_action(w[2]))) return 0;
    s->timeout_s = 60; s->settle_ms = 500; s->repeat = 1;
    unsigned seen = 0;
    for (unsigned i = 3; i < n; i++) {
        char *eq = strchr(w[i], '=');
        if (!eq) return 0;
        *eq = 0;
        const char *k = w[i], *v = eq + 1;
        unsigned number, bit;
        static const char *const keys[] = {"title", "class", "button", "text", "value", "keys", "x", "y",
                                           "timeout", "settle", "repeat", "optional", "replace"};
        for (bit = 0; bit < sizeof(keys) / sizeof(keys[0]) && strcmp(k, keys[bit]); bit++) {}
        if (bit == sizeof(keys) / sizeof(keys[0]) || (seen & (1u << bit))) return 0;
        seen |= 1u << bit;
        int ok = 1;
        if (!strcmp(k, "title")) ok = tp_copy(s->title, sizeof(s->title), v);
        else if (!strcmp(k, "class")) ok = tp_copy(s->cls, sizeof(s->cls), v);
        else if (!strcmp(k, "button")) ok = tp_copy(s->button, sizeof(s->button), v);
        else if (!strcmp(k, "text")) ok = tp_copy(s->text, sizeof(s->text), v);
        else if (!strcmp(k, "value")) ok = tp_copy(s->value, sizeof(s->value), v);
        else if (!strcmp(k, "keys")) {
            char list[160];
            ok = tp_copy(list, sizeof(list), v) && tp_keys(s, list);
        } else if (!strcmp(k, "x")) ok = tp_signed(v, 4095, &s->x);
        else if (!strcmp(k, "y")) ok = tp_signed(v, 4095, &s->y);
        else if (!strcmp(k, "timeout")) { ok = tp_number(v, 600, &number) && number; s->timeout_s = (uint16_t)number; }
        else if (!strcmp(k, "settle")) { ok = tp_number(v, 30000, &number); s->settle_ms = (uint16_t)number; }
        else if (!strcmp(k, "repeat")) { ok = tp_number(v, 64, &number) && number; s->repeat = (uint16_t)number; }
        else if (!strcmp(k, "optional")) { ok = tp_number(v, 1, &number); if (number) s->flags |= TP_OPTIONAL; }
        else if (!strcmp(k, "replace")) { ok = tp_number(v, 1, &number); if (number) s->flags |= TP_REPLACE; }
        if (!ok) return 0;
    }
    /* Per-action requirements: input actions need a target window. */
    int target = s->title[0] || s->cls[0] || s->button[0] || s->text[0];
    if (s->action == TP_BUTTON && !s->button[0]) return 0;
    if (s->action == TP_KEYS_ACTION && !s->key_count) return 0;
    if (s->action == TP_TEXT && !s->value[0]) return 0;
    if (s->action == TP_CLICK && (!(seen & (1u << 6)) || !(seen & (1u << 7)) || s->x < 0 || s->y < 0)) return 0;
    if (s->action == TP_EXPECT && !s->text[0]) return 0;
    if (s->action != TP_KEYS_ACTION && s->key_count) return 0;
    if (s->action != TP_TEXT && s->value[0]) return 0;
    if (s->action != TP_CLICK && (seen & ((1u << 6) | (1u << 7)))) return 0;
    if (!target && s->action != TP_DEFAULT && s->action != TP_KEYS_ACTION && s->action != TP_TEXT) return 0;
    if (s->segment == TP_SEG_INSTALL) {
        if (p->uninstall_steps) return 0; /* install steps precede uninstall steps */
        p->install_steps++;
    } else p->uninstall_steps++;
    p->step_count++;
    return 1;
}
/* 0 on success, else the 1-based line number of the first error. */
static inline unsigned tp_parse(struct tp_program *p, const char *text, unsigned length)
{
    memset(p, 0, sizeof(*p));
    p->timeout_s = 480; p->idle_ms = 4000; p->max_defaults = 40; p->shot_ms = 1500;
    p->uninstall_timeout_s = 240;
    unsigned line = 0, at = 0, header = 0, ended = 0, entry = 0, workload = 0;
    while (at < length) {
        char buffer[TP_LINE];
        unsigned n = 0;
        line++;
        while (at < length && text[at] != '\n') {
            if (n + 1 >= sizeof(buffer) || !text[at]) return line;
            buffer[n++] = text[at++];
        }
        if (at < length) at++;
        if (n && buffer[n - 1] == '\r') n--;
        buffer[n] = 0;
        if (!n) continue;
        if (ended) return line;
        char *w[24];
        unsigned count = tp_words(buffer, w, 24);
        if (!count || count > 24) return line;
        if (!header) {
            if (count != 2 || strcmp(w[0], "CIUKIOP") || strcmp(w[1], "1")) return line;
            header = 1; continue;
        }
        unsigned v;
        if (!strcmp(w[0], "end") && count == 1) ended = 1;
        else if (!strcmp(w[0], "workload") && count == 2 && !workload) {
            if (!tp_copy(p->workload, sizeof(p->workload), w[1])) return line;
            workload = 1;
        } else if (!strcmp(w[0], "choices") && count == 2) {
            if (strcmp(w[1], "default")) return line;
            p->choices_default = 1;
        } else if (!strcmp(w[0], "entry") && count == 2 && !entry) {
            if (!tp_copy(p->entry, sizeof(p->entry), w[1]) || p->entry[0] == '\\' || p->entry[0] == '/' ||
                strchr(p->entry, ':') || strstr(p->entry, "..")) return line;
            entry = 1;
        } else if (!strcmp(w[0], "arguments") && count <= 2) {
            if (count == 2 && !tp_copy(p->arguments, sizeof(p->arguments), w[1])) return line;
        } else if (!strcmp(w[0], "timeout") && count == 2) {
            if (!tp_number(w[1], 600, &p->timeout_s) || !p->timeout_s) return line;
        } else if (!strcmp(w[0], "shot") && count == 2) {
            if (!tp_number(w[1], 10000, &p->shot_ms)) return line;
        } else if (!strcmp(w[0], "defaults") && count >= 2 && count <= 4) {
            if (!tp_number(w[1], 1, &p->defaults)) return line;
            for (unsigned i = 2; i < count; i++) {
                if (!strncmp(w[i], "idle=", 5)) { if (!tp_number(w[i] + 5, 60000, &p->idle_ms) || p->idle_ms < 500) return line; }
                else if (!strncmp(w[i], "max=", 4)) { if (!tp_number(w[i] + 4, 200, &p->max_defaults)) return line; }
                else return line;
            }
        } else if (!strcmp(w[0], "restart") && count == 2) {
            if (!strcmp(w[1], "accept")) p->restart_accept = 1;
            else if (strcmp(w[1], "decline")) return line;
        } else if (!strcmp(w[0], "uninstall") && (count == 2 || count == 3)) {
            if (!tp_number(w[1], 1, &p->uninstall)) return line;
            if (count == 3 && (strncmp(w[2], "timeout=", 8) || !tp_number(w[2] + 8, 600, &v) || !v)) return line;
            if (count == 3) p->uninstall_timeout_s = v;
        } else if (!strcmp(w[0], "step")) {
            if (!tp_step_parse(p, w, count)) return line;
        } else return line;
    }
    if (!header || !ended || !entry || !workload || !p->choices_default) return line + 1;
    if (p->uninstall_steps && !p->uninstall) return line + 1;
    return 0;
}

/* ---- window matching over a copied w32srv window directory snapshot ---- */
#define TP_WS_VISIBLE 0x10000000u
#define TP_WS_DISABLED 0x08000000u
struct tp_window {
    uint32_t hwnd, pid, parent, style;
    char cls[64], title[160];
};
/* Caption without '&' mnemonic markers ("&&" keeps one '&'), trimmed. */
static inline void tp_caption(const char *in, char *out, unsigned cap)
{
    unsigned n = 0;
    for (const char *s = in; *s && n + 1 < cap; s++) {
        if (*s == '&') {
            if (s[1] != '&') continue;
            s++;
        }
        out[n++] = *s;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
    unsigned lead = 0;
    while (out[lead] == ' ') lead++;
    if (lead) memmove(out, out + lead, n - lead + 1);
}
/* The mnemonic letter of a caption ("&Next >" -> 'n'), 0 if none. */
static inline unsigned tp_mnemonic(const char *s)
{
    for (; *s; s++)
        if (*s == '&') {
            if (s[1] == '&') { s++; continue; }
            return (unsigned)tp_lower((unsigned char)s[1]);
        }
    return 0;
}
static inline int tp_glob(const char *pattern, const char *text)
{
    const char *star = 0, *resume = 0;
    while (*text) {
        if (*pattern == '*') { star = pattern++; resume = text; continue; }
        if (*pattern && (*pattern == '?' || tp_lower((unsigned char)*pattern) == tp_lower((unsigned char)*text))) {
            pattern++; text++; continue;
        }
        if (!star) return 0;
        pattern = star + 1; text = ++resume;
    }
    while (*pattern == '*') pattern++;
    return !*pattern;
}
static inline int tp_caption_match(const char *pattern, const char *raw)
{
    char caption[160];
    tp_caption(raw, caption, sizeof(caption));
    return tp_glob(pattern, caption);
}
static inline int tp_tracked(const uint32_t *pids, unsigned count, uint32_t pid)
{
    for (unsigned i = 0; i < count; i++) if (pids[i] == pid) return 1;
    return 0;
}
/* Top-level window index owning w (following parents), or -1. */
static inline int tp_top(const struct tp_window *w, unsigned count, unsigned index)
{
    unsigned at = index;
    for (unsigned depth = 0; depth <= count; depth++) {
        if (!w[at].parent) return (int)at;
        unsigned next = count;
        for (unsigned i = 0; i < count; i++) if (w[i].hwnd == w[at].parent) { next = i; break; }
        if (next == count) return -1;
        at = next;
    }
    return -1;
}
static inline int tp_usable(const struct tp_window *w)
{
    return (w->style & TP_WS_VISIBLE) && !(w->style & TP_WS_DISABLED);
}
/* The first (newest, directory order) visible top-level window of a tracked
 * process matching the step; *child receives the matched button or text
 * child (or -1). Returns the window index or -1. */
static inline int tp_match(const struct tp_step *s, const struct tp_window *w, unsigned count,
                           const uint32_t *pids, unsigned pid_count, int *child)
{
    *child = -1;
    for (unsigned i = 0; i < count; i++) {
        if (w[i].parent || !(w[i].style & TP_WS_VISIBLE) || !tp_tracked(pids, pid_count, w[i].pid)) continue;
        if (s->title[0] && !tp_caption_match(s->title, w[i].title)) continue;
        if (s->cls[0] && !tp_glob(s->cls, w[i].cls)) continue;
        int found = -1;
        if (s->button[0] || s->text[0]) {
            for (unsigned j = 0; j < count && found < 0; j++) {
                if (j == i || !w[j].parent || tp_top(w, count, j) != (int)i) continue;
                if (s->button[0]) {
                    if (!tp_glob("button", w[j].cls) || !tp_usable(&w[j]) || !tp_caption_match(s->button, w[j].title)) continue;
                } else if (!tp_caption_match(s->text, w[j].title)) continue;
                found = (int)j;
            }
            if (found < 0) continue;
        }
        *child = found;
        return (int)i;
    }
    return -1;
}
/* Interactive defaults: the caption order a person follows through a setup
 * wizard. Never Cancel/Back/No/Exit/Abort/Browse. */
static inline int tp_default_button(const struct tp_window *w, unsigned count, unsigned top)
{
    static const char *const order[] = {"finish", "install*", "next*", "yes", "i agree", "agree", "accept",
                                        "i accept*", "ok", "continue", "close", "done", "start*", "setup"};
    for (unsigned k = 0; k < sizeof(order) / sizeof(order[0]); k++)
        for (unsigned j = 0; j < count; j++) {
            if (!w[j].parent || tp_top(w, count, j) != (int)top || !tp_glob("button", w[j].cls) || !tp_usable(&w[j])) continue;
            if (tp_caption_match(order[k], w[j].title)) return (int)j;
        }
    return -1;
}
#endif
