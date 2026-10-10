/* F1 boot LFB: no firmware entry points and no runtime mode changes.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/registry.h>
#include <ciuki/fbdev.h>

static struct fb_device device;
static struct kmutex presenter;
static bool initialized;
static unsigned console_rows;
static unsigned drawing;
enum { CONSOLE_BUSY = 1, PRESENT_BUSY = 2 };

void fbdev_console_region(unsigned rows)
{
    __atomic_store_n(&console_rows, rows, __ATOMIC_RELEASE);
}

bool fbdev_console_begin(void)
{
    unsigned idle = 0;
    return __atomic_compare_exchange_n(&drawing, &idle, CONSOLE_BUSY, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void fbdev_console_end(void)
{
    __atomic_store_n(&drawing, 0, __ATOMIC_RELEASE);
}

static bool present_begin(const struct fb_device *d, const struct fb_rect *c)
{
    if (d != &device || (unsigned)c->y >= __atomic_load_n(&console_rows, __ATOMIC_ACQUIRE))
        return true;
    unsigned idle = 0;
    return __atomic_compare_exchange_n(&drawing, &idle, PRESENT_BUSY, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void present_end(const struct fb_device *d, const struct fb_rect *c)
{
    if (d == &device && (unsigned)c->y < __atomic_load_n(&console_rows, __ATOMIC_ACQUIRE))
        __atomic_store_n(&drawing, 0, __ATOMIC_RELEASE);
}

/* VESA VBE Core Functions 3.0 (1998-09-16), pp. 14, 30-31, 37-39:
 * https://www.cs.utexas.edu/~dahlin/Classes/UGOS/reading/vbe3.pdf
 * 24/32 bpp occupy 3/4 bytes. Pitch is a logical scanline byte count,
 * not width*bytes. VBE 3.0 LFB pitch/masks use the Lin* fields, whereas
 * VBE 2.x uses the ordinary fields. Consume the loader's normalized pitch
 * and RGB masks; the full copy supplies the omitted reserved position.
 * Reserved bits are written zero; X in the source is never a colour. */
static bool layout_valid(const struct fb_device *d)
{
    if (!d->width || !d->height || d->width > INT32_MAX ||
        d->height > INT32_MAX || (d->bpp != 24 && d->bpp != 32))
        return false;
    if ((uint64_t)d->width * (d->bpp / 8) > d->pitch ||
        (uint64_t)d->pitch * d->height > d->size)
        return false;
    uint32_t used = 0;
    const uint8_t fields[4][2] = {
        { d->red_size, d->red_pos }, { d->green_size, d->green_pos },
        { d->blue_size, d->blue_pos }, { d->rsvd_size, d->rsvd_pos },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(fields); i++) {
        uint32_t n = fields[i][0], p = fields[i][1];
        if ((!n && i < 3) || n > 8 || (n && p + n > d->bpp))
            return false;
        uint32_t mask = n ? ((1u << n) - 1u) << p : 0;
        if (used & mask)
            return false;
        used |= mask;
    }
    return true;
}

static bool geometry_valid(const struct fb_device *d)
{
    return layout_valid(d) && d->mapped && (uintptr_t)d->mapped <= UINTPTR_MAX - d->size;
}

