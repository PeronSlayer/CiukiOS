#include "bootinfo.h"
#include "handoff.h"
#include "services.h"

#define EDIT_MAX_LINES 200U
#define EDIT_MAX_COLS 128U
#define EDIT_INPUT_MAX 128U
#define EDIT_RW_CHUNK 256U
#define EDIT_PATH_MAX 96U

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static char g_filename[EDIT_PATH_MAX];
static char g_lines[EDIT_MAX_LINES][EDIT_MAX_COLS + 1U];
static u16 g_line_len[EDIT_MAX_LINES];
static u16 g_line_count;
static u8 g_dirty;
static u8 g_input_buf[2U + EDIT_INPUT_MAX + 2U];
static u8 g_rw_buf[EDIT_RW_CHUNK];
static char g_work_buf[320];

static const char k_default_name[] = "UNTITLED.TXT";
static const char k_banner_0[] = "CiukiOS EDIT  -  line editor$";
static const char k_prompt[] = "edit> ";
static const char k_help_hint[] = "Type :h for help, :l to list, :q to quit.";

static void mem_copy(void *dst, const void *src, u32 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n-- > 0U) {
        *d++ = *s++;
    }
}

static void mem_zero(void *dst, u32 n) {
    u8 *d = (u8 *)dst;
    while (n-- > 0U) {
        *d++ = 0U;
    }
}

