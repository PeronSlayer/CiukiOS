/* fx-selftest model: the same sequencing, bounds and text/record format on
 * the host and in the kernel (docs/design/self-test.md, "Report format").
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/selftest.h>

static const char *const reserved_keys[] = { "v", "run", "seq", "probe", "event", "check", "status", "ms", "part" };

const char *selftest_status_text(enum selftest_status s)
{ return s == SELFTEST_PASS ? "PASS" : s == SELFTEST_SKIP ? "SKIP" : "FAIL"; }
static const char *status_word(enum selftest_status s)
{ return s == SELFTEST_PASS ? "pass" : s == SELFTEST_SKIP ? "skip" : "fail"; }
static bool value_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == ':' || c == '.' || c == ',' || c == '/' || c == '+' || c == '-';
}
static bool key_char(char c, bool first)
{ return (c >= 'a' && c <= 'z') || (!first && ((c >= '0' && c <= '9') || c == '_')); }
static size_t text_len(const char *s, size_t max)
{ size_t n = 0; while (n < max && s[n]) n++; return n; }

/* Append "key=value" (space separated) only when the whole token fits. */
void selftest_token(char *figures, size_t size, const char *key, const char *value)
{
    if (!figures || !size || !key || !*key) return;
    size_t used = text_len(figures, size), k = text_len(key, 32), v = value ? text_len(value, 96) : 0;
    size_t need = (used ? 1 : 0) + k + 1 + (v ? v : 1);
    if (used + need >= size) return;
    char *p = figures + used;
    if (used) *p++ = ' ';
    for (size_t i = 0; i < k; i++) *p++ = key_char(key[i], !i) ? key[i] : '_';
    *p++ = '=';
    if (!v) *p++ = '-';
    for (size_t i = 0; i < v; i++) *p++ = value_char(value[i]) ? value[i] : '_';
    *p = 0;
}

static const char truncated_token[] = "truncated=1";
/* Drop trailing whole tokens until extra more bytes fit after them. Tokens
 * are never cut, so a cut value can never fake another. */
static size_t cut_to_fit(char *figures, size_t size, size_t extra)
{
    size_t used = text_len(figures, size);
    while (used && used + extra >= size) {
        while (used && figures[used - 1] != ' ') used--;
        if (used) used--;
        figures[used] = 0;
    }
    return used;
}
static void put(char *figures, size_t *used, const char *text, size_t n)
{
    if (*used) figures[(*used)++] = ' ';
    memcpy(figures + *used, text, n + 1);
    *used += n;
}
/* Remove every token with this key (in place); true when one was present. */
static bool remove_token(char *figures, size_t size, const char *key)
{
    size_t k = text_len(key, 32), n = text_len(figures, size), i = 0, out = 0;
    bool found = false;
    while (i < n) {
        while (i < n && figures[i] == ' ') i++;
        size_t start = i;
        while (i < n && figures[i] != ' ') i++;
        size_t len = i - start;
        if (!len) break;
        if (len > k && !strncmp(figures + start, key, k) && figures[start + k] == '=') { found = true; continue; }
        if (out) figures[out++] = ' ';
        memmove(figures + out, figures + start, len);
        out += len;
    }
    figures[out] = 0;
    return found;
}
static void mark_truncated(char *figures, size_t size)
{
    if (selftest_has_token(figures, "truncated")) return;
    size_t n = sizeof(truncated_token) - 1, used = cut_to_fit(figures, size, n + 1);
    put(figures, &used, truncated_token, n);
}
/* Append whole tokens. When they do not fit, trailing figures give way and
 * the truncation marker follows them: a bound or a reason always wins. */
static void append_forced(char *figures, size_t size, const char *text)
{
    size_t add = text_len(text, size), marker = sizeof(truncated_token) - 1;
    if (!add || add + marker + 2 >= size) return;
    bool marked = remove_token(figures, size, "truncated");
    size_t used = text_len(figures, size);
    if (used + 1 + add >= size) marked = true;
    if (marked) used = cut_to_fit(figures, size, add + 1 + marker + 1);
    put(figures, &used, text, add);
    if (marked) put(figures, &used, truncated_token, marker);
}

void selftest_figures(struct selftest_result *r, const char *fmt, ...)
{
    if (!r || !fmt) return;
    char text[SELFTEST_FIGURES];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t used = text_len(r->figures, sizeof(r->figures)), add = text_len(text, sizeof(text));
    /* Whole formatted groups only: a cut token could fake another value. */
    if ((size_t)n >= sizeof(text) || used + (used ? 1 : 0) + add >= sizeof(r->figures)) {
        mark_truncated(r->figures, sizeof(r->figures));
        return;
    }
    if (used) r->figures[used++] = ' ';
    memcpy(r->figures + used, text, add + 1);
}