int fbdev_init(void)
{
    if (initialized)
        return 0;
    if ((g_boot.flags & CBI_F_TEXT_MODE) || !g_boot.fb_phys) {
        device.handle = -1;
        initialized = true;
        kmutex_init(&presenter);
        return 0;
    }
    uint64_t size = (uint64_t)g_boot.fb_pitch * g_boot.fb_height;
    if (!size || size > UINT32_MAX || size > UINT32_MAX - g_boot.fb_phys)
        return -EINVAL;
    struct fb_device d = {
        .width = g_boot.fb_width, .height = g_boot.fb_height,
        .pitch = g_boot.fb_pitch, .size = (uint32_t)size, .phys = g_boot.fb_phys,
        .bpp = g_boot.fb_bpp, .red_size = g_boot.fb_red_size, .red_pos = g_boot.fb_red_pos,
        .green_size = g_boot.fb_green_size, .green_pos = g_boot.fb_green_pos,
        .blue_size = g_boot.fb_blue_size, .blue_pos = g_boot.fb_blue_pos,
        .rsvd_size = g_boot.fb_rsvd_size, .mode_id = g_boot.vbe_mode, .handle = -1,
    };
    const uint8_t *m = g_boot.vbe_mode_info;
    bool linear = (g_boot.flags & CBI_F_VBE_CTRL_VALID) &&
                  (g_boot.vbe_ctrl[4] | (uint32_t)g_boot.vbe_ctrl[5] << 8) >= 0x300;
    unsigned fields = linear ? 54 : 31;
    d.rsvd_pos = m[fields + 7];
    /* Reject disagreement with the copied mode description before claiming.
     * Attribute bits: supported, graphics, LFB; memory model 6 = direct. */
    unsigned pitch_off = linear ? 50 : 16;
    uint32_t copied_pitch = m[pitch_off] | (uint32_t)m[pitch_off + 1] << 8;
    uint32_t copied_width = m[18] | (uint32_t)m[19] << 8;
    uint32_t copied_height = m[20] | (uint32_t)m[21] << 8;
    uint32_t copied_phys = m[40] | (uint32_t)m[41] << 8 | (uint32_t)m[42] << 16 | (uint32_t)m[43] << 24;
    if ((m[0] & 0x91) != 0x91 || m[27] != 6 || copied_pitch != d.pitch ||
        copied_width != d.width || copied_height != d.height || m[25] != d.bpp || copied_phys != d.phys ||
        m[fields] != d.red_size || m[fields + 1] != d.red_pos ||
        m[fields + 2] != d.green_size || m[fields + 3] != d.green_pos ||
        m[fields + 4] != d.blue_size || m[fields + 5] != d.blue_pos ||
        m[fields + 6] != d.rsvd_size)
        return -EINVAL;
    if (!layout_valid(&d))
        return -EINVAL;
    for (unsigned i = 0; i < registry_count(); i++) {
        const struct resource *r = registry_get(i);
        if (r && r->type == RES_MMIO && r->start == d.phys && r->end == d.phys + d.size &&
            r->owner && !strncmp(r->owner, "boot-framebuffer", sizeof("boot-framebuffer"))) {
            d.handle = (int)i;
            int rc = registry_claim_reserved(d.handle, r->generation, "fbdev");
            if (rc)
                return rc;
            d.generation = registry_get(i)->generation;
            break;
        }
    }
    /* A missing reservation is an ownership error, not an absent display.
     * registry_init currently leaves this reservation CLAIMED: the lead
     * must publish it as FIRMWARE/DISCOVERED for the transfer API above. */
    if (d.handle < 0)
        return -ENODEV;
    d.mapped = vmm_map_mmio(d.phys, d.size, true);
    if (!d.mapped) {
        registry_quarantine(d.handle, d.generation);
        return -ENOMEM;
    }
    int rc = registry_activate(d.handle, d.generation);
    if (rc) {
        registry_quarantine(d.handle, d.generation);
        return rc;
    }
    d.present = true;
    kmutex_init(&presenter);
    device = d;
    initialized = true;
    return 0;
}

const struct fb_device *fbdev_get(void) { return &device; }

bool fbdev_rect_valid(const struct fb_rect *r)
{
    return r && r->width >= 0 && r->height >= 0 &&
           r->x <= INT32_MAX - r->width && r->y <= INT32_MAX - r->height;
}

static bool clip(const struct fb_device *d, const struct fb_rect *r, struct fb_rect *c)
{
    int32_t x1 = r->x + r->width, y1 = r->y + r->height;
    c->x = r->x > 0 ? r->x : 0;
    c->y = r->y > 0 ? r->y : 0;
    if (x1 > (int32_t)d->width)
        x1 = (int32_t)d->width;
    if (y1 > (int32_t)d->height)
        y1 = (int32_t)d->height;
    if (!r->width || !r->height || x1 <= c->x || y1 <= c->y)
        return false;
    c->width = x1 - c->x;
    c->height = y1 - c->y;
    return true;
}

