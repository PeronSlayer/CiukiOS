/* G1: POSIX TZ rules, civil calendar and the clock.cfg codec (ciuki/tzrule.h).
 * No library calls: the same file is compiled into the kernel, libciuki,
 * kernel32 and the host tests.
 * Civil conversion: H. Hinnant, "chrono-Compatible Low-Level Date
 * Algorithms" (days_from_civil / civil_from_days), public domain,
 * https://howardhinnant.github.io/date_algorithms.html
 * SPDX-License-Identifier: MIT */
#include <ciuki/tzrule.h>

int64_t tzr_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) && ((a < 0) != (b < 0))) q--;
    return q;
}

bool tzr_leap(int64_t y) { return !(y % 4) && ((y % 100) || !(y % 400)); }

unsigned tzr_month_days(int64_t year, unsigned month)
{
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (!month || month > 12) return 0;
    return days[month - 1] + (month == 2 && tzr_leap(year));
}

int64_t tzr_days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = tzr_floor_div(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (int64_t)(m > 2 ? m - 3 : m + 9) + 2) / 5 + (int64_t)d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void tzr_civil_from_days(int64_t z, int64_t *year, unsigned *month, unsigned *day)
{
    z += 719468;
    int64_t era = tzr_floor_div(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    unsigned d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    unsigned m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *year = y + (m <= 2);
    *month = m;
    *day = d;
}

unsigned tzr_weekday(int64_t days)
{
    int64_t w = (days + 4) % 7;
    return (unsigned)(w < 0 ? w + 7 : w);
}

/* ---- parser ---- */
static bool tzr_digit(char c) { return c >= '0' && c <= '9'; }
static bool tzr_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

static bool tzr_tz_name(const char **cursor, char *out)
{
    const char *p = *cursor;
    unsigned n = 0;
    if (*p == '<') {
        p++;
        while (*p && *p != '>') {
            if (!tzr_alpha(*p) && !tzr_digit(*p) && *p != '+' && *p != '-') return false;
            if (n + 1 >= TZR_NAME) return false;
            out[n++] = *p++;
        }
        if (*p != '>') return false;
        p++;
    } else {
        while (tzr_alpha(*p)) {
            if (n + 1 >= TZR_NAME) return false;
            out[n++] = *p++;
        }
    }
    if (n < 3) return false;
    out[n] = 0;
    *cursor = p;
    return true;
}

static bool tzr_number(const char **cursor, unsigned max_digits, unsigned limit, unsigned *out)
{
    const char *p = *cursor;
    unsigned value = 0, n = 0;
    while (tzr_digit(*p) && n < max_digits) { value = value * 10 + (unsigned)(*p++ - '0'); n++; }
    if (!n || value > limit) return false;
    *out = value;
    *cursor = p;
    return true;
}

/* [+-]hh[:mm[:ss]] in seconds, hours up to max_hours. */
static bool tzr_clock_value(const char **cursor, unsigned max_hours, int32_t *out)
{
    const char *p = *cursor;
    int sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }
    unsigned h, m = 0, s = 0;
    if (!tzr_number(&p, 3, max_hours, &h)) return false;
    if (*p == ':') {
        p++;
        if (!tzr_number(&p, 2, 59, &m)) return false;
        if (*p == ':') { p++; if (!tzr_number(&p, 2, 59, &s)) return false; }
    }
    *out = sign * (int32_t)(h * 3600 + m * 60 + s);
    *cursor = p;
    return true;
}

static bool tzr_when(const char **cursor, struct tzr_when *w)
{
    const char *p = *cursor;
    unsigned a, b, c;
    *w = (struct tzr_when){.seconds = 7200};
    if (*p == 'M') {
        p++;
        if (!tzr_number(&p, 2, 12, &a) || !a || *p++ != '.' || !tzr_number(&p, 1, 5, &b) || !b ||
            *p++ != '.' || !tzr_number(&p, 1, 6, &c)) return false;
        w->kind = TZR_DATE_M; w->month = (uint8_t)a; w->week = (uint8_t)b; w->wday = (uint8_t)c;
    } else if (*p == 'J') {
        p++;
        if (!tzr_number(&p, 3, 365, &a) || !a) return false;
        w->kind = TZR_DATE_J; w->day = (uint16_t)a;
    } else if (tzr_digit(*p)) {
        if (!tzr_number(&p, 3, 365, &a)) return false;
        w->kind = TZR_DATE_N; w->day = (uint16_t)a;
    } else return false;
    if (*p == '/') {
        p++;
        if (!tzr_clock_value(&p, 167, &w->seconds)) return false;
    }
    *cursor = p;
    return true;
}