void selftest_set(struct selftest_result *r, enum selftest_status status, const char *reason)
{
    if (!r) return;
    r->status = status;
    if (!reason) return;
    char token[SELFTEST_ID_MAX + 112] = "";
    selftest_token(token, sizeof(token), "reason", reason);
    append_forced(r->figures, sizeof(r->figures), token);
}

/* True when a token with this key is present (whole key match). */
bool selftest_has_token(const char *figures, const char *key)
{
    size_t k = text_len(key, 32);
    for (const char *p = figures; p && *p;) {
        while (*p == ' ') p++;
        if (!strncmp(p, key, k) && p[k] == '=') return true;
        while (*p && *p != ' ') p++;
    }
    return false;
}

void selftest_sanitize(char *figures, enum selftest_status status)
{
    char out[SELFTEST_FIGURES];
    size_t used = 0;
    out[0] = 0;
    const char *p = figures;
    while (p && *p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - start);
        if (!len) continue;
        size_t eq = 0;
        while (eq < len && start[eq] != '=') eq++;
        bool valid = eq > 0 && eq < len && eq < 32;
        for (size_t i = 0; valid && i < eq; i++) valid = key_char(start[i], !i);
        char key[32];
        if (valid) { memcpy(key, start, eq); key[eq] = 0; }
        for (unsigned i = 0; valid && i < ARRAY_SIZE(reserved_keys); i++)
            if (!strncmp(key, reserved_keys[i], 32)) valid = false;
        if (valid && selftest_has_token(out, key)) valid = false; /* first value wins */
        if (!valid) continue;
        if (used + (used ? 1 : 0) + len >= sizeof(out)) { mark_truncated(out, sizeof(out)); break; }
        if (used) out[used++] = ' ';
        for (size_t i = 0; i < len; i++) out[used++] = i <= eq || value_char(start[i]) ? start[i] : '_';
        out[used] = 0;
    }
    if (status == SELFTEST_SKIP && !selftest_has_token(out, "reason"))
        append_forced(out, sizeof(out), "reason=unspecified");
    memcpy(figures, out, text_len(out, sizeof(out)) + 1);
}

/* Emit prefix + tokens in records of at most SELFTEST_RECORD_FIELDS bytes.
 * Later records repeat check=<id> with part=N (N from 2); a detail line's
 * later records carry part=detail_more, so a reader rebuilds each line. */
static void emit_records(const struct selftest_ops *ops, const char *event, const char *first,
                         const char *id, bool detail, const char *tokens)
{
    if (!ops->record) return;
    char fields[SELFTEST_RECORD_FIELDS + 1];
    unsigned part = 1;
    int n = ksnprintf(fields, sizeof(fields), "%s", first);
    size_t used = n > 0 ? (size_t)n : 0, base = used;
    bool any = false;
    const char *p = tokens;
    while (p && *p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - start);
        if (!len) continue;
        if (len > SELFTEST_RECORD_FIELDS - base - 1) len = SELFTEST_RECORD_FIELDS - base - 1;
        if (used + 1 + len > SELFTEST_RECORD_FIELDS) {
            ops->record(ops->ctx, event, fields);
            any = true;
            part++;
            n = detail ? ksnprintf(fields, sizeof(fields), "check=%s part=detail_more", id)
                       : ksnprintf(fields, sizeof(fields), "check=%s part=%u", id, part);
            used = base = n > 0 ? (size_t)n : 0;
        }
        fields[used++] = ' ';
        memcpy(fields + used, start, len);
        used += len;
        fields[used] = 0;
    }
    if (!any || used > base || !detail) ops->record(ops->ctx, event, fields);
}

void selftest_model_begin(const struct selftest_ops *ops, struct selftest_summary *s,
                          const char *run, const char *build, const char *version)
{
    *s = (struct selftest_summary){ .started = ops->now(ops->ctx) };
    char header[160], fields[SELFTEST_RECORD_FIELDS + 1];
    char safe_build[40] = "", safe_version[40] = "";
    selftest_token(safe_build, sizeof(safe_build), "build", build);
    selftest_token(safe_version, sizeof(safe_version), "os", version);
    ksnprintf(header, sizeof(header), "CiukiOS self-test format=%u run=%s %s %s",
              SELFTEST_VERSION, run, safe_build, safe_version);
    ops->line(ops->ctx, header);
    ksnprintf(fields, sizeof(fields), "format=%u %s %s", SELFTEST_VERSION, safe_build, safe_version);
    if (ops->record) ops->record(ops->ctx, "BEGIN", fields);
}