static uint32_t pack(const struct fb_device *d, uint32_t rgb)
{
    return ((((rgb >> 16) & 255u) >> (8 - d->red_size)) << d->red_pos) |
           ((((rgb >> 8) & 255u) >> (8 - d->green_size)) << d->green_pos) |
           (((rgb & 255u) >> (8 - d->blue_size)) << d->blue_pos);
}

static void pixel(uint8_t *p, unsigned bytes, uint32_t colour)
{
    /* i386 supports unaligned integer stores. Lower the C alignment too,
     * so arbitrary pitch/base does not introduce alignment or aliasing UB. */
    typedef uint32_t fb_word __attribute__((aligned(1), may_alias));
    if (bytes == 4)
        *(volatile fb_word *)p = colour;
    else {
        p[0] = (uint8_t)colour;
        p[1] = (uint8_t)(colour >> 8);
        p[2] = (uint8_t)(colour >> 16);
    }
}

int fbdev_present_to(const struct fb_device *d, const struct fb_surface *s, const struct fb_rect *r)
{
    if (!d || !d->present)
        return -ENODEV;
    if (!geometry_valid(d) || !fbdev_rect_valid(r) || !s || s->width > INT32_MAX ||
        s->height > INT32_MAX || (uint32_t)r->width > s->width || (uint32_t)r->height > s->height ||
        (uint64_t)s->width * 4 > s->pitch || (uint64_t)s->pitch * s->height > s->size ||
        (s->size && (!s->pixels || (uintptr_t)s->pixels > UINTPTR_MAX - s->size)))
        return -EINVAL;
    struct fb_rect c;
    if (!clip(d, r, &c))
        return 0;
    if (!present_begin(d, &c))
        return -EBUSY;
    uint32_t sx = (uint32_t)((int64_t)c.x - r->x), sy = (uint32_t)((int64_t)c.y - r->y);
    unsigned bytes = d->bpp / 8;
    for (int32_t y = 0; y < c.height; y++) {
        const uint8_t *sp = (const uint8_t *)s->pixels + (sy + (uint32_t)y) * s->pitch + sx * 4;
        uint8_t *dp = d->mapped + (c.y + (uint32_t)y) * d->pitch + (uint32_t)c.x * bytes;
        for (int32_t x = 0; x < c.width; x++) {
            uint32_t rgb;
            memcpy(&rgb, sp + (uint32_t)x * 4, 4);
            pixel(dp + (uint32_t)x * bytes, bytes, pack(d, rgb));
        }
    }
    present_end(d, &c);
    return 0;
}

int fbdev_fill_to(const struct fb_device *d, const struct fb_rect *r, uint32_t colour)
{
    if (!d || !d->present)
        return -ENODEV;
    if (!geometry_valid(d) || !fbdev_rect_valid(r))
        return -EINVAL;
    struct fb_rect c;
    if (!clip(d, r, &c))
        return 0;
    if (!present_begin(d, &c))
        return -EBUSY;
    unsigned bytes = d->bpp / 8;
    colour = pack(d, colour);
    for (int32_t y = 0; y < c.height; y++) {
        uint8_t *dp = d->mapped + (c.y + (uint32_t)y) * d->pitch + (uint32_t)c.x * bytes;
        for (int32_t x = 0; x < c.width; x++)
            pixel(dp + (uint32_t)x * bytes, bytes, colour);
    }
    present_end(d, &c);
    return 0;
}

static bool live(void)
{
    if (!initialized || !device.present || !registry_valid(device.handle, device.generation))
        return false;
    const struct resource *r = registry_get((unsigned)device.handle);
    return r && r->state == RS_ACTIVE;
}

int fbdev_present(const struct fb_surface *src, const struct fb_rect *dst)
{
    if (!initialized || !device.present)
        return -ENODEV;
    kmutex_lock(&presenter);
    int rc = live() ? fbdev_present_to(&device, src, dst) : -ENODEV;
    kmutex_unlock(&presenter);
    return rc;
}

int fbdev_fill(const struct fb_rect *rect, uint32_t colour)
{
    if (!initialized || !device.present)
        return -ENODEV;
    kmutex_lock(&presenter);
    int rc = live() ? fbdev_fill_to(&device, rect, colour) : -ENODEV;
    kmutex_unlock(&presenter);
    return rc;
}