static u32 str_len(const char *s) {
    u32 n = 0U;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int str_eq(const char *a, const char *b) {
    u32 i = 0U;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int str_starts_with(const char *s, const char *prefix) {
    u32 i = 0U;
    while (prefix[i] != '\0') {
        if (s[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int to_dec(char *out, u32 out_cap, u32 v) {
    char rev[12];
    u32 n = 0U;
    u32 i;

    if (!out || out_cap == 0U) {
        return 0;
    }

    if (v == 0U) {
        if (out_cap < 2U) {
            return 0;
        }
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }

    while (v > 0U && n < sizeof(rev)) {
        rev[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }

    if (n + 1U > out_cap) {
        return 0;
    }

    for (i = 0U; i < n; i++) {
        out[i] = rev[n - 1U - i];
    }
    out[n] = '\0';
    return 1;
}

static int to_hex2(char *out, u32 out_cap, u8 v) {
    static const char hex[] = "0123456789ABCDEF";
    if (!out || out_cap < 3U) {
        return 0;
    }
    out[0] = hex[(v >> 4) & 0x0FU];
    out[1] = hex[v & 0x0FU];
    out[2] = '\0';
    return 1;
}

static int ptr_off(ciuki_dos_context_t *ctx, const void *ptr, u16 *off_out) {
    u64 base;
    u64 p;
    u64 off;

    if (!ctx || !ptr || !off_out) {
        return 0;
    }

    base = ctx->image_linear;
    p = (u64)(const void *)ptr;
    if (p < base) {
        return 0;
    }

    off = p - base;
    if (off > 0xFFFFULL) {
        return 0;
    }

    *off_out = (u16)off;
    return 1;
}

static void regs_zero(ciuki_int21_regs_t *regs) {
    regs->ax = 0U;
    regs->bx = 0U;
    regs->cx = 0U;
    regs->dx = 0U;
    regs->si = 0U;
    regs->di = 0U;
    regs->ds = 0U;
    regs->es = 0U;
    regs->carry = 0U;
    regs->reserved[0] = 0U;
    regs->reserved[1] = 0U;
    regs->reserved[2] = 0U;
}

static int int21(ciuki_dos_context_t *ctx, ciuki_services_t *svc, ciuki_int21_regs_t *regs) {
    if (!ctx || !svc || !regs || !svc->int21) {
        return 0;
    }
    svc->int21(ctx, regs);
    return 1;
}

static int write_buf(ciuki_dos_context_t *ctx, ciuki_services_t *svc, const char *buf, u16 len) {
    ciuki_int21_regs_t regs;
    u16 off;

    if (!buf || len == 0U) {
        return 1;
    }

    if (!ptr_off(ctx, buf, &off)) {
        return 0;
    }

    regs_zero(&regs);
    regs.ax = 0x4000U;
    regs.bx = 0x0001U;
    regs.cx = len;
    regs.dx = off;

    if (!int21(ctx, svc, &regs)) {
        return 0;
    }

    return (regs.carry == 0U && regs.ax == len) ? 1 : 0;
}

static int write_cstr(ciuki_dos_context_t *ctx, ciuki_services_t *svc, const char *s) {
    return write_buf(ctx, svc, s, (u16)str_len(s));
}

static int write_ah09(ciuki_dos_context_t *ctx, ciuki_services_t *svc, const char *s) {
    ciuki_int21_regs_t regs;
    u16 off;

    if (!ptr_off(ctx, s, &off)) {
        return 0;
    }

    regs_zero(&regs);
    regs.ax = 0x0900U;
    regs.dx = off;
    if (!int21(ctx, svc, &regs)) {
        return 0;
    }
    return regs.carry == 0U ? 1 : 0;
}

static void terminate(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u8 code) {
    if (svc && svc->terminate) {
        svc->terminate(ctx, code);
    }
}

static void emit_simple(ciuki_dos_context_t *ctx, ciuki_services_t *svc, const char *s) {
    (void)write_cstr(ctx, svc, s);
}

/*
 * Serial-only telemetry marker. Used for deterministic `[edit] ...` evidence
 * that the smoke harness greps without cluttering the user-facing editor
 * surface on the framebuffer. Falls back to a silent no-op on legacy stage2
 * builds that do not provide serial_print.
 */
static void emit_marker(ciuki_services_t *svc, const char *s) {
    if (svc && svc->serial_print && s) {
        svc->serial_print(s);
    }
}

static void emit_error_rc(ciuki_dos_context_t *ctx, ciuki_services_t *svc, const char *class_name, u8 rc) {
    char hx[3];
    mem_zero(g_work_buf, (u32)sizeof(g_work_buf));
    to_hex2(hx, (u32)sizeof(hx), rc);

    /* Serial-only telemetry marker. */
    mem_copy(g_work_buf, "[edit] error class=", 19U);
    mem_copy(g_work_buf + 19U, class_name, str_len(class_name));
    {
        u32 n = 19U + str_len(class_name);
        mem_copy(g_work_buf + n, " rc=0x", 6U);
        n += 6U;
        g_work_buf[n++] = hx[0];
        g_work_buf[n++] = hx[1];
        g_work_buf[n++] = '\n';
        g_work_buf[n] = '\0';
    }
    emit_marker(svc, g_work_buf);

    /* User-visible friendly message. */
    emit_simple(ctx, svc, "Error: ");
    emit_simple(ctx, svc, class_name);
    emit_simple(ctx, svc, " failed (rc=0x");
    {
        char line[4];
        line[0] = hx[0];
        line[1] = hx[1];
        line[2] = ')';
        line[3] = '\0';
        emit_simple(ctx, svc, line);
    }
    emit_simple(ctx, svc, "\n");
}

static void emit_open_new(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    u32 n = 0U;
    mem_zero(g_work_buf, (u32)sizeof(g_work_buf));
    mem_copy(g_work_buf + n, "[edit] open path=", 17U);
    n += 17U;
    mem_copy(g_work_buf + n, g_filename, str_len(g_filename));
    n += str_len(g_filename);
    mem_copy(g_work_buf + n, " new=1\n", 7U);
    emit_marker(svc, g_work_buf);

    emit_simple(ctx, svc, "New file: ");
    emit_simple(ctx, svc, g_filename);
    emit_simple(ctx, svc, "\n");
}

static void emit_open_stats(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u32 lines, u32 bytes) {
    char a[12];
    char b[12];
    u32 n = 0U;
    to_dec(a, (u32)sizeof(a), lines);
    to_dec(b, (u32)sizeof(b), bytes);

    mem_zero(g_work_buf, (u32)sizeof(g_work_buf));
    mem_copy(g_work_buf + n, "[edit] open path=", 17U);
    n += 17U;
    mem_copy(g_work_buf + n, g_filename, str_len(g_filename));
    n += str_len(g_filename);
    mem_copy(g_work_buf + n, " lines=", 7U);
    n += 7U;
    mem_copy(g_work_buf + n, a, str_len(a));
    n += str_len(a);
    mem_copy(g_work_buf + n, " bytes=", 7U);
    n += 7U;
    mem_copy(g_work_buf + n, b, str_len(b));
    n += str_len(b);
    g_work_buf[n++] = '\n';
    g_work_buf[n] = '\0';
    emit_marker(svc, g_work_buf);

    emit_simple(ctx, svc, "Opened ");
    emit_simple(ctx, svc, g_filename);
    emit_simple(ctx, svc, " (");
    emit_simple(ctx, svc, a);
    emit_simple(ctx, svc, lines == 1U ? " line, " : " lines, ");
    emit_simple(ctx, svc, b);
    emit_simple(ctx, svc, " bytes)\n");
}

static void emit_save_stats(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u32 lines, u32 bytes) {
    char a[12];
    char b[12];
    u32 n = 0U;
    to_dec(a, (u32)sizeof(a), lines);
    to_dec(b, (u32)sizeof(b), bytes);

    mem_zero(g_work_buf, (u32)sizeof(g_work_buf));
    mem_copy(g_work_buf + n, "[edit] save path=", 17U);
    n += 17U;
    mem_copy(g_work_buf + n, g_filename, str_len(g_filename));
    n += str_len(g_filename);
    mem_copy(g_work_buf + n, " lines=", 7U);
    n += 7U;
    mem_copy(g_work_buf + n, a, str_len(a));
    n += str_len(a);
    mem_copy(g_work_buf + n, " bytes=", 7U);
    n += 7U;
    mem_copy(g_work_buf + n, b, str_len(b));
    n += str_len(b);
    g_work_buf[n++] = '\n';
    g_work_buf[n] = '\0';
    emit_marker(svc, g_work_buf);

    emit_simple(ctx, svc, "Saved ");
    emit_simple(ctx, svc, g_filename);
    emit_simple(ctx, svc, " (");
    emit_simple(ctx, svc, a);
    emit_simple(ctx, svc, lines == 1U ? " line, " : " lines, ");
    emit_simple(ctx, svc, b);
    emit_simple(ctx, svc, " bytes)\n");
}

static void emit_quit(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u8 dirty) {
    mem_zero(g_work_buf, (u32)sizeof(g_work_buf));
    mem_copy(g_work_buf, "[edit] quit dirty=", 17U);
    g_work_buf[17] = dirty ? '1' : '0';
    g_work_buf[18] = '\n';
    g_work_buf[19] = '\0';
    emit_marker(svc, g_work_buf);

    emit_simple(ctx, svc, dirty ? "Exiting (unsaved changes discarded).\n"
                                : "Exiting.\n");
}

static int parse_filename(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    u32 i = 0U;
    u32 out = 0U;

    if (!ctx) {
        return 0;
    }

    while (i < (u32)ctx->command_tail_len && (ctx->command_tail[i] == ' ' || ctx->command_tail[i] == '\t')) {
        i++;
    }

    if (i >= (u32)ctx->command_tail_len) {
        mem_copy(g_filename, k_default_name, (u32)sizeof(k_default_name));
        emit_marker(svc, "[edit] warn class=no_filename default=UNTITLED.TXT\n");
        emit_simple(ctx, svc, "No file name given; using UNTITLED.TXT.\n");
        return 1;
    }

    while (i < (u32)ctx->command_tail_len) {
        char ch = ctx->command_tail[i];
        if (ch == ' ' || ch == '\t') {
            break;
        }
        if (out + 1U >= (u32)sizeof(g_filename)) {
            emit_marker(svc, "[edit] error class=parse\n");
            emit_simple(ctx, svc, "Error: file name too long.\n");
            return 0;
        }
        g_filename[out++] = ch;
        i++;
    }
    g_filename[out] = '\0';

    if (g_filename[0] == '\0') {
        mem_copy(g_filename, k_default_name, (u32)sizeof(k_default_name));
        emit_marker(svc, "[edit] warn class=no_filename default=UNTITLED.TXT\n");
        emit_simple(ctx, svc, "No file name given; using UNTITLED.TXT.\n");
    }

    return 1;
}

static int read_line_input(ciuki_dos_context_t *ctx, ciuki_services_t *svc, char *out, u32 out_cap, u16 *len_out) {
    ciuki_int21_regs_t regs;
    u16 off;
    u16 i;
    u16 n;

    if (!out || out_cap == 0U || !len_out) {
        return 0;
    }

    g_input_buf[0] = EDIT_INPUT_MAX;
    g_input_buf[1] = 0U;

    if (!ptr_off(ctx, g_input_buf, &off)) {
        return 0;
    }

    regs_zero(&regs);
    regs.ax = 0x0A00U;
    regs.dx = off;
    if (!int21(ctx, svc, &regs) || regs.carry != 0U) {
        return 0;
    }

    n = g_input_buf[1];
    if ((u32)n + 1U > out_cap) {
        n = (u16)(out_cap - 1U);
    }

    for (i = 0U; i < n; i++) {
        out[i] = (char)g_input_buf[2U + i];
    }
    out[n] = '\0';
    *len_out = n;
    return 1;
}

static int append_line_raw(const char *line, u16 len) {
    u16 n = len;
    if (g_line_count >= EDIT_MAX_LINES) {
        return 0;
    }
    if (n > EDIT_MAX_COLS) {
        n = EDIT_MAX_COLS;
    }
    if (n > 0U) {
        mem_copy(g_lines[g_line_count], line, n);
    }
    g_lines[g_line_count][n] = '\0';
    g_line_len[g_line_count] = n;
    g_line_count++;
    return 1;
}

static void delete_line_at(u16 idx) {
    u16 i;
    if (idx >= g_line_count) {
        return;
    }
    for (i = idx; (u16)(i + 1U) < g_line_count; i++) {
        mem_copy(g_lines[i], g_lines[i + 1U], EDIT_MAX_COLS + 1U);
        g_line_len[i] = g_line_len[i + 1U];
    }
    g_line_count--;
}

static int close_handle(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u16 h) {
    ciuki_int21_regs_t regs;
    regs_zero(&regs);
    regs.ax = 0x3E00U;
    regs.bx = h;
    if (!int21(ctx, svc, &regs)) {
        return 0;
    }
    return regs.carry == 0U ? 1 : 0;
}

static int open_existing(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u16 *h_out, u8 *rc_out) {
    ciuki_int21_regs_t regs;
    u16 off;

    if (!ptr_off(ctx, g_filename, &off)) {
        return 0;
    }

    regs_zero(&regs);
    regs.ax = 0x3D00U;
    regs.dx = off;

    if (!int21(ctx, svc, &regs)) {
        return 0;
    }

    if (regs.carry != 0U) {
        if (rc_out) {
            *rc_out = (u8)(regs.ax & 0x00FFU);
        }
        return 0;
    }

    *h_out = regs.ax;
    if (rc_out) {
        *rc_out = 0U;
    }
    return 1;
}

static int create_truncate(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u16 *h_out, u8 *rc_out) {
    ciuki_int21_regs_t regs;
    u16 off;

    if (!ptr_off(ctx, g_filename, &off)) {
        return 0;
    }

    regs_zero(&regs);
    regs.ax = 0x3C00U;
    regs.cx = 0x0000U;
    regs.dx = off;

    if (!int21(ctx, svc, &regs)) {
        return 0;
    }

    if (regs.carry != 0U) {
        if (rc_out) {
            *rc_out = (u8)(regs.ax & 0x00FFU);
        }
        return 0;
    }

    *h_out = regs.ax;
    if (rc_out) {
        *rc_out = 0U;
    }
    return 1;
}

static int read_handle(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u16 h, u8 *buf, u16 cap, u16 *read_out, u8 *rc_out) {
    ciuki_int21_regs_t regs;
    u16 off;

    if (!ptr_off(ctx, buf, &off)) {
        return 0;
    }

    regs_zero(&regs);
    regs.ax = 0x3F00U;
    regs.bx = h;
    regs.cx = cap;
    regs.dx = off;

    if (!int21(ctx, svc, &regs)) {
        return 0;
    }

    if (regs.carry != 0U) {
        if (rc_out) {
            *rc_out = (u8)(regs.ax & 0x00FFU);
        }
        return 0;
    }

    *read_out = regs.ax;
    if (rc_out) {
        *rc_out = 0U;
    }
    return 1;
}

static int write_handle(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u16 h, const u8 *buf, u16 len, u16 *written_out, u8 *rc_out) {
    ciuki_int21_regs_t regs;
    u16 off;

    if (!ptr_off(ctx, buf, &off)) {
        return 0;
    }

    regs_zero(&regs);
    regs.ax = 0x4000U;
    regs.bx = h;
    regs.cx = len;
    regs.dx = off;

    if (!int21(ctx, svc, &regs)) {
        return 0;
    }

    if (regs.carry != 0U) {
        if (rc_out) {
            *rc_out = (u8)(regs.ax & 0x00FFU);
        }
        return 0;
    }

    *written_out = regs.ax;
    if (rc_out) {
        *rc_out = 0U;
    }
    return 1;
}

static void ingest_loaded_byte(char ch, char *line, u16 *line_len) {
    if (ch == '\r') {
        return;
    }
    if (ch == '\n') {
        (void)append_line_raw(line, *line_len);
        *line_len = 0U;
        line[0] = '\0';
        return;
    }

    if (*line_len < EDIT_MAX_COLS) {
        line[*line_len] = ch;
        (*line_len)++;
        line[*line_len] = '\0';
    }
}

static int load_file(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    u16 h = 0U;
    u8 rc = 0U;
    u32 total_bytes = 0U;
    char line[EDIT_MAX_COLS + 1U];
    u16 line_len = 0U;

    line[0] = '\0';

    if (!open_existing(ctx, svc, &h, &rc)) {
        if (rc == 0x02U) {
            emit_open_new(ctx, svc);
            return 1;
        }
        emit_error_rc(ctx, svc, "open", rc);
        return 0;
    }

    for (;;) {
        u16 n = 0U;
        u16 i;

        if (!read_handle(ctx, svc, h, g_rw_buf, EDIT_RW_CHUNK, &n, &rc)) {
            (void)close_handle(ctx, svc, h);
            emit_error_rc(ctx, svc, "read", rc);
            return 0;
        }

        if (n == 0U) {
            break;
        }

        total_bytes += n;
        for (i = 0U; i < n; i++) {
            ingest_loaded_byte((char)g_rw_buf[i], line, &line_len);
        }
    }

    if (line_len > 0U) {
        (void)append_line_raw(line, line_len);
    }

    (void)close_handle(ctx, svc, h);
    emit_open_stats(ctx, svc, g_line_count, total_bytes);
    return 1;
}

static int save_file(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    u16 h = 0U;
    u8 rc = 0U;
    u32 total = 0U;
    u16 i;

    if (!create_truncate(ctx, svc, &h, &rc)) {
        emit_error_rc(ctx, svc, "write", rc);
        return 0;
    }

    for (i = 0U; i < g_line_count; i++) {
        u16 want = g_line_len[i];
        u16 done = 0U;

        if (want > 0U) {
            mem_copy(g_rw_buf, g_lines[i], want);
        }
        g_rw_buf[want] = '\n';
        want = (u16)(want + 1U);

        if (!write_handle(ctx, svc, h, g_rw_buf, want, &done, &rc) || done != want) {
            (void)close_handle(ctx, svc, h);
            emit_error_rc(ctx, svc, "write", rc);
            return 0;
        }
        total += want;
    }

    if (!close_handle(ctx, svc, h)) {
        emit_error_rc(ctx, svc, "write", 0x05U);
        return 0;
    }

    emit_save_stats(ctx, svc, g_line_count, total);
    g_dirty = 0U;
    return 1;
}

static void print_header(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    emit_simple(ctx, svc, "\n");
    (void)write_ah09(ctx, svc, k_banner_0);
    emit_simple(ctx, svc, "\n");
    emit_simple(ctx, svc, "File: ");
    emit_simple(ctx, svc, g_filename);
    emit_simple(ctx, svc, "\n\n");
    emit_simple(ctx, svc, k_help_hint);
    emit_simple(ctx, svc, "\n\n");
}

static void print_help(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    emit_simple(ctx, svc, "Available commands:\n");
    emit_simple(ctx, svc, "  :w        save current file\n");
    emit_simple(ctx, svc, "  :q        quit (discards unsaved changes)\n");
    emit_simple(ctx, svc, "  :wq       save and quit\n");
    emit_simple(ctx, svc, "  :l        list all lines\n");
    emit_simple(ctx, svc, "  :d N      delete line N (1-based)\n");
    emit_simple(ctx, svc, "  :h        show this help\n");
    emit_simple(ctx, svc, "Any other input is appended as a new line.\n");
}

static void print_lines(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    u16 i;

    if (g_line_count == 0U) {
        emit_simple(ctx, svc, "(empty)\n");
        return;
    }

    for (i = 0U; i < g_line_count; i++) {
        char nbuf[12];
        to_dec(nbuf, (u32)sizeof(nbuf), (u32)(i + 1U));
        emit_simple(ctx, svc, nbuf);
        emit_simple(ctx, svc, ": ");
        if (g_line_len[i] > 0U) {
            write_buf(ctx, svc, g_lines[i], g_line_len[i]);
        }
        emit_simple(ctx, svc, "\n");
    }
}

static int parse_u16(const char *s, u16 *out) {
    u32 v = 0U;
    u32 i = 0U;

    if (!s || !out) {
        return 0;
    }

    if (s[0] == '\0') {
        return 0;
    }

    while (s[i] != '\0') {
        char ch = s[i];
        if (ch < '0' || ch > '9') {
            return 0;
        }
        v = (v * 10U) + (u32)(ch - '0');
        if (v > 65535U) {
            return 0;
        }
        i++;
    }

    *out = (u16)v;
    return 1;
}

static void ltrim(char *s) {
    u32 i = 0U;
    u32 j = 0U;
    while (s[i] == ' ' || s[i] == '\t') {
        i++;
    }
    if (i == 0U) {
        return;
    }
    while (s[i] != '\0') {
        s[j++] = s[i++];
    }
    s[j] = '\0';
}

static void handle_command(ciuki_dos_context_t *ctx, ciuki_services_t *svc, char *cmd, u16 len) {
    (void)len;

    if (str_eq(cmd, ":w")) {
        if (!save_file(ctx, svc)) {
            terminate(ctx, svc, 0x01U);
            return;
        }
        return;
    }

    if (str_eq(cmd, ":q")) {
        emit_quit(ctx, svc, g_dirty ? 1U : 0U);
        terminate(ctx, svc, 0x00U);
        return;
    }

    if (str_eq(cmd, ":wq")) {
        if (!save_file(ctx, svc)) {
            terminate(ctx, svc, 0x01U);
            return;
        }
        emit_quit(ctx, svc, 0U);
        terminate(ctx, svc, 0x00U);
        return;
    }

    if (str_eq(cmd, ":l")) {
        print_lines(ctx, svc);
        return;
    }

    if (str_eq(cmd, ":h")) {
        print_help(ctx, svc);
        return;
    }

    if (str_starts_with(cmd, ":d")) {
        char arg[16];
        u32 i = 2U;
        u32 j = 0U;
        u16 idx = 0U;

        while (cmd[i] == ' ' || cmd[i] == '\t') {
            i++;
        }
        while (cmd[i] != '\0' && j + 1U < (u32)sizeof(arg)) {
            arg[j++] = cmd[i++];
        }
        arg[j] = '\0';

        if (!parse_u16(arg, &idx) || idx == 0U || idx > g_line_count) {
            emit_marker(svc, "[edit] error class=bad_index\n");
            emit_simple(ctx, svc, "Error: invalid line number.\n");
            return;
        }

        delete_line_at((u16)(idx - 1U));
        g_dirty = 1U;
        return;
    }

    emit_marker(svc, "[edit] error class=bad_command\n");
    emit_simple(ctx, svc, "Unknown command. ");
    print_help(ctx, svc);
}

void com_main(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    char line[EDIT_MAX_COLS + 1U];
    u16 line_len = 0U;

    if (!ctx || !svc) {
        return;
    }

    g_line_count = 0U;
    g_dirty = 0U;
    mem_zero(g_filename, (u32)sizeof(g_filename));

    if (!parse_filename(ctx, svc)) {
        terminate(ctx, svc, 0x02U);
        return;
    }

    print_header(ctx, svc);

    if (!load_file(ctx, svc)) {
        terminate(ctx, svc, 0x01U);
        return;
    }

    for (;;) {
        emit_simple(ctx, svc, k_prompt);
        if (!read_line_input(ctx, svc, line, (u32)sizeof(line), &line_len)) {
            emit_marker(svc, "[edit] error class=parse\n");
            emit_simple(ctx, svc, "Input error.\n");
            terminate(ctx, svc, 0x02U);
            return;
        }

        ltrim(line);
        if (line[0] == '\0') {
            continue;
        }

        if (line[0] == ':') {
            handle_command(ctx, svc, line, line_len);
            /* If the command called svc->terminate, exit_reason is no longer 0. */
            if (ctx->exit_reason != (u8)CIUKI_COM_EXIT_RETURN) {
                return;
            }
            continue;
        }

        if (!append_line_raw(line, (u16)str_len(line))) {
            emit_marker(svc, "[edit] error class=buffer_full\n");
            emit_simple(ctx, svc, "Buffer full (max 200 lines).\n");
            continue;
        }
        g_dirty = 1U;
    }
}
