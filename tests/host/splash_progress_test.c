/* f0-55: boot splash progress bar on the production console (console.c),
 * with the embedded Ciuki photograph and CiukiOS Sans. Golden frames are
 * written to argv[1] and hashed by tests/host/test_splash_progress.py; this
 * harness checks the phase mapping, incremental == full redraws, the claim
 * budget, deferred claims and the display-grant handover.
 * f0-55b: phases reported before the first paint are on the first frame,
 * the worker's 40 ms turns alone move the sweep (no phase, no klog), the
 * worker runs above init while the splash is up, and the serial phase
 * records `[console] splash phase=<n> permille=<p>` (at most 14).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/fbdev.h>
#include <ciuki/init.h>
#define CIUKI_CPU_H
#define P2V(p) ((void *)(uintptr_t)(p))
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "splash:%d: %s\n", __LINE__, #c); exit(1); } } while (0)

struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
uint64_t g_tsc_per_ms = 1000000;            /* cpu_path_clock is absent (weak) on the host */
const uint8_t font_cfn_regular[95 + 95 * 32] = { 0 };
__asm__(".pushsection .rodata\n.global font_cfn_sans\nfont_cfn_sans:\n"
        ".incbin \"" CIUKI_TEST_SANS_CFN "\", 64, 3135\n.popsection\n");

static uint8_t *lfb, *claim_copy;
static size_t lfb_size;
static uint32_t flags = 0x200, lfb_pitch, lfb_bytes;
static unsigned claims, yields;
static bool claim_held, deny_claim, grant_at_yield, check_claims;
static char last_log[256];
#define LOG_MAX 256u
static char logs[LOG_MAX][256];             /* klog lines since log_count = 0 */
static unsigned log_count;
static uint32_t boot_flags;                 /* g_boot.flags of the next screen() */
static bool keep_progress;                  /* phases reported before that paint */
static struct task worker_task;             /* console_worker_init's task */
static bool worker_created;
static unsigned set_base_calls, set_level;
static enum task_prio set_prio;
static struct fb_device device;
static void claim_finished(void);

static uint32_t irq_save(void) { uint32_t f = flags; flags = 0; return f; }
static void irq_restore(uint32_t f) { flags = f; }
const struct fb_device *fbdev_get(void) { CHECK(flags & 0x200); return &device; }
bool fbdev_console_begin(void)
{
    CHECK(!claim_held);
    if (deny_claim) return false;
    claims++; claim_held = true;
    if (check_claims) memcpy(claim_copy, lfb, lfb_size);
    return true;
}
void fbdev_console_end(void)
{
    CHECK((flags & 0x200) && claim_held);
    claim_held = false;
    if (check_claims) claim_finished();
}
void fbdev_console_region(unsigned rows) { (void)rows; }
void *vmm_map_framebuffer(uint32_t p, uint32_t n) { (void)p; CHECK(n == lfb_size); return lfb; }
void vmm_revoke_mmio(void *p, uint32_t n) { (void)p; (void)n; }
void kpreempt_point(void)
{
    CHECK(!claim_held && (flags & 0x200));
    yields++;
    if (grant_at_yield) { grant_at_yield = false; console_display_owned(true); }
}
void serial_write(const char *s, size_t n) { (void)s; (void)n; }
void klog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    CHECK(log_count < LOG_MAX);
    memcpy(logs[log_count++], last_log, sizeof(last_log));
}
struct task *task_create_kernel(const char *n, void (*fn)(void *), void *arg, enum task_prio p)
{
    CHECK(!strcmp(n, "console") && fn && !arg && p == P_NORMAL);
    return worker_created ? &worker_task : 0;
}
void task_set_base(struct task *t, enum task_prio prio, unsigned level)
{
    CHECK(t == &worker_task && (flags & 0x200));
    set_base_calls++; set_prio = prio; set_level = level;
}
void task_start(struct task *t) { (void)t; }
void task_sleep_ms(uint32_t n) { (void)n; }
__attribute__((noreturn)) void panic(const char *s, ...) { fprintf(stderr, "%s\n", s); exit(1); }
#include "../../src/kernel/core/console.c"

static bool in_accel(uint32_t x, uint32_t y)
{
    const struct splash_layout *g = &splash_bar.g;
    return g->accel && x >= g->x0 && x < g->x0 + g->dw && y >= g->ay && y < g->ay + 17;
}
static bool in_bar(uint32_t x, uint32_t y)
{
    const struct splash_layout *g = &splash_bar.g;
    if (in_accel(x, y)) return true;           /* f1-97: the line under the bar */
    return x >= g->bx && x < g->bx + g->bw && y >= g->by && y < g->by + g->bh;
}
/* Every pixel a claim changed lies in the bar; the claim wrote at most
 * SPLASH_CLAIM_PIXELS pixels (the renderer's own count, checked below). */
