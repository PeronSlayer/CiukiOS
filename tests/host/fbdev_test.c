/* T0: production framebuffer renderer/probe with heap-backed LFB.
 * Build: scripts/test/host_kernel_tests.sh (ASan/UBSan).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/fbdev.h>
#include <ciuki/registry.h>

static unsigned failures, cases;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
struct ciuki_boot_info g_boot;
static struct resource reservation;
static uint8_t *fake_lfb;
static unsigned map_calls, locks, ends, passes, data_records;
static bool fail_map, fail_activate, fail_alloc;
static char last_end[240];

/* Scheduler and hardware ownership boundaries; all pixels run production
 * code. Transfer deliberately accepts exactly the states in registry.c. */
void kmutex_init(struct kmutex *m) { memset(m, 0, sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { (void)m; CHECK(locks == 0); locks++; }
void kmutex_unlock(struct kmutex *m) { (void)m; CHECK(locks == 1); locks--; }
unsigned registry_count(void) { return 1; }
const struct resource *registry_get(unsigned i) { return i == 0 ? &reservation : 0; }
int registry_claim_reserved(int h, gen_t gen, const char *owner)
{
    CHECK(h == 0);
    if (gen != reservation.generation ||
        (reservation.state != RS_FIRMWARE && reservation.state != RS_DISCOVERED))
        return -EINVAL;
    reservation.owner = owner;
    reservation.generation++;
    reservation.state = RS_CLAIMED;
    return 0;
}
int registry_activate(int h, gen_t gen)
{
    CHECK(h == 0 && gen == reservation.generation);
    if (fail_activate)
        return -EINVAL;
    CHECK(reservation.state == RS_CLAIMED);
    reservation.state = RS_ACTIVE;
    return 0;
}
int registry_quarantine(int h, gen_t gen)
{
    CHECK(h == 0 && gen == reservation.generation);
    reservation.state = RS_QUARANTINED;
    return 0;
}
bool registry_valid(int h, gen_t gen)
{
    return h == 0 && gen == reservation.generation && reservation.state != RS_QUARANTINED &&
           reservation.state != RS_RELEASED;
}
void *vmm_map_mmio(uint32_t phys, uint32_t size, bool uncached)
{
    CHECK(phys == g_boot.fb_phys && size == g_boot.fb_pitch * g_boot.fb_height && uncached);
    CHECK(reservation.state == RS_CLAIMED && !strcmp(reservation.owner, "fbdev"));
    map_calls++;
    return fail_map ? 0 : fake_lfb;
}
void *kmalloc(size_t n) { return fail_alloc ? 0 : malloc(n); }
void kfree(void *p) { free(p); }
uint32_t fnv1a32(const void *data, size_t size, uint32_t seed)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < size; i++)
        seed = (seed ^ p[i]) * 16777619u;
    return seed;
}
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    CHECK(!strcmp(probe, "framebuffer"));
    char extra[240] = "";
    va_list ap;
    va_start(ap, fmt);
    int n = fmt ? vsnprintf(extra, sizeof(extra), fmt, ap) : 0;
    va_end(ap);
    CHECK(n >= 0 && n < 155); /* leave room for the fixed protocol prefix */
    if (!strcmp(event, "END")) {
        ends++;
        passes += strstr(extra, "status=PASS") != 0;
        strcpy(last_end, extra);
    }
    if (!strcmp(event, "DATA"))
        data_records++;
}

#include "../../src/kernel/drivers/fbdev.c"
#include "../../src/kernel/drivers/fbdev_probe.c"

/* Obvious pixel reference: enumerate every source pixel, test destination
 * membership in 64-bit arithmetic, encode channels independently and write
 * each byte. No production clipping, packing or pointer helpers are reused. */