void tzr_utc(struct tzr *out)
{
    *out = (struct tzr){.std = {'U', 'T', 'C', 0}};
}

static bool tzr_parse_prefix(const char **cursor, struct tzr *out)
{
    const char *p = *cursor;
    int32_t value;
    if (!tzr_tz_name(&p, out->std) || !tzr_clock_value(&p, 24, &value)) return false;
    out->std_offset = -value;
    *cursor = p;
    return true;
}

bool tzr_parse(const char *text, struct tzr *out)
{
    if (!text || !out) return false;
    unsigned length = 0;
    while (text[length]) if (++length >= TZR_TEXT) return false;
    struct tzr r = {0};
    const char *p = text;
    if (!tzr_parse_prefix(&p, &r)) return false;
    if (!*p) { *out = r; return true; }
    if (!tzr_tz_name(&p, r.dst)) return false;
    r.has_dst = true;
    r.dst_offset = r.std_offset + 3600;
    if (*p && *p != ',') {
        int32_t value;
        if (!tzr_clock_value(&p, 24, &value)) return false;
        r.dst_offset = -value;
    }
    if (!*p) {
        /* No rule: the US rule glibc's posixrules default uses. */
        r.start = (struct tzr_when){TZR_DATE_M, 3, 2, 0, 0, 7200};
        r.end = (struct tzr_when){TZR_DATE_M, 11, 1, 0, 0, 7200};
    } else {
        if (*p++ != ',' || !tzr_when(&p, &r.start) || *p++ != ',' || !tzr_when(&p, &r.end) || *p) return false;
    }
    if (r.dst_offset == r.std_offset) return false;
    *out = r;
    return true;
}

size_t tzr_standard_only(const char *text, char *out, size_t cap)
{
    struct tzr r;
    if (!text || !out || !tzr_parse(text, &r)) return 0;
    const char *p = text;
    if (!tzr_parse_prefix(&p, &r)) return 0;
    size_t n = (size_t)(p - text);
    if (n + 1 > cap) return 0;
    for (size_t i = 0; i < n; i++) out[i] = text[i];
    out[n] = 0;
    return n;
}

/* ---- evaluation ---- */
static int64_t tzr_change_local(const struct tzr_when *w, int64_t year)
{
    int64_t days = tzr_days_from_civil(year, 1, 1);
    if (w->kind == TZR_DATE_M) {
        int64_t first = tzr_days_from_civil(year, w->month, 1);
        unsigned d = 1 + (w->wday + 7 - tzr_weekday(first)) % 7 + 7u * (w->week - 1u);
        while (d > tzr_month_days(year, w->month)) d -= 7;
        days = first + d - 1;
    } else if (w->kind == TZR_DATE_J) {
        days += w->day - 1 + (tzr_leap(year) && w->day >= 60);
    } else {
        days += w->day;
    }
    return days * 86400 + w->seconds;
}

void tzr_changes(const struct tzr *r, int64_t year, int64_t *start, int64_t *end)
{
    *start = tzr_change_local(&r->start, year) - r->std_offset;
    *end = tzr_change_local(&r->end, year) - r->dst_offset;
}

static int64_t tzr_year_of(int64_t seconds)
{
    int64_t y; unsigned m, d;
    tzr_civil_from_days(tzr_floor_div(seconds, 86400), &y, &m, &d);
    return y;
}

int32_t tzr_offset(const struct tzr *r, int64_t utc, bool *dst)
{
    bool in = false;
    if (r->has_dst) {
        int64_t start, end;
        tzr_changes(r, tzr_year_of(utc + r->std_offset), &start, &end);
        in = start < end ? utc >= start && utc < end : !(utc >= end && utc < start);
    }
    if (dst) *dst = in;
    return in ? r->dst_offset : r->std_offset;
}