static void claim_finished(void)
{
    for (uint32_t y = 0; y < g_boot.fb_height; y++)
        if (memcmp(claim_copy + y * lfb_pitch, lfb + y * lfb_pitch, lfb_pitch))
            for (uint32_t x = 0; x < g_boot.fb_width; x++)
                if (memcmp(claim_copy + y * lfb_pitch + x * lfb_bytes, lfb + y * lfb_pitch + x * lfb_bytes, lfb_bytes))
                    CHECK(in_bar(x, y));
}

static void screen(uint32_t w, uint32_t h, uint32_t bpp, bool quiet_boot, uint64_t ticks)
{
    free(lfb); free(claim_copy);
    lfb_bytes = bpp / 8; lfb_pitch = w * lfb_bytes;
    lfb_size = (size_t)lfb_pitch * h;
    lfb = malloc(lfb_size); claim_copy = malloc(lfb_size);
    CHECK(lfb && claim_copy);
    memset(lfb, 0x5A, lfb_size);                /* what the loader left */
    g_boot = (struct ciuki_boot_info){ .fb_phys = 0xE0000000u, .fb_pitch = lfb_pitch,
        .fb_width = w, .fb_height = h, .fb_bpp = (uint8_t)bpp };
    if (bpp == 16) {
        g_boot.fb_red_pos = 11; g_boot.fb_red_size = 5; g_boot.fb_green_pos = 5;
        g_boot.fb_green_size = 6; g_boot.fb_blue_pos = 0; g_boot.fb_blue_size = 5;
    } else {
        g_boot.fb_red_pos = 16; g_boot.fb_green_pos = 8; g_boot.fb_blue_pos = 0;
        g_boot.fb_red_size = g_boot.fb_green_size = g_boot.fb_blue_size = 8;
    }
    g_boot.flags = boot_flags;                  /* CBI_F_SERIAL_LOG: phase records */
    device = (struct fb_device){ 0 };            /* before fbdev_init: g_boot masks */
    display_owned = false; panic_drawing = false;
    if (!keep_progress) memset(&splash_bar, 0, sizeof(splash_bar));   /* a new boot */
    g_ticks = ticks;
    console_set_quiet(quiet_boot);
    CHECK(console_init_lfb());
}

static uint8_t *snapshot(void)
{
    uint8_t *copy = malloc(lfb_size);
    CHECK(copy);
    memcpy(copy, lfb, lfb_size);
    return copy;
}

/* Frames a and b differ only inside the bar. */
static void differ_only_in_bar(const uint8_t *a, const uint8_t *b)
{
    for (uint32_t y = 0; y < g_boot.fb_height; y++)
        for (uint32_t x = 0; x < g_boot.fb_width; x++)
            if (memcmp(a + y * lfb_pitch + x * lfb_bytes, b + y * lfb_pitch + x * lfb_bytes, lfb_bytes))
                CHECK(in_bar(x, y));
}

/* The incremental update left exactly what a full redraw of the same state
 * draws (the redraw must also leave no claim or state behind). */
static void matches_full_redraw(void)
{
    uint8_t *incremental = snapshot();
    unsigned before = claims;
    splash_bar.shown_valid = false;
    splash_update(true);
    CHECK(splash_bar.shown_valid && claims > before && !splash_bar.busy);
    CHECK(!memcmp(incremental, lfb, lfb_size));
    free(incremental);
}

static void save(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.raw", dir, name);
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(lfb, 1, lfb_size, f) == lfb_size && !fclose(f));
}

static uint32_t pixel(uint32_t x, uint32_t y)
{
    uint32_t v = 0;
    memcpy(&v, lfb + y * lfb_pitch + x * lfb_bytes, lfb_bytes);
    return v;
}

static void phase_mapping(void)
{
    CHECK(splash_phase_permille(SPLASH_PHASE_DRIVERS) > 0);
    for (unsigned p = 1; p < SPLASH_PHASES; p++)
        CHECK(splash_phase_permille((enum splash_phase)p) > splash_phase_permille((enum splash_phase)(p - 1)));
    CHECK(splash_phase_permille(SPLASH_PHASE_HANDOVER) == SPLASH_PERMILLE_FULL);
    CHECK(splash_phase_permille(SPLASH_PHASES) == 0);
    /* init.h activation kinds drive the storage phases. */
    CHECK(splash_storage_phase(ACTIVATION_DEVICE) == -1);
    CHECK(splash_storage_phase(ACTIVATION_STORAGE) == SPLASH_PHASE_DISK);
    CHECK(splash_storage_phase(ACTIVATION_MOUNT) == SPLASH_PHASE_VOLUME);
    CHECK(splash_storage_phase(ACTIVATION_STORAGE_IDENTITY) == SPLASH_PHASE_IDENTITY);
    CHECK(splash_storage_phase(4) == -1);
}

