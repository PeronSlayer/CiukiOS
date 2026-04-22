#include "bootinfo.h"
#include "handoff.h"
#include "services.h"

#define EDIT_MAX_LINES 200U
#define EDIT_MAX_COLS 128U
#define EDIT_INPUT_MAX 128U
#define EDIT_RW_CHUNK 256U
#define EDIT_PATH_MAX 96U
/*
 * Visible viewport height (number of buffer lines drawn per redraw).
 * Chosen so the status line + separators + viewport + prompt hint fit
 * comfortably inside the default 80x25 text surface minus the top bar.
 */
#define EDIT_VIEWPORT_ROWS 18U

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static char g_filename[EDIT_PATH_MAX];
static char g_lines[EDIT_MAX_LINES][EDIT_MAX_COLS + 1U];
static u16 g_line_len[EDIT_MAX_LINES];
static u16 g_line_count;
static u8 g_dirty;
/*
 * Viewport + cursor state. `g_cursor_line` is a 0-based index into
 * g_lines (clamped to [0, g_line_count-1] or 0 when the buffer is
 * empty). `g_viewport_top` is the 0-based index of the first line
 * currently drawn on screen; the redraw keeps the cursor inside the
 * viewport by shifting this value when needed.
 *
 * `g_view_active` is non-zero only while the interactive `:v` scroll
 * mode is running. Outside of view mode the in-buffer `>` cursor
 * indicator is suppressed and the writing/editing cursor is rendered
 * on a synthetic "insertion line" below the buffer — this matches
 * the user's mental model: the cursor lives where new text will land,
 * not on an arbitrary navigation slot.
 */
static u16 g_cursor_line;
static u16 g_viewport_top;
static u8 g_view_active;
static u8 g_input_buf[2U + EDIT_INPUT_MAX + 2U];
static u8 g_rw_buf[EDIT_RW_CHUNK];
static char g_work_buf[320];

static const char k_default_name[] = "UNTITLED.TXT";
static const char k_prompt[] = "edit> ";
/*
 * Top header bar content. Kept short and command-oriented so it stays
 * readable on the default text-mode width. The final '$' of the banner
 * strings is required by AH=09h and not suitable here — this string is
 * rendered directly via ui_top_bar.
 */
static const char k_header_bar[] =
    "CiukiOS EDIT  :w save  :q quit  :wq save+quit  :i ins  :s sub  :d del  :c clr  :r reload  :h help";

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

/*
 * Prepare the editor display surface:
 *   1. clear the screen so no shell clutter remains;
 *   2. render a white top bar with the core editor commands in black;
 *   3. reserve the first text row so user input/output lands below the bar.
 * The function is null-safe on every optional ABI pointer so older stage2
 * builds without ui_top_bar/ui_reserve_top_row still run CIUKEDIT (just
 * without the decorated header).
 */
