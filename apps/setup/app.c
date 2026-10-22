/* SPDX-License-Identifier: MIT
 * Setup flow: start-up checks, page sequence, keyboard model, dialogs, the
 * unattended runner and the installation transaction (setup_txn.h).
 *
 * Keyboard (directive f9-03 item 1): Tab/Shift+Tab move the focus in the
 * mockup's reading order, Enter presses the focused button or else the
 * page's default button, Esc goes back (Welcome: exit, Copy: cancel,
 * Rescue/Text: close), F1 shows the page's help, F3 exits Setup after a
 * confirmation, F10 opens the read-only rescue console (the mockup's footer
 * copy; see the report). setup_txn_execute is called only by st_install,
 * which only the "Ready to install" page reaches. */
#include "setup.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const page_names[ST_PAGES] = {
    "welcome", "licence", "hardware", "disk", "components", "user", "network", "summary", "copy", "restart",
    "stop", "failed", "status", "rescue", "text"
};
const char *st_page_name(enum st_page p) { return (unsigned)p < ST_PAGES ? page_names[p] : "unknown"; }

static uint64_t now_ns(struct st_app *a) { return a->h.monotonic ? a->h.monotonic(a->h.ctx) : 0; }

void st_log(struct st_app *a, const char *fmt, ...)
{
    char line[400];
    int n = snprintf(line, sizeof(line), "%" PRIu64 " ", now_ns(a) / 1000000u);
    va_list ap; va_start(ap, fmt);
    if (n > 0 && (size_t)n < sizeof(line)) vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    setup_log_append(&a->log, line);
}

void st_record(struct st_app *a, const char *fmt, ...)
{
    char line[400];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    if (a->h.record) a->h.record(a->h.ctx, line);
    st_log(a, "%s", line);
}

void st_message(struct st_app *a, const char *title, const char *text, int icon)
{
    static const char *const ok[] = {"OK"};
    du_message(a->ui, ST_DLG_MESSAGE, title, text, icon, ok, 1, 0, 0);
}

/* ---- status records and SETUP.STS ---- */
static void status_file(struct st_app *a, const char *line)
{
    const char *src = a->answers.source;
    if (!a->answers.unattended || !src[0] || src[0] == 'D' || src[1] != ':') return;
    for (int i = 0; i < a->volume_count; i++) {
        if (a->volumes[i].letter != src[0] || !a->volumes[i].usb || !a->volumes[i].writable) continue;
        char path[16], text[200];
        snprintf(path, sizeof(path), "%c:\\SETUP.STS", src[0]);
        int n = snprintf(text, sizeof(text), "%s\r\n", line);
        int r = a->h.write_file ? a->h.write_file(a->h.ctx, path, text, (size_t)n) : -ENOSYS;
        st_log(a, "status file %s result=%d", path, r);
    }
}

static void stopped(struct st_app *a, const char *reason, const char *key)
{
    char line[160];
    snprintf(line, sizeof(line), "[setup] status=stopped reason=%s key=%s", reason, key && key[0] ? key : "-");
    snprintf(a->status_line, sizeof(a->status_line), "%s", line);
    st_record(a, "%s", line);
    status_file(a, line + 8);
}

/* ---- start ---- */
static int compare_names(const void *x, const void *y)
{
    const char *a = x, *b = y;
    for (;; a++, b++) {
        int p = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, q = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (p != q || !p) return p - q;
    }
}

/* C:\system\licenses, labelled through D:\LICENSES\INDEX.TXT when it names
 * a file: "<8.3 name> <original name> [description]" (f9-01's mapping). */
int st_licences_load(struct st_app *a)
{
    a->licence_count = 0;
    if (!a->h.list_dir) return -ENOSYS;
    int n = a->h.list_dir(a->h.ctx, "C:\\system\\licenses", a->licence_names, ST_LICENCES_MAX);
    if (n <= 0) return n;
    a->licence_count = (unsigned)n;
    qsort(a->licence_names, a->licence_count, sizeof(a->licence_names[0]), compare_names);
    size_t length = 0;
    int index = a->h.read_file(a->h.ctx, "D:\\LICENSES\\INDEX.TXT", a->file, ST_FILE_BYTES - 1, &length);
    if (index) length = 0;
    a->file[length] = 0;
    for (unsigned i = 0; i < a->licence_count; i++) {
        snprintf(a->licence_labels[i], sizeof(a->licence_labels[i]), "%s", a->licence_names[i]);
        for (char *line = a->file; line && *line;) {
            char *end = strchr(line, '\n');
            char row[200]; size_t k = end ? (size_t)(end - line) : strlen(line);
            if (k >= sizeof(row)) k = sizeof(row) - 1;
            memcpy(row, line, k); row[k] = 0;
            if (k && row[k - 1] == '\r') row[k - 1] = 0;
            char dos[16] = "", original[96] = ""; int used = 0;
            if (sscanf(row, "%15s %95s %n", dos, original, &used) >= 2 && !compare_names(original, a->licence_names[i])) {
                const char *description = row + used;
                if (*description)
                    snprintf(a->licence_labels[i], sizeof(a->licence_labels[i]), "%.40s \xe2\x80\x94 %.50s", a->licence_names[i], description);
                break;
            }
            line = end ? end + 1 : NULL;
        }
    }
    return 0;
}