int64_t tzr_local_to_utc(const struct tzr *r, int64_t local, int isdst, bool *dst)
{
    if (!r->has_dst) { if (dst) *dst = false; return local - r->std_offset; }
    int64_t as_std = local - r->std_offset, as_dst = local - r->dst_offset;
    bool d1, d2;
    tzr_offset(r, as_std, &d1);
    tzr_offset(r, as_dst, &d2);
    bool std_ok = !d1, dst_ok = d2, use_dst;
    if (isdst > 0) use_dst = true;
    else if (isdst == 0) use_dst = false;
    else if (std_ok && dst_ok) use_dst = true;      /* repeated hour: first occurrence */
    else if (dst_ok) use_dst = true;
    else use_dst = false;                           /* valid standard time, or the skipped hour */
    if (dst) *dst = use_dst;
    return use_dst ? as_dst : as_std;
}

bool tzr_next_change(const struct tzr *r, int64_t utc, int64_t *at)
{
    if (!r->has_dst) return false;
    int64_t y = tzr_year_of(utc + r->std_offset), best = 0;
    bool found = false;
    for (int64_t k = y - 1; k <= y + 1; k++) {
        int64_t s, e;
        tzr_changes(r, k, &s, &e);
        if (s > utc && (!found || s < best)) { best = s; found = true; }
        if (e > utc && (!found || e < best)) { best = e; found = true; }
    }
    if (found) *at = best;
    return found;
}

/* ---- clock.cfg ---- */
static size_t tzr_put(char *out, size_t cap, size_t at, const char *s)
{
    while (*s) { if (at + 1 < cap) out[at] = *s; at++; s++; }
    return at;
}
static size_t tzr_put_int(char *out, size_t cap, size_t at, int64_t v)
{
    char digits[24];
    unsigned n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { digits[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) at = tzr_put(out, cap, at, "-");
    while (n) { char c[2] = {digits[--n], 0}; at = tzr_put(out, cap, at, c); }
    return at;
}

size_t tzr_fixed_text(int32_t minutes, char *out, size_t cap)
{
    size_t at = 0;
    if (!minutes) at = tzr_put(out, cap, 0, "UTC0");
    else {
        int32_t a = minutes < 0 ? -minutes : minutes;
        char name[8] = {'<', minutes < 0 ? '-' : '+', (char)('0' + a / 600), (char)('0' + a / 60 % 10), 0};
        at = tzr_put(out, cap, at, name);
        if (a % 60) { char mm[3] = {(char)('0' + a % 60 / 10), (char)('0' + a % 10), 0}; at = tzr_put(out, cap, at, mm); }
        at = tzr_put(out, cap, at, ">");
        at = tzr_put(out, cap, at, minutes < 0 ? "" : "-");
        at = tzr_put_int(out, cap, at, a / 60);
        if (a % 60) { at = tzr_put(out, cap, at, ":"); char mm[3] = {(char)('0' + a % 60 / 10), (char)('0' + a % 10), 0}; at = tzr_put(out, cap, at, mm); }
    }
    if (at >= cap) { if (cap) out[0] = 0; return 0; }
    out[at] = 0;
    return at;
}

void tzr_config_default(struct tzr_config *c)
{
    *c = (struct tzr_config){.version = 2, .auto_dst = true};
    tzr_fixed_text(0, c->tz, sizeof(c->tz));
}

static bool tzr_same(const char *a, size_t n, const char *b)
{
    size_t i = 0;
    for (; i < n; i++) if (!b[i] || a[i] != b[i]) return false;
    return !b[i];
}
static bool tzr_parse_int(const char *v, size_t n, int32_t lo, int32_t hi, int32_t *out)
{
    size_t i = 0;
    int sign = 1;
    int64_t value = 0;
    if (i < n && v[i] == '-') { sign = -1; i++; }
    if (i == n || n - i > 6) return false;
    for (; i < n; i++) { if (!tzr_digit(v[i])) return false; value = value * 10 + (v[i] - '0'); }
    value *= sign;
    if (value < lo || value > hi) return false;
    *out = (int32_t)value;
    return true;
}
static bool tzr_copy_text(const char *v, size_t n, char *out, size_t cap, bool zone)
{
    if (n + 1 > cap) return false;
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (zone ? !(tzr_alpha(c) || tzr_digit(c) || c == '/' || c == '_' || c == '-' || c == '+')
                 : (c < 33 || c > 126)) return false;
        out[i] = c;
    }
    out[n] = 0;
    return true;
}