static void editor_setup_surface(ciuki_services_t *svc) {
    if (!svc) {
        return;
    }
    if (svc->cls) {
        svc->cls();
    }
    if (svc->ui_top_bar) {
        svc->ui_top_bar(k_header_bar,
                        0x00FFFFFFU /* white bar */,
                        0x00000000U /* black text */);
    }
    if (svc->ui_reserve_top_row) {
        svc->ui_reserve_top_row(1U);
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
    (void)ctx;
    mem_zero(g_work_buf, (u32)sizeof(g_work_buf));
    mem_copy(g_work_buf + n, "[edit] open path=", 17U);
    n += 17U;
    mem_copy(g_work_buf + n, g_filename, str_len(g_filename));
    n += str_len(g_filename);
    mem_copy(g_work_buf + n, " new=1\n", 7U);
    emit_marker(svc, g_work_buf);
    /*
     * No visible print here: the editor redraw renders a status line
     * after the load phase. Keeping this serial-only avoids polluting
     * the decorated surface that redraw builds immediately after.
     */
}

static void emit_open_stats(ciuki_dos_context_t *ctx, ciuki_services_t *svc, u32 lines, u32 bytes) {
    char a[12];
    char b[12];
    u32 n = 0U;
    (void)ctx;
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
    /* Visible "Opened … (N lines, M bytes)" is produced by the redraw. */
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

/* Insert `text` as a brand-new line before `idx` (0-based). Shifts the
 * existing tail down by one slot. No-op with return 0 if the buffer is
 * already full or idx is past end+1. */
static int insert_line_at(u16 idx, const char *text, u16 len) {
    u16 i;
    u16 n = len;

    if (g_line_count >= EDIT_MAX_LINES) {
        return 0;
    }
    if (idx > g_line_count) {
        return 0;
    }
    if (n > EDIT_MAX_COLS) {
        n = EDIT_MAX_COLS;
    }

    for (i = g_line_count; i > idx; i--) {
        mem_copy(g_lines[i], g_lines[i - 1U], EDIT_MAX_COLS + 1U);
        g_line_len[i] = g_line_len[i - 1U];
    }

    if (n > 0U) {
        mem_copy(g_lines[idx], text, n);
    }
    g_lines[idx][n] = '\0';
    g_line_len[idx] = n;
    g_line_count++;
    return 1;
}

/* Replace the content of the existing line `idx` with `text`. Returns 0
 * if idx is out of range. */
static int replace_line_at(u16 idx, const char *text, u16 len) {
    u16 n = len;
    if (idx >= g_line_count) {
        return 0;
    }
    if (n > EDIT_MAX_COLS) {
        n = EDIT_MAX_COLS;
    }
    if (n > 0U) {
        mem_copy(g_lines[idx], text, n);
    }
    g_lines[idx][n] = '\0';
    g_line_len[idx] = n;
    return 1;
}

static void clear_buffer(void) {
    g_line_count = 0U;
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

static void print_help(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    emit_simple(ctx, svc, "Available commands:\n");
    emit_simple(ctx, svc, "  :w        save current file\n");
    emit_simple(ctx, svc, "  :q        quit (discards unsaved changes)\n");
    emit_simple(ctx, svc, "  :wq       save and quit\n");
    emit_simple(ctx, svc, "  :l        redraw buffer\n");
    emit_simple(ctx, svc, "  :v        scroll/view mode (arrows=move, PgUp/PgDn, Home/End, Esc/q=exit)\n");
    emit_simple(ctx, svc, "  :d N      delete line N (1-based)\n");
    emit_simple(ctx, svc, "  :i N TEXT insert TEXT as new line before N\n");
    emit_simple(ctx, svc, "  :s N TEXT replace line N with TEXT\n");
    emit_simple(ctx, svc, "  :g N      go to line N (move cursor)\n");
    emit_simple(ctx, svc, "  :c        clear buffer\n");
    emit_simple(ctx, svc, "  :r        reload file from disk\n");
    emit_simple(ctx, svc, "  :h        show this help\n");
    emit_simple(ctx, svc, "Any plain input is appended as a new line.\n");
}

/*
 * Render the full editor surface (cls + top bar + status line + visible
 * buffer). Called after every operation that changes what the user
 * needs to see, so the on-screen view is always consistent with the
 * internal buffer. Also emits the `[edit] render lines=N` serial marker
 * so external validation can confirm post-load visibility.
 */
/*
 * Keep the cursor inside [viewport_top, viewport_top + ROWS). Also
 * clamp values against the current buffer size so deletions or
 * reloads can never leave the state pointing past the end.
 */
static void clamp_viewport(void) {
    if (g_line_count == 0U) {
        g_cursor_line = 0U;
        g_viewport_top = 0U;
        return;
    }
    if (g_cursor_line >= g_line_count) {
        g_cursor_line = (u16)(g_line_count - 1U);
    }
    if (g_cursor_line < g_viewport_top) {
        g_viewport_top = g_cursor_line;
    }
    if (g_cursor_line >= (u16)(g_viewport_top + EDIT_VIEWPORT_ROWS)) {
        g_viewport_top = (u16)(g_cursor_line - EDIT_VIEWPORT_ROWS + 1U);
    }
    if (g_viewport_top >= g_line_count) {
        g_viewport_top = (u16)(g_line_count - 1U);
    }
}

static void print_buffer_lines(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    u16 i;
    u16 end;

    if (g_line_count == 0U) {
        emit_simple(ctx, svc, "(empty buffer - type text to add lines)\n");
        return;
    }

    end = (u16)(g_viewport_top + EDIT_VIEWPORT_ROWS);
    if (end > g_line_count) {
        end = g_line_count;
    }

    for (i = g_viewport_top; i < end; i++) {
        char nbuf[12];
        /*
         * Cursor indicator in the gutter: `>` only while interactive
         * view mode is active. In normal edit mode the cursor lives
         * on the synthetic insertion line drawn below the buffer.
         */
        emit_simple(ctx, svc,
                    (g_view_active && i == g_cursor_line) ? ">" : " ");

        to_dec(nbuf, (u32)sizeof(nbuf), (u32)(i + 1U));
        /* Right-pad the number column to 3 for a consistent gutter. */
        if (str_len(nbuf) < 3U) {
            u32 pad = 3U - str_len(nbuf);
            u32 k;
            for (k = 0U; k < pad; k++) {
                emit_simple(ctx, svc, " ");
            }
        }
        emit_simple(ctx, svc, nbuf);
        emit_simple(ctx, svc, " | ");
        if (g_line_len[i] > 0U) {
            (void)write_buf(ctx, svc, g_lines[i], g_line_len[i]);
        }
        emit_simple(ctx, svc, "\n");
    }
}

static void emit_render_marker(ciuki_services_t *svc, u32 lines) {
    char nbuf[12];
    u32 n;

    to_dec(nbuf, (u32)sizeof(nbuf), lines);

    mem_zero(g_work_buf, (u32)sizeof(g_work_buf));
    n = 0U;
    mem_copy(g_work_buf + n, "[edit] render lines=", 20U);
    n += 20U;
    mem_copy(g_work_buf + n, nbuf, str_len(nbuf));
    n += str_len(nbuf);
    g_work_buf[n++] = '\n';
    g_work_buf[n] = '\0';
    emit_marker(svc, g_work_buf);
}

static void editor_redraw(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    char nbuf[12];

    clamp_viewport();

    /* Full surface reset so nothing from previous output leaks in. */
    editor_setup_surface(svc);

    /* Status line — filename, total lines, cursor (in view mode), dirty. */
    emit_simple(ctx, svc, "File: ");
    emit_simple(ctx, svc, g_filename);
    emit_simple(ctx, svc, "   Lines: ");
    to_dec(nbuf, (u32)sizeof(nbuf), (u32)g_line_count);
    emit_simple(ctx, svc, nbuf);
    if (g_view_active && g_line_count > 0U) {
        emit_simple(ctx, svc, "   Cur: ");
        to_dec(nbuf, (u32)sizeof(nbuf), (u32)(g_cursor_line + 1U));
        emit_simple(ctx, svc, nbuf);
    }
    emit_simple(ctx, svc, g_dirty ? "   [modified]\n" : "   [clean]\n");

    if (g_view_active) {
        emit_simple(ctx, svc,
                    "[VIEW MODE] arrows/PgUp/PgDn/Home/End to scroll, Esc/q/Enter to exit\n");
    }

    /* Visual separator. */
    emit_simple(ctx, svc,
                "-------------------------------------------------\n");

    /* Visible buffer (viewport slice, cursor-marked). */
    print_buffer_lines(ctx, svc);

    /*
     * Synthetic insertion line (only outside view mode). Marks where
     * the next plain-input append will land — this is the "writing
     * cursor" the user expects to see in edit mode.
     */
    if (!g_view_active) {
        char nbuf[12];
        u32 next = (u32)g_line_count + 1U;
        to_dec(nbuf, (u32)sizeof(nbuf), next);
        emit_simple(ctx, svc, ">");
        if (str_len(nbuf) < 3U) {
            u32 pad = 3U - str_len(nbuf);
            u32 k;
            for (k = 0U; k < pad; k++) {
                emit_simple(ctx, svc, " ");
            }
        }
        emit_simple(ctx, svc, nbuf);
        emit_simple(ctx, svc, " | _\n");
    }

    /* Bottom separator + compact action hint. */
    emit_simple(ctx, svc,
                "-------------------------------------------------\n");
    emit_simple(ctx, svc,
                ":w save  :q quit  :v scroll (up/dn)  :i/:s/:d edit  :h help\n\n");

    /* Telemetry: lets validation verify that a loaded file was rendered. */
    emit_render_marker(svc, (u32)g_line_count);
}

/*
 * Interactive view/scroll mode.
 * Uses svc->int16 (AH=00h — blocking read, returns scan in AH / ASCII
 * in AL) to drive arrow-based navigation. The viewport and cursor
 * state are global so when the mode exits the next editor_redraw from
 * prompt-mode stays consistent with where the user left off.
 *
 * Stage2's keyboard layer encodes extended keys as cooked ASCII bytes
 * (STAGE2_KEY_UP=0x80, STAGE2_KEY_DOWN=0x81, LEFT=0x82, RIGHT=0x83,
 * HOME=0x84, END=0x85, DEL=0x86). The legacy BIOS scancode (0x48 etc.)
 * is also exposed in AH for compatibility, so we accept either.
 *
 * Keys:
 *   Up   / 'k' / 'w'  — cursor -1 line
 *   Down / 'j' / 's'  — cursor +1 line
 *   PgUp              — cursor - viewport rows
 *   PgDn              — cursor + viewport rows
 *   Home              — cursor to first line
 *   End               — cursor to last line
 *   Esc / 'q' / Enter — leave view mode
 *
 * No-op (and safely returns) when svc->int16 is NULL.
 */
static void view_mode(ciuki_dos_context_t *ctx, ciuki_services_t *svc) {
    ciuki_int21_regs_t regs;
    u8 scan;
    u8 ascii;
    int moved;

    if (!svc || !svc->int16) {
        emit_simple(ctx, svc, "View mode requires int16 keyboard ABI.\n");
        return;
    }

    if (g_line_count == 0U) {
        emit_simple(ctx, svc, "(empty buffer - nothing to scroll)\n");
        return;
    }

    g_view_active = 1U;
    emit_marker(svc, "[edit] view enter\n");
    editor_redraw(ctx, svc);

    for (;;) {
        regs_zero(&regs);
        regs.ax = 0x0000U;  /* AH=00h blocking read */
        svc->int16(ctx, &regs);
        scan = (u8)((regs.ax >> 8) & 0xFFU);
        ascii = (u8)(regs.ax & 0xFFU);
        moved = 0;

        /* Exit keys. */
        if (ascii == 0x1BU /* Esc */
            || ascii == 0x0DU /* Enter / CR */
            || ascii == 0x0AU /* LF */
            || ascii == 'q' || ascii == 'Q') {
            break;
        }

        /*
         * Recognize navigation by both encodings: the cooked ASCII
         * byte produced by stage2 (0x80..0x86) AND the legacy BIOS
         * scancode (0x48..0x51), so the editor keeps working if the
         * shell ever switches to a closer-to-BIOS encoding.
         */
        if (ascii == 0x80U /* STAGE2_KEY_UP */ || scan == 0x48U
            || ascii == 'k' || ascii == 'w') {
            if (g_cursor_line > 0U) {
                g_cursor_line--;
                moved = 1;
            }
        } else if (ascii == 0x81U /* STAGE2_KEY_DOWN */ || scan == 0x50U
                   || ascii == 'j' || ascii == 's') {
            if ((u16)(g_cursor_line + 1U) < g_line_count) {
                g_cursor_line++;
                moved = 1;
            }
        } else if (scan == 0x49U /* PgUp */) {
            if (g_cursor_line > EDIT_VIEWPORT_ROWS) {
                g_cursor_line = (u16)(g_cursor_line - EDIT_VIEWPORT_ROWS);
            } else {
                g_cursor_line = 0U;
            }
            moved = 1;
        } else if (scan == 0x51U /* PgDn */) {
            u32 next = (u32)g_cursor_line + EDIT_VIEWPORT_ROWS;
            if (next >= (u32)g_line_count) {
                next = (u32)(g_line_count - 1U);
            }
            g_cursor_line = (u16)next;
            moved = 1;
        } else if (ascii == 0x84U /* STAGE2_KEY_HOME */ || scan == 0x47U) {
            g_cursor_line = 0U;
            moved = 1;
        } else if (ascii == 0x85U /* STAGE2_KEY_END */ || scan == 0x4FU) {
            g_cursor_line = (u16)(g_line_count - 1U);
            moved = 1;
        } else {
            /* Unknown key — ignore silently. */
            continue;
        }

        if (moved) {
            editor_redraw(ctx, svc);
        }
    }

    g_view_active = 0U;
    emit_marker(svc, "[edit] view exit\n");
    editor_redraw(ctx, svc);
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
        editor_redraw(ctx, svc);
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
        editor_redraw(ctx, svc);
        return;
    }

    if (str_eq(cmd, ":v")) {
        view_mode(ctx, svc);
        return;
    }

    if (str_eq(cmd, ":h")) {
        print_help(ctx, svc);
        return;
    }

    if (str_eq(cmd, ":c")) {
        if (g_line_count > 0U) {
            g_dirty = 1U;
        }
        clear_buffer();
        emit_marker(svc, "[edit] clear\n");
        editor_redraw(ctx, svc);
        return;
    }

    if (str_eq(cmd, ":r")) {
        clear_buffer();
        g_dirty = 0U;
        emit_marker(svc, "[edit] reload\n");
        if (!load_file(ctx, svc)) {
            terminate(ctx, svc, 0x01U);
            return;
        }
        editor_redraw(ctx, svc);
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
        editor_redraw(ctx, svc);
        return;
    }

    if (str_starts_with(cmd, ":g")) {
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

        g_cursor_line = (u16)(idx - 1U);
        editor_redraw(ctx, svc);
        return;
    }

    if (str_starts_with(cmd, ":i") && (cmd[2] == ' ' || cmd[2] == '\t')) {
        char arg[16];
        u32 i = 2U;
        u32 j = 0U;
        u16 idx = 0U;
        const char *text;
        u16 text_len;

        while (cmd[i] == ' ' || cmd[i] == '\t') {
            i++;
        }
        while (cmd[i] != '\0' && cmd[i] != ' ' && cmd[i] != '\t'
               && j + 1U < (u32)sizeof(arg)) {
            arg[j++] = cmd[i++];
        }
        arg[j] = '\0';

        if (!parse_u16(arg, &idx) || idx == 0U
            || idx > (u16)(g_line_count + 1U)) {
            emit_marker(svc, "[edit] error class=bad_index\n");
            emit_simple(ctx, svc, "Error: invalid line number.\n");
            return;
        }

        while (cmd[i] == ' ' || cmd[i] == '\t') {
            i++;
        }
        text = cmd + i;
        text_len = (u16)str_len(text);

        if (!insert_line_at((u16)(idx - 1U), text, text_len)) {
            emit_marker(svc, "[edit] error class=buffer_full\n");
            emit_simple(ctx, svc, "Buffer full (max 200 lines).\n");
            return;
        }
        g_dirty = 1U;
        editor_redraw(ctx, svc);
        return;
    }

    if (str_starts_with(cmd, ":s") && (cmd[2] == ' ' || cmd[2] == '\t')) {
        char arg[16];
        u32 i = 2U;
        u32 j = 0U;
        u16 idx = 0U;
        const char *text;
        u16 text_len;

        while (cmd[i] == ' ' || cmd[i] == '\t') {
            i++;
        }
        while (cmd[i] != '\0' && cmd[i] != ' ' && cmd[i] != '\t'
               && j + 1U < (u32)sizeof(arg)) {
            arg[j++] = cmd[i++];
        }
        arg[j] = '\0';

        if (!parse_u16(arg, &idx) || idx == 0U || idx > g_line_count) {
            emit_marker(svc, "[edit] error class=bad_index\n");
            emit_simple(ctx, svc, "Error: invalid line number.\n");
            return;
        }

        while (cmd[i] == ' ' || cmd[i] == '\t') {
            i++;
        }
        text = cmd + i;
        text_len = (u16)str_len(text);

        if (!replace_line_at((u16)(idx - 1U), text, text_len)) {
            emit_marker(svc, "[edit] error class=bad_index\n");
            emit_simple(ctx, svc, "Error: invalid line number.\n");
            return;
        }
        g_dirty = 1U;
        editor_redraw(ctx, svc);
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
    g_cursor_line = 0U;
    g_viewport_top = 0U;
    g_view_active = 0U;
    mem_zero(g_filename, (u32)sizeof(g_filename));

    /*
     * Set up the clean editor surface first so any warning from
     * parse_filename lands on the decorated layout rather than on
     * leftover shell output.
     */
    editor_setup_surface(svc);

    if (!parse_filename(ctx, svc)) {
        terminate(ctx, svc, 0x02U);
        return;
    }

    if (!load_file(ctx, svc)) {
        terminate(ctx, svc, 0x01U);
        return;
    }

    /*
     * Root-cause fix for the "reopened file looks empty" bug:
     * load_file() only populates g_lines[] — previously the user
     * saw nothing on screen until `:l` was typed. Render the full
     * decorated editor surface (status + buffer) immediately so the
     * loaded content is actually visible after open.
     */
    editor_redraw(ctx, svc);

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
        /* Keep the cursor glued to the most recently appended line so
         * the viewport auto-scrolls and the user sees what they typed. */
        if (g_line_count > 0U) {
            g_cursor_line = (u16)(g_line_count - 1U);
        }
        /*
         * Full redraw after every appended line: this refreshes the
         * status line (Lines:/[modified]) and the synthetic insertion
         * line so the writing cursor stays right under the new text.
         */
        editor_redraw(ctx, svc);
    }
}