static const enum splash_phase normal_boot[] = {
    SPLASH_PHASE_DRIVERS, SPLASH_PHASE_FRAMEBUFFER, SPLASH_PHASE_INPUT, SPLASH_PHASE_ATA,
    SPLASH_PHASE_DISK, SPLASH_PHASE_VOLUME, SPLASH_PHASE_VOLUME, SPLASH_PHASE_IDENTITY,
    SPLASH_PHASE_STORAGE, SPLASH_PHASE_FILES, SPLASH_PHASE_AUDIO, SPLASH_PHASE_DEVICES,
    SPLASH_PHASE_SUPERVISOR, SPLASH_PHASE_DESKTOP,
};
/* safe=1: no ATA activation, so no disk scan or volume records. */
static const enum splash_phase safe_boot[] = {
    SPLASH_PHASE_DRIVERS, SPLASH_PHASE_FRAMEBUFFER, SPLASH_PHASE_INPUT, SPLASH_PHASE_ATA,
    SPLASH_PHASE_IDENTITY, SPLASH_PHASE_STORAGE, SPLASH_PHASE_FILES, SPLASH_PHASE_AUDIO,
    SPLASH_PHASE_DEVICES, SPLASH_PHASE_SUPERVISOR, SPLASH_PHASE_DESKTOP,
};

/* Feed a phase sequence through the production entry points, then grant
 * the display. Returns the handover frame. */
static uint8_t *boot_sequence(const enum splash_phase *phases, unsigned count)
{
    uint8_t *previous = snapshot();
    uint32_t level = 0;
    for (unsigned i = 0; i < count; i++) {
        console_splash_phase(phases[i]);
        if (splash_phase_permille(phases[i]) > level) level = splash_phase_permille(phases[i]);
        CHECK(splash_bar.determinate && splash_bar.level == level && splash_bar.shown_level == level);
        differ_only_in_bar(previous, lfb);
        matches_full_redraw();
        memcpy(previous, lfb, lfb_size);
    }
    /* A late or repeated record never moves the bar back. */
    unsigned before = claims;
    console_splash_phase(SPLASH_PHASE_DISK);
    CHECK(splash_bar.level == level && claims == before && !memcmp(previous, lfb, lfb_size));
    free(previous);
    console_display_owned(true);
    CHECK(display_owned && !splash_bar.active && splash_bar.shown_level == SPLASH_PERMILLE_FULL);
    CHECK(strstr(last_log, "[console] splash handover progress=1000 complete=1 ") != 0);
    CHECK(strstr(last_log, " max_claim_us=0") != 0);
    CHECK(splash_bar.max_claim_pixels <= SPLASH_CLAIM_PIXELS);
    return snapshot();
}

/* After the grant nothing of the console reaches the screen until the
 * desktop's first frame replaces the splash. */
static void after_handover_nothing_draws(void)
{
    uint8_t *handed = snapshot();
    unsigned before = claims;
    console_splash_phase(SPLASH_PHASE_DESKTOP);
    console_splash_phase(SPLASH_PHASE_HANDOVER);
    for (unsigned i = 0; i < 40; i++) console_write("[desktop] stage=startup\n", 24);
    g_ticks += 1000;
    console_service();
    CHECK(splash_service() == 100);
    console_display_owned(false);               /* the desktop died: quiet still */
    console_service();
    CHECK(splash_service() == 100);
    CHECK(claims == before && !memcmp(handed, lfb, lfb_size));
    bool checking = check_claims;
    check_claims = false;                       /* console rows, not the bar */
    console_wake();                             /* the boot log replaces it */
    g_ticks += 1000;
    for (unsigned i = 0; i < 100000 && (repaint_pending || paint.active); i++) console_service();
    CHECK(memcmp(handed, lfb, lfb_size) && !splash_bar.active);
    check_claims = checking;
    free(handed);
}

/* Columns of the moving segment inside the track. */
static int32_t visible_columns(int32_t segment)
{
    const struct splash_layout *g = &splash_bar.g;
    int32_t left = segment > 0 ? segment : 0, right = segment + (int32_t)splash_segment_width(g);
    if (right > (int32_t)g->bw) right = (int32_t)g->bw;
    return right - left;
}