int st_text_load(struct st_app *a, const char *title, const char *path, const char *text, size_t length)
{
    if (!a->text_ready) return -ENOMEM;
    char *buffer = a->text.text;
    if (path) {
        int r = a->h.read_file(a->h.ctx, path, buffer, a->text.cap, &length);
        if (r == -EFBIG) { length = a->text.cap; r = 0; }
        if (r) return r;
    } else {
        if (length > a->text.cap) length = a->text.cap;
        memmove(buffer, text, length);
    }
    /* The viewer shows Latin-1 text; drop CR and the NULs of binary files. */
    size_t k = 0;
    for (size_t i = 0; i < length; i++) if (buffer[i] != '\r' && buffer[i]) buffer[k++] = buffer[i];
    char *copy = malloc(k + 1);
    if (!copy) return -ENOMEM;
    memcpy(copy, buffer, k); copy[k] = 0;
    int r = du_ta_set_text(&a->text, copy, k);
    free(copy);
    a->text.readonly = 1; du_ta_set_wrap(&a->text, 1);
    snprintf(a->text_title, sizeof(a->text_title), "%s", title);
    return r;
}

static void start_unattended(struct st_app *a);
int st_init(struct st_app *a, struct du_ui *u, const struct st_host *h)
{
    memset(a, 0, sizeof(*a));
    a->ui = u; a->h = *h;
    u->event = NULL; u->user = a;
    a->file = malloc(ST_FILE_BYTES);
    int memory = !a->file || du_ta_init(&a->text, ST_TEXT_BYTES);
    a->text_ready = !memory;
    setup_answers_defaults(&a->answers);
    a->tz = st_zone_index(0); a->kbd = 1;
    a->now = now_ns(a);
    st_record(a, "[setup] start version=1 design=%dx%d", ST_W, ST_H);
    if (memory) {
        /* D9: an allocation failure is a stop, never a degraded Setup. */
        a->failed_class = SETUP_ERR_MEMORY;
        stopped(a, st_error_id(SETUP_ERR_MEMORY), "start");
        a->page = ST_PAGE_FAILED;
        st_page_build(a);
        return 0;
    }
    st_inventory(a);
    st_hw_evaluate(a);
    st_licences_load(a);
    int answers = st_answers_load(a);
    for (unsigned i = 0; i < st_keyboard_count; i++) if (!strcmp(st_keyboards[i].id, a->answers.keyboard)) a->kbd = (int)i;
    int zone = st_zone_index(a->answers.tz_minutes);
    if (zone >= 0) a->tz = zone;
    a->clock_utc = a->answers.clock == SETUP_CLOCK_UTC;
    a->net_manual = a->answers.network == SETUP_NET_STATIC;
    st_record(a, "[setup] mode=%s answers=%s hardware=%s", a->unattended ? "unattended" : "interactive",
              a->answers.source[0] ? a->answers.source : "none", a->stop ? st_stop_code(a->stop) : "supported");
    if (a->stop) {
        static const char *const keys[] = {"none", "video", "disk0", "cd", "disk"};
        if (a->unattended) stopped(a, "hardware", keys[a->stop]);
        else st_record(a, "[setup] refused reason=hardware key=%s code=%s", keys[a->stop], st_stop_code(a->stop));
        st_goto(a, ST_PAGE_STOP);
        return 0;
    }
    if (answers < 0) {
        stopped(a, a->status.reason, a->status.key);
        st_goto(a, ST_PAGE_STATUS);
        return 0;
    }
    st_goto(a, ST_PAGE_WELCOME);
    if (a->unattended) start_unattended(a);
    return 0;
}

void st_free(struct st_app *a)
{
    if (a->text_ready) du_ta_free(&a->text);
    free(a->file); a->file = NULL; a->text_ready = 0;
}

void st_goto(struct st_app *a, enum st_page p)
{
    if (a->page != p || !a->ui->count) st_log(a, "page %s -> %s", st_page_name(a->page), st_page_name(p));
    a->page = p;
    if (p == ST_PAGE_RESTART) {
        a->restart_state = 0;
        a->countdown_deadline = now_ns(a) + (uint64_t)ST_COUNTDOWN_S * 1000000000ull;
    }
    st_page_build(a);
}