void selftest_model_run(const struct selftest_ops *ops, const struct selftest_check *checks, unsigned count,
                        struct selftest_result *results, struct selftest_summary *s)
{
    char line[SELFTEST_FIGURES + 64], first[96];
    for (unsigned i = 0; i < count; i++) {
        const struct selftest_check *c = &checks[i];
        struct selftest_result *r = &results[i];
        *r = (struct selftest_result){ .status = SELFTEST_FAIL };
        if (ops->progress) ops->progress(ops->ctx, c->id);
        uint64_t start = ops->now(ops->ctx);
        if (c->run) c->run(ops->ctx, c, start + c->limit_ms, r);
        else { r->status = SELFTEST_SKIP; selftest_figures(r, "reason=not_available"); }
        uint64_t elapsed = ops->now(ops->ctx) - start;
        r->ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
        if (elapsed > c->limit_ms) {
            /* Keep the measured figures of a late check; the bound wins. */
            r->status = SELFTEST_FAIL;
            char extra[48];
            ksnprintf(extra, sizeof(extra), "deadline_exceeded=1 limit_ms=%u", c->limit_ms);
            append_forced(r->figures, sizeof(r->figures), extra);
        }
        selftest_sanitize(r->figures, r->status);
        s->checks++;
        if (r->status == SELFTEST_PASS) s->passed++;
        else if (r->status == SELFTEST_SKIP) s->skipped++;
        else s->failed++;
        ksnprintf(line, sizeof(line), "%s %s ms=%u%s%s", selftest_status_text(r->status), c->id, r->ms,
                  r->figures[0] ? " " : "", r->figures);
        ops->line(ops->ctx, line);
        ksnprintf(first, sizeof(first), "check=%s status=%s ms=%u", c->id, status_word(r->status), r->ms);
        emit_records(ops, "DATA", first, c->id, false, r->figures);
        if (ops->progress) ops->progress(ops->ctx, 0);
    }
}

void selftest_model_detail(const struct selftest_ops *ops, char *details, size_t size, size_t *used,
                           const char *id, const char *tokens)
{
    char clean[SELFTEST_FIGURES];
    size_t n = text_len(tokens, sizeof(clean) - 1);
    memcpy(clean, tokens, n); clean[n] = 0;
    selftest_sanitize(clean, SELFTEST_PASS);
    if (details && used && *used < size) {
        int w = ksnprintf(details + *used, size - *used, "%s %s\n", id, clean);
        /* A full buffer keeps whole lines only; the records still carry it. */
        if (w > 0 && *used + (size_t)w < size) *used += (size_t)w;
        else details[*used] = 0;
    }
    char first[64];
    ksnprintf(first, sizeof(first), "check=%s part=detail", id);
    emit_records(ops, "DATA", first, id, true, clean);
}

void selftest_notice(char *out, size_t size, const struct selftest_summary *s)
{
    ksnprintf(out, size, "Self-test complete: %u passed, %u failed, %u skipped", s->passed, s->failed, s->skipped);
}

void selftest_model_end(const struct selftest_ops *ops, struct selftest_summary *s, const char *details)
{
    s->ms = ops->now(ops->ctx) - s->started;
    char line[160];
    selftest_notice(line, sizeof(line), s);
    ops->line(ops->ctx, line);
    ksnprintf(line, sizeof(line), "SUMMARY passed=%u failed=%u skipped=%u checks=%u ms=%llu",
              s->passed, s->failed, s->skipped, s->checks, (unsigned long long)s->ms);
    ops->line(ops->ctx, line);
    ops->line(ops->ctx, "");
    ops->line(ops->ctx, "Details:");
    for (const char *p = details; p && *p;) {
        char row[SELFTEST_FIGURES + SELFTEST_ID_MAX + 8];
        size_t n = 0;
        while (p[n] && p[n] != '\n') n++;
        size_t copy = n < sizeof(row) - 1 ? n : sizeof(row) - 1;
        memcpy(row, p, copy); row[copy] = 0;
        ops->line(ops->ctx, row);
        p += n;
        if (*p == '\n') p++;
    }
    char fields[SELFTEST_RECORD_FIELDS + 1];
    ksnprintf(fields, sizeof(fields), "status=%s passed=%u failed=%u skipped=%u checks=%u ms=%llu",
              s->failed ? "FAIL" : "PASS", s->passed, s->failed, s->skipped, s->checks,
              (unsigned long long)s->ms);
    if (ops->record) ops->record(ops->ctx, "END", fields);
}