/* f0-55b: with no phase and no klog line, the console worker's turns (one
 * per SPLASH_TICK_MS) move the sweep on every turn, through two sweeps and
 * their wraps, and no frame shows an empty track. */
static void sweep_moves_on_worker_turns(void)
{
    screen(1024, 768, 32, true, 0);
    const struct splash_layout *g = &splash_bar.g;
    int32_t stride = (int32_t)((g->bw + splash_segment_width(g)) / 40);
    CHECK(visible_columns(splash_bar.shown_segment) == (int32_t)splash_segment_width(g));
    log_count = 0;
    unsigned wraps = 0, phases = splash_bar.phases;
    uint8_t *previous = snapshot();
    for (unsigned turn = 0; turn < 80; turn++) {
        int32_t before = splash_bar.shown_segment;
        g_ticks += SPLASH_TICK_MS;
        CHECK(splash_service() == SPLASH_TICK_MS);
        int32_t now = splash_bar.shown_segment;
        if (now < before) { wraps++; CHECK(now == stride - (int32_t)splash_segment_width(g)); }
        else CHECK(now == before + stride);
        CHECK(visible_columns(now) >= stride);
        CHECK(memcmp(previous, lfb, lfb_size));
        differ_only_in_bar(previous, lfb);
        memcpy(previous, lfb, lfb_size);
    }
    CHECK(wraps == 2 && !log_count && !splash_bar.determinate && splash_bar.phases == phases);
    matches_full_redraw();
    /* Within one tick the turn draws nothing. */
    unsigned before = claims;
    CHECK(splash_service() == SPLASH_TICK_MS && claims == before && !memcmp(previous, lfb, lfb_size));
    free(previous);
}

/* f0-55b: the console worker runs above the boot task while the splash is
 * on screen and returns to P_NORMAL at the handover. */
static void worker_runs_above_init(void)
{
    CHECK(SPLASH_WORKER_LEVEL > SCHED_FOREGROUND + 1 && SPLASH_WORKER_LEVEL <= SCHED_BOOST_CAP);
    screen(1024, 768, 32, true, 0);
    worker_created = true; set_base_calls = 0;
    console_worker_init();
    CHECK(console_task == &worker_task && set_base_calls == 1 && set_prio == P_NORMAL &&
          set_level == SPLASH_WORKER_LEVEL);
    g_ticks += SPLASH_TICK_MS;
    CHECK(splash_service() == SPLASH_TICK_MS && set_base_calls == 1);
    console_splash_phase(SPLASH_PHASE_DRIVERS);
    CHECK(splash_service() == 100 && set_base_calls == 1);      /* still on screen */
    console_display_owned(true);
    CHECK(splash_service() == 100 && set_base_calls == 2 && set_prio == P_NORMAL && !set_level);
    CHECK(splash_service() == 100 && set_base_calls == 2);
    console_display_owned(false);
    /* A verbose boot never raises it. */
    bool checking = check_claims;
    check_claims = false;                       /* console rows, not the bar */
    screen(1024, 768, 32, false, 0);
    check_claims = checking;
    console_task = 0; splash_worker_raised = false; set_base_calls = 0;
    console_worker_init();
    CHECK(console_task == &worker_task && !set_base_calls);
    CHECK(splash_service() == 100 && !set_base_calls);
    console_task = 0; worker_created = false;
}

/* f0-55b: phases a quiet boot reports before the splash is painted are on
 * the first frame; the bar continues from there. */
/* f1-97/f2-92: `2D engine: NO (<reason>)`, `2D engine: OK (drawing: software)`
 * or, once the engine has drawn, `2D acceleration: YES` under the bar. */
static unsigned diff_outside_accel(const uint8_t *a, const uint8_t *b)
{
    unsigned inside = 0;
    for (uint32_t y = 0; y < g_boot.fb_height; y++)
        for (uint32_t x = 0; x < g_boot.fb_width; x++)
            if (memcmp(a + y * lfb_pitch + x * lfb_bytes, b + y * lfb_pitch + x * lfb_bytes, lfb_bytes)) {
                CHECK(in_accel(x, y));
                inside++;
            }
    return inside;
}