/* ---- validation of the pages that take typed answers ---- */
static int parse_clock(const char *s, int64_t *local)
{
    int y, mo, d, h, mi; char tail;
    if (sscanf(s, "%4d-%2d-%2d %2d:%2d%c", &y, &mo, &d, &h, &mi, &tail) != 5) return -1;
    if (y < 1999 || y > 2099 || mo < 1 || mo > 12 || d < 1 || h > 23 || mi > 59 || h < 0 || mi < 0) return -1;
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100) || y % 400 == 0;
    if (d > days[mo - 1] + (mo == 2 && leap)) return -1;
    /* Days from 1970-01-01 (proleptic Gregorian). */
    int64_t yy = y - (mo <= 2), era = yy / 400, yoe = yy - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    *local = (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60;
    return 0;
}

static int mask_prefix(const uint8_t m[4], unsigned *prefix)
{
    uint32_t v = (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
    unsigned n = 0;
    while (n < 32 && (v & (0x80000000u >> n))) n++;
    if (n < 32 && (v << n)) return -1;
    *prefix = n;
    return 0;
}

static int invalid(struct st_app *a, int focus, const char *text)
{
    du_focus(a->ui, focus);
    st_message(a, "CiukiOS Setup", text, DU_DLG_ICON_WARNING);
    return -1;
}

static int validate(struct st_app *a)
{
    struct setup_answers *s = &a->answers;
    if (a->page == ST_PAGE_USER) {
        if (!setup_valid_user(s->user_name)) return invalid(a, ST_ID_USER, "Type your name: 1 to 32 characters.");
        if (!setup_valid_computer(s->computer_name))
            return invalid(a, ST_ID_COMPUTER, "Type a computer name of 1 to 15 letters, digits or hyphens. It cannot be only digits or start or end with a hyphen.");
        int64_t local;
        if (parse_clock(a->clock_text, &local)) return invalid(a, ST_ID_CLOCK, "Type the date and time as YYYY-MM-DD HH:MM.");
        int64_t now = a->h.realtime ? a->h.realtime(a->h.ctx) : 0;
        int64_t shown = a->clock_shown - a->clock_shown % 60;   /* the minute that was shown */
        int64_t typed = local - (int64_t)s->tz_minutes * 60;
        s->clock_adjust_s = a->clock_edited && typed != shown + 0 ? typed - (now - now % 60) : 0;
        s->present |= 1u << SETUP_KEY_USER_NAME | 1u << SETUP_KEY_COMPUTER_NAME | 1u << SETUP_KEY_KEYBOARD |
                      1u << SETUP_KEY_TIME_ZONE | 1u << SETUP_KEY_CLOCK;
    }
    if (a->page == ST_PAGE_NETWORK) {
        if (a->adapter_none) s->network = SETUP_NET_NONE;
        else if (!a->net_manual) s->network = SETUP_NET_DHCP;
        else {
            uint8_t ip[4], mask[4], gw[4] = {0}, dns[4] = {0};
            unsigned prefix = 0;
            const struct du_widget *w[4];
            for (int i = 0; i < 4; i++) w[i] = du_get(a->ui, ST_ID_IP + i);
            static const char text[] = "Type a valid IP address and subnet mask, or choose Automatic (DHCP). The gateway and the DNS server may be left empty.";
            if (!w[0] || setup_parse_ipv4(w[0]->text, ip)) return invalid(a, ST_ID_IP, text);
            if (!w[1] || setup_parse_ipv4(w[1]->text, mask) || mask_prefix(mask, &prefix)) return invalid(a, ST_ID_MASK, text);
            if (w[2] && w[2]->text[0] && setup_parse_ipv4(w[2]->text, gw)) return invalid(a, ST_ID_GATEWAY, text);
            if (w[3] && w[3]->text[0] && setup_parse_ipv4(w[3]->text, dns)) return invalid(a, ST_ID_DNS, text);
            if (!setup_static_valid(ip, prefix, gw, dns)) return invalid(a, ST_ID_IP, text);
            s->network = SETUP_NET_STATIC; s->prefix = (uint8_t)prefix;
            memcpy(s->ip, ip, 4); memcpy(s->gateway, gw, 4); memcpy(s->dns, dns, 4);
        }
        s->present |= 1u << SETUP_KEY_NETWORK | 1u << SETUP_KEY_IPX;
    }
    if (a->page == ST_PAGE_LICENCE && !s->accept_licences) return -1;
    if (a->page == ST_PAGE_LICENCE) s->present |= 1u << SETUP_KEY_ACCEPT_LICENCES;
    if (a->page == ST_PAGE_COMPONENTS) {
        if (!setup_components_valid(s->components)) return -1;
        s->present |= 1u << SETUP_KEY_COMPONENTS;
    }
    if (a->page == ST_PAGE_DISK) {
        if (a->stop) return -1;
        s->disk = SETUP_DISK_ATA0; s->present |= 1u << SETUP_KEY_DISK;
    }
    return 0;
}

/* ---- prepare (summary page) ---- */
static void prepare(struct st_app *a)
{
    struct setup_answers *s = &a->answers;
    if (!s->unattended) { s->erase_disk = 1; s->disk = SETUP_DISK_ATA0; }   /* consent: Install on the summary */
    s->hardware_report = a->report; s->hardware_report_length = a->report_length;
    s->log = &a->log;
    memset(&a->plan, 0, sizeof(a->plan));
    int r = setup_txn_prepare(s, &a->plan);
    a->plan_ok = r == 0;
    st_log(a, "prepare result=%d required=%" PRIu64 " overhead=%" PRIu64 " reserve=%" PRIu64 " free_after=%" PRIu64,
           r, a->plan.required_bytes, a->plan.overhead_bytes, a->plan.reserve_bytes, a->plan.free_after_bytes);
    if (r == 0) { st_goto(a, ST_PAGE_SUMMARY); return; }
    enum setup_error_class cls = (enum setup_error_class)(r < 0 ? -r : SETUP_ERR_INTERNAL);
    a->error_valid = !setup_txn_last_error(&a->error);
    if (a->unattended) {
        a->failed_class = cls;
        stopped(a, st_error_id(cls), "prepare");
        st_goto(a, ST_PAGE_FAILED);
        return;
    }
    int retry = cls == SETUP_ERR_SOURCE_READ || cls == SETUP_ERR_MEDIA_REMOVED;
    static const char *const both[] = {"Retry", "Cancel"}, *const cancel[] = {"Cancel"};
    char text[400];
    snprintf(text, sizeof(text), "%s\n\nNothing has been written to the disk.", st_error_text(cls));
    a->failed_class = cls;
    du_message(a->ui, ST_DLG_PREPARE, st_error_title(cls), text, DU_DLG_ICON_ERROR, retry ? both : cancel, retry ? 2 : 1, 0, retry ? 1 : 0);
}

/* ---- the transaction ---- */
static int is_system_path(const char *p)
{
    while (*p == '\\' || *p == '/') p++;
    static const char *const roots[] = {"system", "windows", "programs"};
    for (unsigned i = 0; i < 3; i++) {
        size_t n = strlen(roots[i]), k = 0;
        while (k < n && p[k] && (p[k] | 0x20) == roots[i][k]) k++;
        if (k == n && (p[n] == '\\' || p[n] == '/' || !p[n])) return i < 2;
    }
    return 0;
}

static int pump(struct st_app *a, int wait)
{
    struct ciuki_input_event events[CIUKI_INPUT_READ_MAX];
    int n = a->h.input ? a->h.input(a->h.ctx, events, CIUKI_INPUT_READ_MAX) : -1;
    for (int i = 0; i < n; i++) st_input_event(a, &events[i]);
    a->now = now_ns(a);
    if (a->ui->dirty && a->h.present) { st_render(a); a->h.present(a->h.ctx, a->ui->canvas.pixels, a->ui->canvas.width, a->ui->canvas.height); }
    if (n == 0 && wait && a->h.idle) a->h.idle(a->h.ctx, 10000000u);
    return n;
}

static void cb_progress(void *ctx, const struct setup_progress *p)
{
    struct st_app *a = ctx;
    a->progress = *p; a->progress.step = NULL;
    snprintf(a->step, sizeof(a->step), "%s", p->step ? p->step : "");
    if (p->phase == SETUP_PHASE_COPY && a->step[0]) {
        int system = is_system_path(a->step);
        a->copy_row_system = system; a->copy_row_programs = !system;
    }
    a->ui->dirty = 1;
    uint64_t now = now_ns(a);
    if (now - a->last_frame >= 50000000u || p->phase == SETUP_PHASE_COMPLETE) { a->last_frame = now; pump(a, 0); }
}

static int modal(struct st_app *a, int id)
{
    a->modal_id = id; a->modal_result = -1;
    a->ui->dirty = 1;
    while (a->modal_result < 0) {
        if (pump(a, 1) < 0) { st_log(a, "input closed during dialog %d", id); du_dialog_close(a->ui); a->modal_result = 1000; }
        if (a->quit) break;
    }
    a->modal_id = 0;
    return a->modal_result;
}

static enum setup_decision cb_error(void *ctx, const struct setup_error *e)
{
    struct st_app *a = ctx;
    a->error = *e; a->error_valid = 1;
    int retry = e->retryable && e->attempt < SETUP_RETRY_MAX &&
                (e->cls == SETUP_ERR_SOURCE_READ || e->cls == SETUP_ERR_MEDIA_REMOVED);
    st_log(a, "error class=%s err=%d phase=%d path=%s lba=%" PRIu64 " attempt=%u retryable=%d",
           st_error_id(e->cls), e->err, e->phase, e->path, e->lba, e->attempt, e->retryable);
    if (a->unattended) return retry ? SETUP_DECISION_RETRY : SETUP_DECISION_CANCEL;
    if (a->ui->dialog.kind && a->ui->dialog.id == ST_DLG_CANCEL) du_dialog_close(a->ui);
    char text[700], where[300] = "";
    if (e->path[0]) snprintf(where, sizeof(where), "\n\nFile: %s", e->path);
    else if (e->count) snprintf(where, sizeof(where), "\n\nSectors: LBA %" PRIu64 ", %" PRIu64 " sectors", e->lba, e->count);
    snprintf(text, sizeof(text), "%s%s\n\nAttempt %u of %u.", st_error_text(e->cls), where, e->attempt, SETUP_RETRY_MAX);
    static const char *const both[] = {"Retry", "Cancel"}, *const cancel[] = {"Cancel"};
    du_message(a->ui, ST_DLG_ERROR, st_error_title(e->cls), text, DU_DLG_ICON_ERROR, retry ? both : cancel, retry ? 2 : 1, 0, retry ? 1 : 0);
    int choice = modal(a, ST_DLG_ERROR);
    st_log(a, "error decision=%s", retry && choice == 0 ? "retry" : "cancel");
    return retry && choice == 0 ? SETUP_DECISION_RETRY : SETUP_DECISION_CANCEL;
}

static bool cb_cancel(void *ctx)
{
    struct st_app *a = ctx;
    uint64_t now = now_ns(a);
    if (now - a->last_frame >= 50000000u) { a->last_frame = now; pump(a, 0); }
    return a->cancel != 0;
}

static void cb_log(void *ctx, const char *line) { setup_log_append(&((struct st_app *)ctx)->log, line); }

static void install(struct st_app *a)
{
    if (a->page != ST_PAGE_SUMMARY || !a->plan_ok || a->executing) return;
    a->plan.answers.log = &a->log;
    a->plan.answers.hardware_report = a->report; a->plan.answers.hardware_report_length = a->report_length;
    a->execute_calls++;
    a->executing = 1; a->cancel = 0; a->error_valid = 0;
    memset(&a->progress, 0, sizeof(a->progress)); a->step[0] = 0;
    a->tip = 0; a->tip_deadline = now_ns(a) + (uint64_t)ST_TIP_S * 1000000000ull;
    st_record(a, "[setup] install disk=0 components=0x%02x unattended=%d", a->plan.components, a->unattended);
    st_goto(a, ST_PAGE_COPY);
    pump(a, 0);
    static const struct setup_callbacks callbacks = {cb_progress, cb_error, cb_cancel, cb_log};
    int r = setup_txn_execute(&a->plan, &callbacks, a);
    a->executing = 0;
    if (a->ui->dialog.kind) du_dialog_close(a->ui);
    st_log(a, "execute result=%d", r);
    if (r == 0) {
        a->progress.phase = SETUP_PHASE_COMPLETE;
        a->dest_letter = a->h.dest_letter ? a->h.dest_letter(a->h.ctx) : 0;
        char line[200];
        snprintf(line, sizeof(line), "[setup] status=complete release=%s disk=0 letter=%c", a->release[0] ? a->release : "unknown",
                 a->dest_letter ? a->dest_letter : '?');
        st_record(a, "%s", line);
        status_file(a, line + 8);
        st_goto(a, ST_PAGE_RESTART);
        return;
    }
    enum setup_error_class cls = (enum setup_error_class)(r < 0 && -r <= SETUP_ERR_INTERNAL ? -r : SETUP_ERR_INTERNAL);
    struct setup_error last;
    if (!setup_txn_last_error(&last) && last.cls != SETUP_ERR_NONE) { a->error = last; a->error_valid = 1; }
    a->failed_class = cls;
    if (cls != SETUP_ERR_CANCELLED && !a->unattended) {
        /* D8: one dialog; Retry only for retryable errors, which libsetup
         * already offered through on_error. */
        char text[700], where[300] = "";
        if (a->error_valid && a->error.path[0]) snprintf(where, sizeof(where), "\n\nFile: %s", a->error.path);
        else if (a->error_valid && a->error.count) snprintf(where, sizeof(where), "\n\nSectors: LBA %" PRIu64 ", %" PRIu64 " sectors", a->error.lba, a->error.count);
        snprintf(text, sizeof(text), "%s%s", st_error_text(cls), where);
        static const char *const cancel[] = {"Cancel"};
        du_message(a->ui, ST_DLG_ERROR, st_error_title(cls), text, DU_DLG_ICON_ERROR, cancel, 1, 0, 0);
        modal(a, ST_DLG_ERROR);
    }
    static const char *const phases[] = {"plan", "partition", "format", "copy", "configure", "publish", "complete"};
    stopped(a, st_error_id(cls), phases[a->error_valid && a->error.phase <= SETUP_PHASE_COMPLETE ? a->error.phase : a->progress.phase]);
    st_goto(a, ST_PAGE_FAILED);
}

/* ---- restart (screen 10) ---- */
static void reboot(struct st_app *a)
{
    a->restart_state = 2; a->ui->dirty = 1;
    st_record(a, "[setup] restart policy=%s", a->answers.restart == SETUP_RESTART_WAIT ? "wait" :
              a->answers.restart == SETUP_RESTART_FORCE ? "force" : "eject");
    a->restart_requested = 1;
    if (a->h.reboot) a->h.reboot(a->h.ctx);
    a->quit = 1;
}

static void begin_restart(struct st_app *a)
{
    if (a->restart_state) return;
    uint64_t now = now_ns(a);
    a->restart_state = 1; a->ui->dirty = 1;
    if (a->answers.restart != SETUP_RESTART_FORCE && a->h.eject) {
        int r = a->h.eject(a->h.ctx);
        st_log(a, "eject result=%d", r);
    }
    a->eject_deadline = a->answers.restart == SETUP_RESTART_WAIT ? UINT64_MAX : now + (uint64_t)ST_EJECT_WAIT_S * 1000000000ull;
}

static void restart_tick(struct st_app *a)
{
    uint64_t now = a->now;
    if (a->restart_state == 0) {
        if (now >= a->countdown_deadline) begin_restart(a);
        else {
            static uint64_t shown;
            uint64_t s = (a->countdown_deadline - now) / 1000000000ull;
            if (s != shown) { shown = s; a->ui->dirty = 1; }
        }
        return;
    }
    if (a->restart_state != 1) return;
    int present = a->h.media_present ? a->h.media_present(a->h.ctx) : -1;
    if ((a->answers.restart != SETUP_RESTART_FORCE && present == 0) || now >= a->eject_deadline) reboot(a);
}

/* ---- rescue ---- */
static void rescue_open(struct st_app *a)
{
    a->rescue_return = a->page;
    int n = a->h.volumes ? a->h.volumes(a->h.ctx, a->volumes, 26) : -1;   /* a USB drive may be new */
    a->volume_count = n > 0 ? n : 0;
    a->status_line[0] = 0;
    st_goto(a, ST_PAGE_RESCUE);
}

static void text_open(struct st_app *a, const char *title, const char *path, const char *text, size_t length)
{
    int r = st_text_load(a, title, path, text, length);
    if (r) {
        char line[200]; snprintf(line, sizeof(line), "Setup cannot read %s (error %d).", path ? path : title, -r);
        st_message(a, "CiukiOS Setup", line, DU_DLG_ICON_WARNING);
        return;
    }
    a->back_page = a->page;
    st_goto(a, ST_PAGE_TEXT);
}

void st_licence_open(struct st_app *a, unsigned index)
{
    if (index >= a->licence_count) return;
    char path[128];
    snprintf(path, sizeof(path), "C:\\system\\licenses\\%s", a->licence_names[index]);
    text_open(a, a->licence_names[index], path, NULL, 0);
}

/* ---- navigation ---- */
static int pre_install(enum st_page p) { return p <= ST_PAGE_SUMMARY; }

static void next(struct st_app *a)
{
    st_page_collect(a);
    if (validate(a)) return;
    switch (a->page) {
    case ST_PAGE_WELCOME: st_goto(a, ST_PAGE_LICENCE); break;
    case ST_PAGE_LICENCE: st_goto(a, a->stop ? ST_PAGE_STOP : ST_PAGE_HARDWARE); break;
    case ST_PAGE_HARDWARE: if (!a->stop) st_goto(a, ST_PAGE_DISK); break;
    case ST_PAGE_DISK: st_goto(a, ST_PAGE_COMPONENTS); break;
    case ST_PAGE_COMPONENTS: st_goto(a, ST_PAGE_USER); break;
    case ST_PAGE_USER: st_goto(a, ST_PAGE_NETWORK); break;
    case ST_PAGE_NETWORK: prepare(a); break;
    case ST_PAGE_SUMMARY: install(a); break;
    default: break;
    }
}

static void back(struct st_app *a)
{
    if (a->page <= ST_PAGE_WELCOME || a->page > ST_PAGE_SUMMARY) return;
    st_page_collect(a);
    if (a->page == ST_PAGE_USER) {   /* keep what was typed even when it is not valid yet */
        snprintf(a->answers.user_name, sizeof(a->answers.user_name), "%s", a->answers.user_name);
    }
    st_goto(a, (enum st_page)(a->page - 1));
}

static void exit_request(struct st_app *a)
{
    if (!pre_install(a->page)) {
        if (a->page == ST_PAGE_COPY) st_action(a, ST_ID_CANCEL);
        return;
    }
    static const char *const buttons[] = {"Exit Setup", "Resume"};
    du_message(a->ui, ST_DLG_EXIT, "Exit Setup",
               "CiukiOS is not installed. Nothing has been written to the disk.\n\nExit Setup and restart the computer?",
               DU_DLG_ICON_QUESTION, buttons, 2, 1, 1);
}

void st_action(struct st_app *a, int id)
{
    if (a->executing && id != ST_ID_CANCEL) return;
    switch (id) {
    case ST_ID_NEXT: next(a); break;
    case ST_ID_BACK: back(a); break;
    case ST_ID_EXIT: exit_request(a); break;
    case ST_ID_CANCEL:
        if (a->page == ST_PAGE_COPY && a->executing && !a->cancel && !a->ui->dialog.kind) {
            static const char *const buttons[] = {"Cancel installation", "Continue"};
            du_message(a->ui, ST_DLG_CANCEL, "Cancel the installation",
                       "If you cancel now, the disk is not bootable. Restart Setup from the CD to install again.\n\nCancel the installation?",
                       DU_DLG_ICON_WARNING, buttons, 2, 1, 1);
        }
        break;
    case ST_ID_RESTART:
        if (a->page == ST_PAGE_RESTART) begin_restart(a);
        else reboot(a);
        break;
    case ST_ID_SHUTDOWN:
        st_record(a, "[setup] power off");
        a->power_requested = 1;
        if (a->h.power_off) a->h.power_off(a->h.ctx);
        a->quit = 1;
        break;
    case ST_ID_SAVE_REPORT: case ST_ID_LINK_SAVE: {
        char result[200];
        int r = st_save_report(a, result, sizeof(result));
        st_message(a, "Hardware report", result, r ? DU_DLG_ICON_WARNING : DU_DLG_ICON_INFO);
        break;
    }
    case ST_ID_RESCUE: rescue_open(a); break;
    case ST_ID_COPY_LOGS: st_rescue_copy(a, a->status_line, sizeof(a->status_line)); a->ui->dirty = 1; break;
    case ST_ID_REPORT: text_open(a, "Hardware report", NULL, a->report, a->report_length); break;
    case ST_ID_CLOSE:
        if (a->page == ST_PAGE_TEXT) st_goto(a, a->back_page);
        else if (a->page == ST_PAGE_RESCUE) st_goto(a, a->rescue_return);
        break;
    case ST_ID_DEFAULTS: st_components_defaults(a); break;
    }
}

static void dialog_result(struct st_app *a, int id, int index)
{
    if (a->modal_id && id == a->modal_id) { a->modal_result = index; return; }
    switch (id) {
    case ST_DLG_EXIT:
        if (index == 0) { st_record(a, "[setup] exit page=%s", st_page_name(a->page)); reboot(a); }
        break;
    case ST_DLG_CANCEL:
        if (index == 0 && a->executing) { a->cancel = 1; st_log(a, "cancel requested"); }
        break;
    case ST_DLG_PREPARE:
        if (index == 0 && (a->failed_class == SETUP_ERR_SOURCE_READ || a->failed_class == SETUP_ERR_MEDIA_REMOVED)) prepare(a);
        else if (a->failed_class == SETUP_ERR_DEST_FULL) st_goto(a, ST_PAGE_COMPONENTS);
        break;
    default: break;
    }
}

static void default_action(struct st_app *a)
{
    for (unsigned i = 0; i < a->ui->count; i++) {
        struct du_widget *w = &a->ui->widgets[i];
        if (w->kind == DU_BUTTON && w->def && !w->hidden) { if (!w->disabled) st_action(a, w->id); return; }
    }
}

static void escape(struct st_app *a)
{
    switch (a->page) {
    case ST_PAGE_WELCOME: exit_request(a); break;
    case ST_PAGE_LICENCE: case ST_PAGE_HARDWARE: case ST_PAGE_DISK: case ST_PAGE_COMPONENTS:
    case ST_PAGE_USER: case ST_PAGE_NETWORK: case ST_PAGE_SUMMARY: back(a); break;
    case ST_PAGE_COPY: st_action(a, ST_ID_CANCEL); break;
    case ST_PAGE_RESCUE: case ST_PAGE_TEXT: st_action(a, ST_ID_CLOSE); break;
    default: break;
    }
}

static void on_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct st_app *a = user;
    (void)u;
    switch (e->type) {
    case DU_EV_ACTIVATE: st_action(a, e->id); break;
    case DU_EV_DIALOG: dialog_result(a, e->id, e->index); break;
    case DU_EV_COMMIT: if (!a->executing) default_action(a); break;
    default: st_page_event(a, e); break;
    }
}