static void reference_pixel(const struct fb_device *d, uint8_t *p, uint32_t rgb)
{
    unsigned sizes[] = { d->red_size, d->green_size, d->blue_size };
    unsigned positions[] = { d->red_pos, d->green_pos, d->blue_pos };
    unsigned components[] = { (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255 };
    uint32_t encoded = 0;
    for (unsigned i = 0; i < 3; i++)
        encoded += (components[i] / (1u << (8 - sizes[i]))) * (1u << positions[i]);
    for (unsigned i = 0; i < d->bpp / 8; i++) {
        p[i] = encoded % 256;
        encoded /= 256;
    }
}

static void fb_reference_present(const struct fb_device *d, uint8_t *out,
                                 const struct fb_surface *s, const struct fb_rect *r)
{
    for (int32_t y = 0; y < r->height; y++) {
        for (int32_t x = 0; x < r->width; x++) {
            int64_t dx = (int64_t)r->x + x, dy = (int64_t)r->y + y;
            if (dx < 0 || dy < 0 || dx >= d->width || dy >= d->height)
                continue;
            const uint8_t *p = (const uint8_t *)s->pixels + (size_t)y * s->pitch + (size_t)x * 4;
            uint32_t rgb = (uint32_t)p[0] + (uint32_t)p[1] * 256 + (uint32_t)p[2] * 65536;
            reference_pixel(d, out + (size_t)dy * d->pitch + (size_t)dx * (d->bpp / 8), rgb);
        }
    }
}

static void reference_fill(const struct fb_device *d, uint8_t *out, const struct fb_rect *r, uint32_t colour)
{
    for (unsigned y = 0; y < d->height; y++)
        for (unsigned x = 0; x < d->width; x++)
            if ((int64_t)x >= r->x && (int64_t)y >= r->y &&
                (int64_t)x < (int64_t)r->x + r->width && (int64_t)y < (int64_t)r->y + r->height)
                reference_pixel(d, out + y * d->pitch + x * (d->bpp / 8), colour);
}

static uint32_t rng = 0xF105;
static uint32_t random_u32(void) { rng = rng * 1664525u + 1013904223u; return rng; }

static void render_tests(unsigned bpp, unsigned padding, unsigned format, unsigned source_padding)
{
    enum { W = 17, H = 13, SRC_W = 25, SRC_H = 21, G = 33 };
    unsigned source_pitch = SRC_W * 4 + source_padding;
    unsigned pitch = W * (bpp / 8) + padding, size = pitch * H;
    uint8_t *raw = malloc(size + 2 * G), *expected = malloc(size + 2 * G);
    uint8_t *source = malloc(source_pitch * SRC_H + 2 * G), *saved = malloc(source_pitch * SRC_H + 2 * G);
    CHECK(raw && expected && source && saved);
    if (!raw || !expected || !source || !saved)
        exit(2);
    memset(raw, 0xA5, size + 2 * G);
    memset(expected, 0xA5, size + 2 * G);
    memset(source, 0x5A, source_pitch * SRC_H + 2 * G);
    for (unsigned y = 0; y < SRC_H; y++)
        for (unsigned x = 0; x < SRC_W; x++) {
            uint32_t rgb = random_u32();
            memcpy(source + G + y * source_pitch + x * 4, &rgb, 4);
        }
    memcpy(saved, source, source_pitch * SRC_H + 2 * G);
    struct fb_surface s = { source + G, SRC_W, SRC_H, source_pitch, source_pitch * SRC_H };
    struct fb_device d = {
        .width = W, .height = H, .pitch = pitch, .size = size, .mapped = raw + G,
        .bpp = bpp, .present = true, .red_size = 8, .red_pos = 16,
        .green_size = 8, .green_pos = 8, .blue_size = 8, .blue_pos = 0,
        .rsvd_size = bpp == 32 ? 8 : 0, .rsvd_pos = 24,
    };
    if (format == 1) { d.red_pos = 0; d.blue_pos = 16; }
    if (format == 2) {
        d.red_size = 5; d.red_pos = 11; d.green_size = 6; d.green_pos = 5;
        d.blue_size = 5; d.rsvd_size = 0;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(rectangles) + 300; i++) {
        struct fb_rect r;
        if (i < ARRAY_SIZE(rectangles))
            r = rectangles[i];
        else
            r = (struct fb_rect){ (int32_t)(random_u32() % 61) - 30,
                                 (int32_t)(random_u32() % 51) - 25,
                                 (int32_t)(random_u32() % 26), (int32_t)(random_u32() % 22) };
        CHECK(fbdev_present_to(&d, &s, &r) == 0);
        fb_reference_present(&d, expected + G, &s, &r);
        CHECK(!memcmp(raw, expected, size + 2 * G));
        uint32_t colour = random_u32();
        CHECK(fbdev_fill_to(&d, &r, colour) == 0);
        reference_fill(&d, expected + G, &r, colour);
        CHECK(!memcmp(raw, expected, size + 2 * G));
        cases += 2;
    }
    const struct fb_rect rejected[] = {
        { INT32_MAX, 0, 1, 1 }, { 0, INT32_MAX, 1, 1 }, { INT32_MAX - 1, 0, 2, 1 },
        { 0, INT32_MAX - 1, 1, 2 }, { 0, 0, -1, 1 }, { 0, 0, 1, INT32_MIN },
        { 0, 0, SRC_W + 1, 1 }, { 0, 0, 1, SRC_H + 1 },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(rejected); i++) {
        CHECK(fbdev_present_to(&d, &s, &rejected[i]) == -EINVAL);
        if (i < 6)
            CHECK(fbdev_fill_to(&d, &rejected[i], 0) == -EINVAL);
        CHECK(!memcmp(raw, expected, size + 2 * G));
    }
    struct fb_rect one = { 0, 0, 1, 1 };
    CHECK(!fbdev_rect_valid(0));
    CHECK(fbdev_present_to(&d, &s, 0) == -EINVAL);
    CHECK(fbdev_fill_to(&d, 0, 0) == -EINVAL);
    CHECK(fbdev_present_to(&d, 0, &one) == -EINVAL);
    for (unsigned i = 0; i < 6; i++) {
        struct fb_surface bad = s;
        switch (i) {
        case 0: bad.pitch = SRC_W * 4 - 1; break;
        case 1: bad.pitch = UINT32_MAX; break;
        case 2: bad.size--; break;
        case 3: bad.pixels = 0; break;
        case 4: bad.width = UINT32_MAX; break;
        case 5: bad.pixels = (void *)(uintptr_t)(UINTPTR_MAX - 1); break;
        }
        CHECK(fbdev_present_to(&d, &bad, &one) == -EINVAL);
        CHECK(!memcmp(raw, expected, size + 2 * G));
    }
    for (unsigned i = 0; i < 9; i++) {
        struct fb_device bad = d;
        switch (i) {
        case 0: bad.pitch = W * (bpp / 8) - 1; break;
        case 1: bad.pitch = UINT32_MAX; break;
        case 2: bad.size--; break;
        case 3: bad.mapped = 0; break;
        case 4: bad.bpp = 16; break;
        case 5: bad.red_size = 9; break;
        case 6: bad.red_pos = 31; break;
        case 7: bad.green_pos = bad.red_pos; break;
        case 8: bad.mapped = (void *)(uintptr_t)(UINTPTR_MAX - 1); break;
        }
        CHECK(fbdev_present_to(&bad, &s, &one) == -EINVAL);
        CHECK(fbdev_fill_to(&bad, &one, 0) == -EINVAL);
        CHECK(!memcmp(raw, expected, size + 2 * G));
    }
    const struct fb_rect far[] = {
        { INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX },
        { INT32_MAX, INT32_MAX, 0, 0 }, { INT32_MIN, 0, 1, 1 },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(far); i++) {
        CHECK(fbdev_fill_to(&d, &far[i], 0) == 0);
        CHECK(!memcmp(raw, expected, size + 2 * G));
    }
    d.present = false;
    CHECK(fbdev_present_to(&d, 0, 0) == -ENODEV);
    CHECK(fbdev_fill_to(&d, 0, 0) == -ENODEV);
    CHECK(fbdev_present_to(0, &s, &one) == -ENODEV);
    CHECK(!memcmp(source, saved, source_pitch * SRC_H + 2 * G));
    for (unsigned i = 0; i < G; i++)
        CHECK(raw[i] == 0xA5 && raw[G + size + i] == 0xA5);
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = W * (bpp / 8); x < pitch; x++)
            CHECK(raw[G + y * pitch + x] == 0xA5);
    free(saved); free(source); free(expected); free(raw);
}