static uint8_t *accel_boot(const char *name, bool yes, bool used, const char *reason, const char *out, uint8_t *plain)
{
    screen(1024, 768, 32, true, 800);
    const struct splash_layout *g = &splash_bar.g;
    CHECK(g->accel && g->ay >= g->by + g->bh && g->ay + 17 <= g->y0 + g->dh);
    console_splash_phase(SPLASH_PHASE_DRIVERS);
    uint8_t *before = snapshot();
    unsigned claims_before = claims, logs_before = log_count;
    console_splash_accel(yes, used, reason);
    /* The caller never draws: the console worker does, in budgeted claims. */
    CHECK(claims == claims_before && !memcmp(before, lfb, lfb_size) && log_count == logs_before);
    free(before);
    CHECK(splash_service() == 100);
    CHECK(claims > claims_before + 1 && !splash_bar.accel_busy &&
          !memcmp(splash_bar.accel, splash_bar.accel_shown, SPLASH_ACCEL_TEXT));
    CHECK(splash_bar.max_claim_pixels <= SPLASH_CLAIM_PIXELS);      /* the bar's lock budget */
    uint8_t *shown = snapshot();
    claims_before = claims;
    splash_service();
    CHECK(claims == claims_before);                                 /* nothing left to draw */
    uint8_t *frame = boot_sequence(normal_boot, sizeof(normal_boot) / sizeof(normal_boot[0]));
    save(out, name);
    CHECK(diff_outside_accel(plain, frame) > 200);                  /* only the line differs, in ink */
    free(shown);
    return frame;
}

static void accel_indicator(const char *out)
{
    check_claims = true;
    screen(1024, 768, 32, true, 800);
    uint8_t *plain = boot_sequence(normal_boot, sizeof(normal_boot) / sizeof(normal_boot[0]));
    CHECK(!splash_bar.accel[0]);
    uint8_t *ok = accel_boot("splash-accel-ok", true, false, 0, out, plain);
    CHECK(!strcmp(splash_bar.accel, "2D engine: OK (drawing: software)"));
    uint8_t *yes = accel_boot("splash-accel-yes", true, true, 0, out, plain);
    CHECK(!strcmp(splash_bar.accel, "2D acceleration: YES"));
    const char *token = "bci_test_readback_failed";
    uint8_t *no = accel_boot("splash-accel-no", false, false, token, out, plain);
    CHECK(!strcmp(splash_bar.accel, "2D engine: NO (bci_test_readback_failed)"));
    CHECK(memcmp(yes, no, lfb_size) && memcmp(ok, yes, lfb_size) && memcmp(ok, no, lfb_size));
    const struct splash_layout *g = &splash_bar.g;
    uint32_t w = splash_string_width(splash_bar.accel);
    CHECK(g->x0 + (g->dw - w) / 2 >= g->x0 && w + 1 < g->dw);
    /* A state that changes (OK, YES after the first engine draw, then a
     * later quarantine) leaves no old ink. */
    screen(1024, 768, 32, true, 800);
    console_splash_phase(SPLASH_PHASE_DRIVERS);
    console_splash_accel(true, false, 0);
    splash_service();
    console_splash_accel(true, true, 0);
    splash_service();
    console_splash_accel(false, true, token);
    splash_service();
    uint8_t *flipped = boot_sequence(normal_boot, sizeof(normal_boot) / sizeof(normal_boot[0]));
    CHECK(!memcmp(flipped, no, lfb_size));
    free(flipped);
    /* A denied claim defers the line; the next worker turn finishes it. */
    screen(1024, 768, 32, true, 800);
    console_splash_accel(false, false, "unsupported_device");
    deny_claim = true;
    unsigned deferred = splash_bar.deferred;
    splash_service();
    CHECK(splash_bar.deferred > deferred && splash_bar.accel_shown[0] == 0 && !splash_bar.accel_busy);
    deny_claim = false;
    splash_service();
    CHECK(!strcmp(splash_bar.accel_shown, "2D engine: NO (unsupported_device)"));
    /* The handover draws a line still pending. */
    screen(1024, 768, 32, true, 800);
    console_splash_phase(SPLASH_PHASE_DRIVERS);
    console_splash_accel(true, false, 0);
    console_display_owned(true);
    CHECK(!strcmp(splash_bar.accel_shown, "2D engine: OK (drawing: software)"));
    /* Decided before the first paint: it is on the first frame. */
    memset(&splash_bar, 0, sizeof(splash_bar));
    console_set_quiet(true);
    console_splash_accel(false, false, "boot_disabled");
    keep_progress = true;
    screen(1024, 768, 32, true, 800);
    keep_progress = false;
    CHECK(!strcmp(splash_bar.accel_shown, "2D engine: NO (boot_disabled)"));
    /* No ink outside the photograph area on a small mode, and a tiny one has no line. */
    screen(640, 480, 32, true, 800);
    CHECK(splash_bar.g.accel);
    console_splash_accel(false, false, token);
    splash_service();
    CHECK(splash_bar.accel_shown[0] && splash_bar.max_claim_pixels <= SPLASH_CLAIM_PIXELS);
    free(plain); free(ok); free(yes); free(no);
    check_claims = false;
}

