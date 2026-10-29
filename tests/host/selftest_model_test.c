/* fx-selftest model (core/selftest_model.c) on the host: check sequencing,
 * per-check bounds, figure sanitizing, the SELFTEST.LOG text and the serial
 * record framing. argv[1] receives SELFTEST.LOG and serial.txt for the
 * parser tests (tests/host/test_selftest.py). No kernel, disk or UART.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SELFTEST_HOST 1
#include <ciuki/kernel.h>
#include <ciuki/selftest.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static uint64_t clock_ms = 1000;
static char log_text[16384], serial[32768], trace[1024];
static char details[2048];
static size_t details_used;
static unsigned seq, records;
static const struct selftest_ops ops;

static uint64_t op_now(void *ctx) { (void)ctx; return clock_ms; }
static void append(char *buf, size_t size, const char *s)
{
    size_t used = strlen(buf), n = strlen(s);
    CHECK(used + n + 1 < size);
    memcpy(buf + used, s, n + 1);
}
static void op_line(void *ctx, const char *line)
{
    (void)ctx;
    CHECK(!strchr(line, '\n'));
    append(log_text, sizeof(log_text), line);
    append(log_text, sizeof(log_text), "\n");
}
/* The framing of core/output.c rec_emit: 240 bytes at most per record. */
static void op_record(void *ctx, const char *event, const char *fields)
{
    (void)ctx;
    CHECK(strlen(fields) <= SELFTEST_RECORD_FIELDS);
    char line[300];
    int n = snprintf(line, sizeof(line), "CIUKI_TEST v=1 run=1a2b3c4d seq=%06u probe=selftest event=%s%s%s",
                     ++seq, event, *fields ? " " : "", fields);
    CHECK(n > 0 && n <= 240);
    /* Every token follows the controller grammar. */
    for (const char *p = line + 11; *p;) {
        const char *start = p, *eq = 0;
        while (*p && *p != ' ') { if (*p == '=' && !eq) eq = p; p++; }
        CHECK(eq && eq > start && *start >= 'a' && *start <= 'z' && eq + 1 < p);
        while (*p == ' ') p++;
    }
    append(serial, sizeof(serial), line);
    append(serial, sizeof(serial), "\r\n");
    records++;
}
static void op_progress(void *ctx, const char *id)
{
    (void)ctx;
    append(trace, sizeof(trace), id ? id : "-");
    append(trace, sizeof(trace), ",");
}
static const struct selftest_ops ops = { 0, op_now, op_line, op_record, op_progress };

static void detail(const char *id, const char *tokens)
{ selftest_model_detail(&ops, details, sizeof(details), &details_used, id, tokens); }

#define RUN(name) static void name(void *ctx, const struct selftest_check *c, uint64_t until, struct selftest_result *r)
RUN(run_pass)
{
    (void)ctx; CHECK(until == clock_ms + c->limit_ms);
    clock_ms += 7;
    selftest_figures(r, "count=%u", 3u);
    selftest_token(r->figures, sizeof(r->figures), "label", "ok");
    detail(c->id, "line=0 modes=101:640x480x8,111:640x480x16");
    r->status = SELFTEST_PASS;
}
RUN(run_fail)
{
    (void)ctx; (void)c; (void)until;
    clock_ms += 12;
    selftest_token(r->figures, sizeof(r->figures), "model", "IBM DVD-ROM (x)=y");
    selftest_token(r->figures, sizeof(r->figures), "reason", "probe");
    r->status = SELFTEST_FAIL;
}
RUN(run_skip_unexplained) { (void)ctx; (void)c; (void)until; r->status = SELFTEST_SKIP; }
RUN(run_slow)
{
    (void)ctx; (void)c; (void)until;
    selftest_figures(r, "phase=wait");
    clock_ms += 80;
    r->status = SELFTEST_PASS;
}
RUN(run_reserved)
{
    (void)ctx; (void)c; (void)until;
    selftest_figures(r, "status=pass ms=9 check=x part=2 run=1 seq=1 v=1 probe=a event=b ok=1 ok=2 =bad noeq KEY=x k@y=1 v2=a=b");
    r->status = SELFTEST_PASS;
}
RUN(run_long)
{
    (void)ctx; (void)c; (void)until;
    for (unsigned i = 0; i < 40; i++) selftest_figures(r, "field%02u=%u", i, 1000000u + i);
    CHECK(selftest_has_token(r->figures, "truncated"));
    CHECK(strlen(r->figures) < SELFTEST_FIGURES);
    char long_tokens[SELFTEST_FIGURES] = "";
    for (unsigned i = 0; i < 12; i++) {
        char key[8]; snprintf(key, sizeof(key), "p%u", i);
        selftest_token(long_tokens, sizeof(long_tokens), key, "marker_begin:123456:-5");
    }
    detail(c->id, long_tokens);
    r->status = SELFTEST_FAIL;
}