void st_input_event(struct st_app *a, const struct ciuki_input_event *ev)
{
    struct du_ui *u = a->ui;
    u->event = on_event; u->user = a;
    if (ev->type == CIUKI_INPUT_KEY && ev->value && ev->code >= 0 && ev->code <= 0x200) {
        unsigned code = (unsigned)ev->code;
        if (code == DU_KEY_LSHIFT || code == DU_KEY_RSHIFT || code == DU_KEY_LCTRL || code == DU_KEY_RCTRL ||
            code == DU_KEY_LALT || code == DU_KEY_RALT || code == DU_KEY_CAPS) { du_input(u, ev); return; }
        u->keys[code] = 1;
        if (a->popup && !u->dialog.kind) {
            if (st_popup_key(a, code)) return;
        }
        if (u->dialog.kind) { du_input(u, ev); return; }
        struct du_widget *f = u->focus >= 0 && (unsigned)u->focus < u->count ? &u->widgets[u->focus] : NULL;
        switch (code) {
        case DU_KEY_F1:
            if (!a->executing || a->page == ST_PAGE_COPY) {
                static const char *const ok[] = {"OK"};
                du_message(u, ST_DLG_HELP, "CiukiOS Setup Help", st_help(a->page), DU_DLG_ICON_INFO, ok, 1, 0, 0);
            }
            return;
        case 0x3d: /* F3 */ exit_request(a); return;
        case DU_KEY_F10:
            if ((pre_install(a->page) || a->page == ST_PAGE_STOP || a->page == ST_PAGE_FAILED || a->page == ST_PAGE_STATUS) && !a->executing)
                rescue_open(a);
            return;
        case DU_KEY_ESC: escape(a); return;
        case DU_KEY_ENTER:
            if (f && f->kind == DU_BUTTON && !f->disabled) break;
            if (f && f->kind == DU_CANVAS) {
                struct du_event e = {.type = DU_EV_KEY, .id = f->id, .key = code};
                if (st_page_event(a, &e)) return;
            }
            if (!a->executing) default_action(a);
            return;
        }
    }
    if (ev->type == CIUKI_INPUT_BUTTON && ev->value && a->popup && !u->dialog.kind) {
        st_popup_click(a, u->mouse_x, u->mouse_y);
        return;
    }
    du_input(u, ev);
}