static void phases_before_first_paint(void)
{
    memset(&splash_bar, 0, sizeof(splash_bar));
    console_set_quiet(true);
    log_count = 0;
    console_splash_phase(SPLASH_PHASE_DRIVERS);
    console_splash_phase(SPLASH_PHASE_FRAMEBUFFER);
    console_splash_phase(SPLASH_PHASE_INPUT);
    CHECK(!splash_bar.active && splash_bar.phases == 3 && splash_bar.level == 80 && !log_count);
    keep_progress = true; boot_flags = CBI_F_SERIAL_LOG;
    unsigned claims_before = claims;
    screen(1024, 768, 32, true, 800);
    keep_progress = false;
    CHECK(splash_bar.active && splash_bar.determinate && splash_bar.shown_valid &&
          splash_bar.shown_determinate && splash_bar.shown_level == 80 && claims == claims_before);
    const struct splash_layout *g = &splash_bar.g;
    uint32_t mid = g->by + g->bh / 2, fill = splash_fill_width(g, 80);
    CHECK(fill > 2 * g->bh);                    /* more than the rounded ends */
    uint32_t inside = g->bx + fill / 2, outside = g->bx + fill + g->bh;
    CHECK(pixel(inside, mid) == pack(splash_blend(SPLASH_FILL_FROM, SPLASH_FILL_TO, (inside - g->bx) * 256 / (g->bw - 1))));
    CHECK(pixel(outside, mid) == pack(splash_blend(splash_photo(g, outside, mid), SPLASH_TRACK, SPLASH_TRACK_ALPHA)));
    matches_full_redraw();                      /* the first frame is the 8 % frame */
    CHECK(log_count == 1 && !strcmp(logs[0], "[console] splash phase=2 permille=80"));
    console_splash_phase(SPLASH_PHASE_ATA);
    CHECK(splash_bar.shown_level == 120 && log_count == 2 && !strcmp(logs[1], "[console] splash phase=3 permille=120"));
    /* After the handover a phase is ignored until the next boot. */
    console_display_owned(true);
    CHECK(splash_bar.ended && !splash_bar.active);
    unsigned phases = splash_bar.phases;
    console_display_owned(false);
    console_splash_phase(SPLASH_PHASE_AUDIO);
    CHECK(splash_bar.phases == phases && splash_bar.level == SPLASH_PERMILLE_FULL);
    boot_flags = 0;
}

/* Lines that start like a phase record, from logs[first]. */
static unsigned phase_lines(unsigned first)
{
    unsigned n = 0;
    for (unsigned i = first; i < log_count; i++) n += !strncmp(logs[i], "[console] splash phase=", 23);
    return n;
}

/* f0-55b: one `[console] splash phase=<n> permille=<p>` line per drawn
 * phase, in order, at most SPLASH_PHASES, the 100 % one just before the
 * handover summary; none without the serial log; a deferred phase is
 * recorded when the worker draws it. */