static void boot_fixture(bool vbe3)
{
    memset(&g_boot, 0, sizeof(g_boot));
    memset(&device, 0, sizeof(device));
    initialized = false;
    fail_map = fail_activate = fail_alloc = false;
    map_calls = ends = passes = data_records = 0;
    g_boot.flags = CBI_F_VBE_CTRL_VALID;
    g_boot.vbe_ctrl[5] = vbe3 ? 3 : 2;
    g_boot.fb_phys = 0xE0000000;
    g_boot.fb_width = 17; g_boot.fb_height = 13; g_boot.fb_bpp = 32;
    g_boot.fb_pitch = 17 * 4 + 7; g_boot.vbe_mode = 0x118;
    g_boot.fb_red_size = g_boot.fb_green_size = g_boot.fb_blue_size = g_boot.fb_rsvd_size = 8;
    g_boot.fb_red_pos = 16; g_boot.fb_green_pos = 8;
    unsigned p = vbe3 ? 50 : 16, f = vbe3 ? 54 : 31;
    uint8_t *m = g_boot.vbe_mode_info;
    m[0] = 0x91; m[27] = 6;
    m[18] = g_boot.fb_width; m[19] = g_boot.fb_width >> 8;
    m[20] = g_boot.fb_height; m[21] = g_boot.fb_height >> 8;
    m[25] = g_boot.fb_bpp;
    for (unsigned i = 0; i < 4; i++)
        m[40 + i] = (uint8_t)(g_boot.fb_phys >> (i * 8));
    m[p] = g_boot.fb_pitch; m[p + 1] = g_boot.fb_pitch >> 8;
    m[f] = m[f + 2] = m[f + 4] = m[f + 6] = 8;
    m[f + 1] = 16; m[f + 3] = 8; m[f + 7] = 24;
    /* Ordinary fields deliberately differ for VBE 3.0. */
    if (vbe3) { m[16] = 1; m[31] = 0; }
    reservation = (struct resource){ RES_MMIO, g_boot.fb_phys,
        g_boot.fb_phys + g_boot.fb_pitch * g_boot.fb_height,
        "boot-framebuffer", 7, RS_FIRMWARE, false };
    memset(fake_lfb, 0xA5, 13 * (17 * 4 + 7));
}