void st_key(struct st_app *a, unsigned code, int value)
{
    struct ciuki_input_event e = {.type = CIUKI_INPUT_KEY, .code = (int32_t)code, .value = value, .monotonic_ns = now_ns(a)};
    st_input_event(a, &e);
}

/* ---- unattended: the same pages, skipped while their answers are complete ---- */
static void start_unattended(struct st_app *a)
{
    a->answers.accept_licences = 1;
    st_log(a, "unattended start");
}

static void unattended_tick(struct st_app *a)
{
    if (!a->unattended || a->ui->dialog.kind || a->executing) return;
    if (a->page < ST_PAGE_SUMMARY) { st_log(a, "unattended skip %s", st_page_name(a->page)); next(a); }
    else if (a->page == ST_PAGE_SUMMARY && a->plan_ok) install(a);
    else if (a->page == ST_PAGE_RESTART && a->restart_state == 0) begin_restart(a);
}

static void tick(struct st_app *a)
{
    a->now = now_ns(a);
    if (a->page == ST_PAGE_RESTART) restart_tick(a);
    if (a->page == ST_PAGE_COPY && a->now >= a->tip_deadline) {
        a->tip = (a->tip + 1) % st_tip_count;
        a->tip_deadline = a->now + (uint64_t)ST_TIP_S * 1000000000ull;
        a->ui->dirty = 1;
    }
    unattended_tick(a);
}

int st_turn(struct st_app *a)
{
    a->ui->event = on_event; a->ui->user = a;
    struct ciuki_input_event events[CIUKI_INPUT_READ_MAX];
    int n = a->h.input ? a->h.input(a->h.ctx, events, CIUKI_INPUT_READ_MAX) : -1;
    for (int i = 0; i < n && !a->quit; i++) st_input_event(a, &events[i]);
    if (!a->quit) tick(a);
    if (a->ui->dirty && a->h.present) {
        st_render(a);
        a->h.present(a->h.ctx, a->ui->canvas.pixels, a->ui->canvas.width, a->ui->canvas.height);
    }
    if (n < 0 && !a->unattended) return -1;
    if (n == 0 && !a->quit && a->h.idle) a->h.idle(a->h.ctx, 10000000u);
    return a->quit;
}

void st_run(struct st_app *a)
{
    while (!st_turn(a)) {}
}