static void phase_records(void)
{
    static const enum splash_phase repeats[] = {
        SPLASH_PHASE_DRIVERS, SPLASH_PHASE_DRIVERS, SPLASH_PHASE_DISK, SPLASH_PHASE_DISK,
        SPLASH_PHASE_VOLUME, SPLASH_PHASE_VOLUME, SPLASH_PHASE_VOLUME, SPLASH_PHASE_IDENTITY,
        SPLASH_PHASE_STORAGE, SPLASH_PHASE_FILES, SPLASH_PHASE_AUDIO, SPLASH_PHASE_DEVICES,
        SPLASH_PHASE_SUPERVISOR, SPLASH_PHASE_DESKTOP,
    };
    boot_flags = CBI_F_SERIAL_LOG;
    screen(1024, 768, 32, true, 800);
    log_count = 0;
    free(boot_sequence(normal_boot, sizeof(normal_boot) / sizeof(normal_boot[0])));
    unsigned records = 0;
    for (unsigned i = 0; i < log_count; i++) {
        if (strncmp(logs[i], "[console] splash phase=", 23)) continue;
        char expect[64];
        snprintf(expect, sizeof(expect), "[console] splash phase=%u permille=%u",
                 records, splash_phase_permille((enum splash_phase)records));
        CHECK(!strcmp(logs[i], expect));        /* phases 0..13, in order */
        records++;
    }
    CHECK(records == SPLASH_PHASES && splash_bar.records == SPLASH_PHASES && log_count >= 2);
    CHECK(!strcmp(logs[log_count - 2], "[console] splash phase=13 permille=1000"));
    CHECK(!strncmp(logs[log_count - 1], "[console] splash handover progress=1000 complete=1 ", 51));
    /* Repeated and late phases add no line; nothing after the handover. */
    screen(1024, 768, 32, true, 800);
    log_count = 0;
    for (unsigned i = 0; i < sizeof(repeats) / sizeof(repeats[0]); i++) console_splash_phase(repeats[i]);
    console_splash_phase(SPLASH_PHASE_ATA);
    console_display_owned(true);
    for (unsigned p = 0; p <= SPLASH_PHASES; p++) console_splash_phase((enum splash_phase)p);
    CHECK(phase_lines(0) == 11 && splash_bar.records == 11);   /* 10 distinct levels + 100 % */
    console_display_owned(false);
    /* The serial log off (BOOT.CFG serial=0): no record. */
    boot_flags = 0;
    screen(1024, 768, 32, true, 800);
    log_count = 0;
    free(boot_sequence(normal_boot, sizeof(normal_boot) / sizeof(normal_boot[0])));
    CHECK(!phase_lines(0) && strstr(last_log, "[console] splash handover ") == last_log);
    /* A deferred update is recorded by the worker turn that draws it. */
    boot_flags = CBI_F_SERIAL_LOG;
    screen(1024, 768, 32, true, 800);
    log_count = 0;
    deny_claim = true;
    console_splash_phase(SPLASH_PHASE_STORAGE);
    CHECK(splash_bar.deferred == 1 && !log_count);
    deny_claim = false;
    CHECK(splash_service() == 100 && log_count == 1 && !strcmp(logs[0], "[console] splash phase=7 permille=600"));
    boot_flags = 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *out = argv[1];
    phase_mapping();

    /* Geometry: under the wordmark (ink ends at row 681 of 768), the
     * tagline above the bar, everything inside the photograph. */
    static const uint32_t modes[][2] = { {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 1024}, {1600, 1200} };
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        struct splash_layout g;
        splash_layout(modes[i][0], modes[i][1], &g);
        CHECK(g.bar && g.bh >= 4 && g.bh <= 9 && g.bw >= 240 && g.tw == splash_text_width());
        CHECK(g.ty > g.y0 + g.dh * 681 / 768 && g.ty + 17 <= g.by);
        CHECK(g.bx >= g.x0 && g.bx + g.bw <= g.x0 + g.dw && g.by + g.bh <= g.y0 + g.dh);
        CHECK(g.tx >= g.x0 && g.tx + g.tw + 1 <= g.x0 + g.dw);
        CHECK(SPLASH_CLAIM_PIXELS / g.bh * g.bh <= SPLASH_CLAIM_PIXELS);
    }
    struct splash_layout ref;
    splash_layout(1024, 768, &ref);
    CHECK(ref.bx == 320 && ref.bw == 384 && ref.by == 725 && ref.bh == 6 && ref.ty == 690);
    splash_layout(17, 35, &ref);
    CHECK(!ref.bar);                            /* tiny fixtures: photograph only */

    /* 1024x768x32 golden frames. Indeterminate at tick 0, the first frame
     * that kmain paints before the PIT runs: the segment is centred at
     * columns 144..240 of the 384-column track (f0-55b; f0-55 drew it at
     * -96..0 there, an empty track). */
    check_claims = true;
    screen(1024, 768, 32, true, 0);
    CHECK(splash_bar.active && !splash_bar.determinate && splash_bar.segment == 144);
    save(out, "splash-indeterminate");
    uint8_t *indeterminate = snapshot();
    CHECK(splash_service() == SPLASH_TICK_MS);  /* same tick: nothing to draw */
    CHECK(!claims && !memcmp(indeterminate, lfb, lfb_size));
    g_ticks += SPLASH_TICK_MS;                  /* the worker animates the sweep */
    CHECK(splash_service() == SPLASH_TICK_MS && claims == 1 && splash_bar.segment == 156);
    differ_only_in_bar(indeterminate, lfb);
    matches_full_redraw();
    /* Determinate 0 % (only the track) and 40 %, drawn incrementally. */
    splash_bar.determinate = true; splash_bar.level = 0;
    splash_update(true);
    matches_full_redraw();
    save(out, "splash-0");
    const struct splash_layout *g = &splash_bar.g;
    uint32_t mid = g->by + g->bh / 2;
    CHECK(pixel(g->bx + g->bw / 2, mid) == pack(splash_blend(splash_photo(g, g->bx + g->bw / 2, mid), SPLASH_TRACK, SPLASH_TRACK_ALPHA)));
    splash_bar.level = 400;
    splash_update(true);
    matches_full_redraw();
    save(out, "splash-40");
    uint32_t inside = g->bx + g->bw / 5, outside = g->bx + g->bw * 3 / 5;
    CHECK(pixel(inside, mid) == pack(splash_blend(SPLASH_FILL_FROM, SPLASH_FILL_TO, (inside - g->bx) * 256 / (g->bw - 1))));
    CHECK(pixel(outside, mid) == pack(splash_blend(splash_photo(g, outside, mid), SPLASH_TRACK, SPLASH_TRACK_ALPHA)));
    differ_only_in_bar(indeterminate, lfb);
    free(indeterminate);

    /* Normal boot through the production entry points, then the grant. */
    claims = 0;
    screen(1024, 768, 32, true, 800);
    uint8_t *normal = boot_sequence(normal_boot, sizeof(normal_boot) / sizeof(normal_boot[0]));
    save(out, "splash-100");
    CHECK(pixel(g->bx + g->bw - 1 - g->bh, mid) == pack(splash_blend(SPLASH_FILL_FROM, SPLASH_FILL_TO,
          (g->bw - 1 - g->bh) * 256 / (g->bw - 1))));
    after_handover_nothing_draws();
    /* safe=1 phase records end on the same frame. */
    screen(1024, 768, 32, true, 800);
    uint8_t *safe = boot_sequence(safe_boot, sizeof(safe_boot) / sizeof(safe_boot[0]));
    CHECK(!memcmp(normal, safe, lfb_size));
    free(safe);

    /* A busy presenter defers the update to the console worker. */
    screen(1024, 768, 32, true, 800);
    uint8_t *before = snapshot();
    deny_claim = true;
    console_splash_phase(SPLASH_PHASE_STORAGE);
    CHECK(splash_bar.deferred == 1 && !splash_bar.shown_valid && !memcmp(before, lfb, lfb_size));
    CHECK(splash_service() == SPLASH_TICK_MS && splash_bar.deferred == 2);
    deny_claim = false;
    CHECK(splash_service() == 100 && splash_bar.shown_level == 600);
    matches_full_redraw();
    free(before);

    /* The grant arrives at the yield between two claims of a full redraw:
     * the handover redraws everything at 100 % and the interrupted update
     * stops without touching the screen again. */
    screen(1024, 768, 32, true, 800);
    grant_at_yield = true;
    unsigned yields_before = yields;
    console_splash_phase(SPLASH_PHASE_DRIVERS);
    CHECK(!grant_at_yield && yields == yields_before + 1 && display_owned && !splash_bar.active);
    CHECK(!splash_bar.busy && !memcmp(normal, lfb, lfb_size));
    CHECK(strstr(last_log, "progress=1000 complete=1 ") != 0);
    free(normal);

    /* Verbose boots (safe=1, serial log, tests) keep the text console: no
     * splash, no bar, phases ignored. */
    check_claims = false;
    screen(1024, 768, 32, false, 800);
    CHECK(!splash_bar.active);
    uint8_t *verbose = snapshot();
    unsigned claimed = claims, phases = splash_bar.phases;
    console_splash_phase(SPLASH_PHASE_STORAGE);
    console_display_owned(true);
    CHECK(claims == claimed && splash_bar.phases == phases && !memcmp(verbose, lfb, lfb_size));
    free(verbose);

    /* 800x600x16 and 640x480x16: the same states inside the bar only. */
    check_claims = true;
    for (unsigned m = 0; m < 2; m++) {
        screen(m ? 640 : 800, m ? 480 : 600, 16, true, 800);
        uint8_t *first = snapshot();
        uint8_t *done = boot_sequence(normal_boot, sizeof(normal_boot) / sizeof(normal_boot[0]));
        differ_only_in_bar(first, done);
        free(first); free(done);
    }
    /* f0-55b: first frame, worker turns, worker class, phase records. */
    accel_indicator(out);
    sweep_moves_on_worker_turns();
    worker_runs_above_init();
    phases_before_first_paint();
    phase_records();
    free(lfb); free(claim_copy);
    printf("splash progress: phases, golden frames, claims <= %u pixels, deferral and handover, "
           "first frame, worker turns and phase records PASS\n", SPLASH_CLAIM_PIXELS);
    return 0;
}