static void boot_tests(void)
{
    fake_lfb = malloc(13 * (17 * 4 + 7));
    CHECK(fake_lfb != 0);
    boot_fixture(true);
    reservation.state = RS_CLAIMED; /* current registry_init contract gap */
    CHECK(fbdev_init() == -EINVAL && !map_calls && !fbdev_get()->present);
    CHECK(reservation.state == RS_CLAIMED && reservation.generation == 7);
    boot_fixture(true);
    reservation.owner = "other-device";
    CHECK(fbdev_init() == -ENODEV && !map_calls);
    boot_fixture(true);
    fail_map = true;
    CHECK(fbdev_init() == -ENOMEM && reservation.state == RS_QUARANTINED);
    boot_fixture(true);
    fail_activate = true;
    CHECK(fbdev_init() == -EINVAL && reservation.state == RS_QUARANTINED);
    boot_fixture(true);
    g_boot.fb_red_pos = 8;
    CHECK(fbdev_init() == -EINVAL && !map_calls && reservation.generation == 7);
    boot_fixture(true);
    g_boot.fb_phys = UINT32_MAX - 10;
    CHECK(fbdev_init() == -EINVAL && !map_calls);
    for (unsigned v = 0; v < 2; v++) {
        boot_fixture(v != 0);
        CHECK(fbdev_init() == 0 && fbdev_get()->present && map_calls == 1);
        CHECK(reservation.state == RS_ACTIVE && reservation.generation == 8);
        CHECK(!strcmp(reservation.owner, "fbdev"));
        struct fb_rect one = { 0, 0, 1, 1 };
        uint32_t rgb = 0xAA123456;
        struct fb_surface s = { &rgb, 1, 1, 4, 4 };
        CHECK(fbdev_present(&s, &one) == 0 && !locks);
        CHECK(fake_lfb[0] == 0x56 && fake_lfb[1] == 0x34 && fake_lfb[2] == 0x12 && fake_lfb[3] == 0);
        g_boot.vbe_mode = 0x117;
        CHECK(fbdev_init() == 0 && fbdev_get()->mode_id == 0x118 && map_calls == 1);
        uint8_t saved[4]; memcpy(saved, fake_lfb, 4);
        reservation.generation++;
        CHECK(fbdev_fill(&one, 0) == -ENODEV && !memcmp(saved, fake_lfb, 4));
        CHECK(fbdev_present(&s, &one) == -ENODEV && !locks);
        reservation.generation--;
        reservation.state = RS_QUIESCING;
        CHECK(fbdev_fill(&one, 0) == -ENODEV && !memcmp(saved, fake_lfb, 4));
    }
    for (unsigned flag = 0; flag < 2; flag++) {
        boot_fixture(true);
        if (flag) g_boot.flags |= CBI_F_TEXT_MODE;
        else g_boot.fb_phys = 0;
        CHECK(fbdev_init() == 0 && !fbdev_get()->present && !map_calls);
        CHECK(fbdev_present(0, 0) == -ENODEV && fbdev_fill(0, 0) == -ENODEV);
    }
    /* Live VBE 3.0 24-bit conversion and reserved-position validation. */
    boot_fixture(true);
    g_boot.fb_bpp = 24; g_boot.fb_pitch = 17 * 3 + 7; g_boot.fb_rsvd_size = 0;
    g_boot.vbe_mode_info[25] = 24; g_boot.vbe_mode_info[50] = g_boot.fb_pitch;
    g_boot.vbe_mode_info[60] = 0;
    reservation.end = reservation.start + g_boot.fb_pitch * g_boot.fb_height;
    CHECK(fbdev_init() == 0 && fbdev_get()->bpp == 24);
    struct fb_rect one = { 0, 0, 1, 1 };
    uint32_t rgb = 0xEE123456;
    struct fb_surface s = { &rgb, 1, 1, 4, 4 };
    CHECK(fbdev_present(&s, &one) == 0);
    CHECK(fake_lfb[0] == 0x56 && fake_lfb[1] == 0x34 && fake_lfb[2] == 0x12 && fake_lfb[3] == 0xA5);
    boot_fixture(true);
    g_boot.vbe_mode_info[61] = 16; /* reserved mask overlaps red */
    CHECK(fbdev_init() == -EINVAL && !map_calls && reservation.generation == 7);
    boot_fixture(true);
    g_boot.vbe_mode_info[18]++;
    CHECK(fbdev_init() == -EINVAL && !map_calls);
    boot_fixture(true);
    CHECK(probe_framebuffer() == 0 && ends == 1 && passes == 1 && data_records == 7);
    /* Evidence refresh must leave the photographed one-pixel frame. */
    for (unsigned x = 0; x < 17; x++)
        CHECK(fake_lfb[x * 4] == 255 && fake_lfb[12 * (17 * 4 + 7) + x * 4] == 255);
    for (unsigned y = 0; y < 13; y++)
        for (unsigned x = 17 * 4; x < 17 * 4 + 7; x++)
            CHECK(fake_lfb[y * (17 * 4 + 7) + x] == 0xA5);
    boot_fixture(true);
    g_boot.flags |= CBI_F_TEXT_MODE;
    CHECK(probe_framebuffer() == 0 && ends == 1 && passes == 1 && !map_calls);
    boot_fixture(true);
    reservation.state = RS_CLAIMED;
    CHECK(probe_framebuffer() != 0 && ends == 1 && !passes && !map_calls);
    boot_fixture(true);
    fail_alloc = true;
    CHECK(probe_framebuffer() != 0 && ends == 1 && !passes && !map_calls);
    free(fake_lfb);
}

int main(void)
{
    for (unsigned bpp = 24; bpp <= 32; bpp += 8)
        for (unsigned pad = 0; pad <= 7; pad += 7)
            for (unsigned format = 0; format < 3; format++)
                for (unsigned source_pad = 0; source_pad <= 7; source_pad += 7)
                    render_tests(bpp, pad, format, source_pad);
    boot_tests();
    printf("framebuffer host tests: %s (%u reference comparisons, %u failures)\n",
           failures ? "FAIL" : "PASS", cases, failures);
    return failures ? 1 : 0;
}