bool tzr_config_parse(const char *text, size_t length, struct tzr_config *out)
{
    struct tzr_config c;
    tzr_config_default(&c);
    c.version = 0;
    bool have_local = false, have_offset = false, have_tz = false, have_bias = false;
    size_t at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n') end++;
        if (end == length) return false;              /* every line ends with LF */
        size_t eq = at;
        while (eq < end && text[eq] != '=') eq++;
        if (eq == end) return false;
        const char *k = text + at, *v = text + eq + 1;
        size_t kn = eq - at, vn = end - eq - 1;
        int32_t value;
        if (tzr_same(k, kn, "version")) {
            if (!tzr_parse_int(v, vn, 1, 2, &value)) return false;
            c.version = (uint32_t)value;
        } else if (tzr_same(k, kn, "rtc_local")) {
            if (!tzr_parse_int(v, vn, 0, 1, &value)) return false;
            c.rtc_local = value; have_local = true;
        } else if (tzr_same(k, kn, "offset_minutes")) {
            if (!tzr_parse_int(v, vn, -720, 840, &c.offset_minutes)) return false;
            have_offset = true;
        } else if (tzr_same(k, kn, "auto_dst")) {
            if (!tzr_parse_int(v, vn, 0, 1, &value)) return false;
            c.auto_dst = value;
        } else if (tzr_same(k, kn, "rtc_bias_minutes")) {
            if (!tzr_parse_int(v, vn, -1440, 1440, &c.rtc_bias_minutes)) return false;
            have_bias = true;
        } else if (tzr_same(k, kn, "zone")) {
            if (!tzr_copy_text(v, vn, c.zone, sizeof(c.zone), true)) return false;
        } else if (tzr_same(k, kn, "tz")) {
            if (!tzr_copy_text(v, vn, c.tz, sizeof(c.tz), false)) return false;
            have_tz = true;
        }                                              /* unknown keys: later versions */
        at = end + 1;
    }
    if (!c.version || !have_local) return false;
    if (c.version == 1) {
        if (!have_offset) return false;
        c.zone[0] = 0; c.auto_dst = true;
        tzr_fixed_text(c.offset_minutes, c.tz, sizeof(c.tz));
        c.rtc_bias_minutes = c.rtc_local ? c.offset_minutes : 0;
        c.version = 2;
    } else {
        struct tzr r;
        if (!have_tz || !tzr_parse(c.tz, &r)) return false;
        if (!have_offset) c.offset_minutes = r.std_offset / 60;
        if (!c.rtc_local) c.rtc_bias_minutes = 0;
        else if (!have_bias) c.rtc_bias_minutes = r.std_offset / 60;
    }
    *out = c;
    return true;
}

size_t tzr_config_format(const struct tzr_config *c, char *out, size_t cap)
{
    size_t at = 0;
    at = tzr_put(out, cap, at, "version=2\nrtc_local=");
    at = tzr_put_int(out, cap, at, c->rtc_local);
    at = tzr_put(out, cap, at, "\noffset_minutes=");
    at = tzr_put_int(out, cap, at, c->offset_minutes);
    at = tzr_put(out, cap, at, "\nzone=");
    at = tzr_put(out, cap, at, c->zone);
    at = tzr_put(out, cap, at, "\ntz=");
    at = tzr_put(out, cap, at, c->tz);
    at = tzr_put(out, cap, at, "\nauto_dst=");
    at = tzr_put_int(out, cap, at, c->auto_dst);
    at = tzr_put(out, cap, at, "\nrtc_bias_minutes=");
    at = tzr_put_int(out, cap, at, c->rtc_local ? c->rtc_bias_minutes : 0);
    at = tzr_put(out, cap, at, "\n");
    if (at >= cap) { if (cap) out[0] = 0; return 0; }
    out[at] = 0;
    return at;
}

bool tzr_config_rule(const struct tzr_config *c, struct tzr *out)
{
    if (!tzr_parse(c->tz, out)) return false;
    if (!c->auto_dst) out->has_dst = false;
    return true;
}