static const struct selftest_check table[] = {
    { "alpha.pass", 100, run_pass },
    { "beta.fail", 100, run_fail },
    { "gamma.skip", 100, run_skip_unexplained },
    { "delta.none", 100, 0 },
    { "epsilon.slow", 50, run_slow },
    { "zeta.reserved", 100, run_reserved },
    { "eta.long", 100, run_long },
};

static const char *line_of(const char *prefix)
{
    const char *p = strstr(log_text, prefix);
    CHECK(p);
    static char line[512];
    size_t n = strcspn(p, "\n");
    CHECK(n < sizeof(line));
    memcpy(line, p, n); line[n] = 0;
    return line;
}

int main(int argc, char **argv)
{
    /* Token helper: sanitized values, '-' for empty, whole tokens only. */
    char f[24] = "";
    selftest_token(f, sizeof(f), "a", "x y");
    selftest_token(f, sizeof(f), "b", "");
    selftest_token(f, sizeof(f), "Bad", "1");
    CHECK(!strcmp(f, "a=x_y b=- _ad=1"));
    selftest_token(f, sizeof(f), "toolong", "0123456789");
    CHECK(!strcmp(f, "a=x_y b=- _ad=1"));          /* did not fit: not appended */
    char big[200] = "";
    char value[150]; memset(value, 'v', sizeof(value) - 1); value[sizeof(value) - 1] = 0;
    selftest_token(big, sizeof(big), "k", value);
    CHECK(strlen(big) == 2 + 96);                    /* values stop at 96 bytes */
    /* Every fill level keeps the truncation marker, then a late reason. */
    for (unsigned width = 1; width <= 24; width++) {
        struct selftest_result full = { .status = SELFTEST_PASS };
        char pad[32]; memset(pad, 'x', width); pad[width] = 0;
        for (unsigned i = 0; i < 200; i++) selftest_figures(&full, "f%03u=%s", i, pad);
        CHECK(selftest_has_token(full.figures, "truncated"));
        selftest_set(&full, SELFTEST_FAIL, "draw failed");
        CHECK(full.status == SELFTEST_FAIL && strstr(full.figures, " reason=draw_failed"));
        CHECK(selftest_has_token(full.figures, "truncated") && strlen(full.figures) < SELFTEST_FIGURES);
        selftest_sanitize(full.figures, full.status);
        CHECK(selftest_has_token(full.figures, "reason") && selftest_has_token(full.figures, "truncated"));
    }

    struct selftest_summary s;
    selftest_model_begin(&ops, &s, "1a2b3c4d", "12345678", "1.0.0-beta.1");
    CHECK(!strcmp(line_of("CiukiOS self-test"), "CiukiOS self-test format=1 run=1a2b3c4d build=12345678 os=1.0.0-beta.1"));
    struct selftest_result results[ARRAY_SIZE(table)];
    selftest_model_run(&ops, table, ARRAY_SIZE(table), results, &s);

    /* Sequencing: progress names each check before it runs, then clears. */
    CHECK(!strcmp(trace, "alpha.pass,-,beta.fail,-,gamma.skip,-,delta.none,-,epsilon.slow,-,zeta.reserved,-,eta.long,-,"));
    CHECK(s.checks == 7 && s.passed == 2 && s.failed == 3 && s.skipped == 2);
    CHECK(!strcmp(line_of("PASS alpha.pass"), "PASS alpha.pass ms=7 count=3 label=ok"));
    CHECK(!strcmp(line_of("FAIL beta.fail"), "FAIL beta.fail ms=12 model=IBM_DVD-ROM__x__y reason=probe"));
    CHECK(!strcmp(line_of("SKIP gamma.skip"), "SKIP gamma.skip ms=0 reason=unspecified"));
    CHECK(!strcmp(line_of("SKIP delta.none"), "SKIP delta.none ms=0 reason=not_available"));
    /* An overrun is FAIL with its bound, and the measured figures stay. */
    CHECK(results[4].status == SELFTEST_FAIL && results[4].ms == 80);
    CHECK(!strcmp(line_of("FAIL epsilon.slow"), "FAIL epsilon.slow ms=80 phase=wait deadline_exceeded=1 limit_ms=50"));
    /* Reserved, duplicate and malformed keys never reach the report. */
    CHECK(!strcmp(line_of("PASS zeta.reserved"), "PASS zeta.reserved ms=0 ok=1 v2=a_b"));
    CHECK(strstr(line_of("FAIL eta.long"), " truncated=1"));

    selftest_model_end(&ops, &s, details);
    CHECK(s.ms == clock_ms - 1000);
    CHECK(!strcmp(line_of("Self-test complete"), "Self-test complete: 2 passed, 3 failed, 2 skipped"));
    char summary[96];
    snprintf(summary, sizeof(summary), "SUMMARY passed=2 failed=3 skipped=2 checks=7 ms=%llu", (unsigned long long)s.ms);
    CHECK(!strcmp(line_of("SUMMARY"), summary));
    CHECK(strstr(log_text, "\n\nDetails:\nalpha.pass line=0 modes=101:640x480x8,111:640x480x16\neta.long p0="));

    /* Records: BEGIN, one DATA per check (+ continuations), details, END. */
    CHECK(strstr(serial, "event=BEGIN format=1 build=12345678 os=1.0.0-beta.1\r\n"));
    CHECK(strstr(serial, "event=DATA check=alpha.pass status=pass ms=7 count=3 label=ok\r\n"));
    CHECK(strstr(serial, "event=DATA check=eta.long part=2 "));
    CHECK(strstr(serial, "event=DATA check=eta.long part=detail p0="));
    CHECK(strstr(serial, "event=END status=FAIL passed=2 failed=3 skipped=2 checks=7 ms="));
    CHECK(strstr(serial, "event=DATA check=eta.long part=detail_more p5="));          /* split detail */

    /* The notice fits the 80-byte device reason with the largest counts. */
    struct selftest_summary most = { .passed = 4294967295u, .failed = 4294967295u, .skipped = 4294967295u };
    char notice[80];
    selftest_notice(notice, sizeof(notice), &most);
    CHECK(!strcmp(notice, "Self-test complete: 4294967295 passed, 4294967295 failed, 4294967295 skipped"));

    /* A full detail buffer keeps whole lines (the records still carry it). */
    char small[48]; size_t used = 0;
    unsigned before = records;
    selftest_model_detail(&ops, small, sizeof(small), &used, "a.b", "x=1 y=2");
    selftest_model_detail(&ops, small, sizeof(small), &used, "a.b", "z=0123456789012345678901234567890123456789");
    CHECK(!strcmp(small, "a.b x=1 y=2\n") && used == strlen(small) && records == before + 2);

    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof(path), "%s/SELFTEST.LOG", argv[1]);
        FILE *out = fopen(path, "wb"); CHECK(out); fputs(log_text, out); fclose(out);
        snprintf(path, sizeof(path), "%s/serial.txt", argv[1]);
        out = fopen(path, "wb"); CHECK(out);
        fputs("L:CPU noise\r\n\x01\x02", out);   /* UART noise before the first record */
        fputs(serial, out); fclose(out);
    }
    printf("selftest model: %u records, 0 failures\n", records);
    return 0;
}
