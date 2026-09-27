#include "vga_presenter.h"

static void zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

static int same_rect(const cvp_rect *a, const cvp_rect *b)
{
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

static int same_format(const cvp_format *a, const cvp_format *b)
{
    return a->pitch == b->pitch && a->width == b->width && a->height == b->height &&
           a->bytes == b->bytes && a->red_size == b->red_size && a->red_pos == b->red_pos &&
           a->green_size == b->green_size && a->green_pos == b->green_pos &&
           a->blue_size == b->blue_size && a->blue_pos == b->blue_pos;
}

static int valid_channel(unsigned size, unsigned pos, unsigned bytes)
{
    return size >= 1 && size <= 8 && pos + size <= bytes * 8u;
}

static uint32_t pack(const cvp_format *f, unsigned r6, unsigned g6, unsigned b6)
{
    unsigned r = (r6 << 2) | (r6 >> 4), g = (g6 << 2) | (g6 >> 4), b = (b6 << 2) | (b6 >> 4);
    return ((uint32_t)(r >> (8 - f->red_size)) << f->red_pos) |
           ((uint32_t)(g >> (8 - f->green_size)) << f->green_pos) |
           ((uint32_t)(b >> (8 - f->blue_size)) << f->blue_pos);
}

void cvp_init(cvp_state *s)
{
    zero(s, sizeof(*s));
}

static void set_pending(cvp_state *s, unsigned row)
{
    s->pending[row >> 3] |= (uint8_t)(1u << (row & 7));
}

static int is_pending(const cvp_state *s, unsigned row)
{
    return (s->pending[row >> 3] >> (row & 7)) & 1;
}

static void clear_pending(cvp_state *s, unsigned row)
{
    s->pending[row >> 3] &= (uint8_t)~(1u << (row & 7));
}

/* Framebuffer mappings are uncached: every store is a bus transaction, so
 * store aligned dwords. Loads come from the cached native row buffer. */
static void copy_to_surface(uint8_t *dst, const uint8_t *src, unsigned n)
{
    volatile uint32_t *words;
    while (n && ((uintptr_t)dst & 3)) {
        *(volatile uint8_t *)dst++ = *src++;
        --n;
    }
    words = (volatile uint32_t *)dst;
    while (n >= 4) {
        *words++ = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                   ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        src += 4;
        n -= 4;
    }
    dst = (uint8_t *)words;
    while (n--) *(volatile uint8_t *)dst++ = *src++;
}

static uint32_t blit_row(const cvp_state *s, const cvp_format *f, const cvp_request *q,
                         uint8_t *target, unsigned y)
{
    int ty = q->window.top + (int)y, left, right;
    unsigned i, count = q->clip_count ? q->clip_count : 1, n;
    uint32_t written = 0;
    const uint8_t *src;
    uint8_t *dst;
    if (ty < 0 || ty >= (int)f->height) return 0;
    for (i = 0; i < count; ++i) {
        const cvp_rect *c = q->clip_count ? &q->clip[i] : &q->window;
        if (ty < c->top || ty >= c->bottom) continue;
        left = q->window.left;
        if (c->left > left) left = c->left;
        if (left < 0) left = 0;
        right = q->window.right;
        if (c->right < right) right = c->right;
        if (right > (int)f->width) right = (int)f->width;
        if (right <= left) continue;
        src = s->native + (uint32_t)(left - q->window.left) * f->bytes;
        dst = target + (uint32_t)ty * f->pitch + (uint32_t)left * f->bytes;
        n = (unsigned)(right - left) * f->bytes;
        copy_to_surface(dst, src, n);
        written += (uint32_t)(right - left);
    }
    return written;
}

static void convert_row(cvp_state *s, const cvp_format *f, int blank)
{
    unsigned x;
    uint8_t *d = s->native;
    uint32_t c;
    for (x = 0; x < s->width; ++x) {
        c = blank ? 0 : s->lut[s->line8[s->x_map[x]]];
        d[0] = (uint8_t)c;
        d[1] = (uint8_t)(c >> 8);
        if (f->bytes >= 3) d[2] = (uint8_t)(c >> 16);
        if (f->bytes == 4) d[3] = (uint8_t)(c >> 24);
        d += f->bytes;
    }
}

int cvp_present(cvp_state *s, cvga_state *v, const cvp_format *f, const cvp_request *q,
                uint8_t *target, cvp_stats *st)
{
    cvga_geometry g;
    unsigned w, h, x, y, row, i, j, full, last = 0xffffu, phases, rows_drawn = 0, visited;
    uint32_t written = 0;
    int geometry_changed, window_changed;
    zero(st, sizeof(*st));
    if ((f->bytes < 2 || f->bytes > 4) || !f->width || !f->height ||
        f->pitch < (uint32_t)f->width * f->bytes ||
        !valid_channel(f->red_size, f->red_pos, f->bytes) ||
        !valid_channel(f->green_size, f->green_pos, f->bytes) ||
        !valid_channel(f->blue_size, f->blue_pos, f->bytes))
        return st->status = CVP_BAD_FORMAT;
    if (q->window.right <= q->window.left || q->window.bottom <= q->window.top ||
        q->clip_count > CVP_MAX_CLIPS)
        return st->status = CVP_BAD_RECT;
    w = (unsigned)(q->window.right - q->window.left);
    h = (unsigned)(q->window.bottom - q->window.top);
    if (w > CVP_MAX_WIDTH || h > CVP_MAX_HEIGHT) return st->status = CVP_BAD_RECT;
    if (!cvga_get_geometry(v, &g) || g.height > CVP_MAX_ROWS || g.width > CVP_MAX_WIDTH)
        return st->status = CVP_UNSUPPORTED;
    phases = (q->flags & CVP_BLINK_VISIBLE ? CVGA_BLINK_VISIBLE : 0) |
             (q->flags & CVP_CURSOR_VISIBLE ? CVGA_CURSOR_VISIBLE : 0);
    geometry_changed = !s->initialised || g.width != s->geometry.width ||
                       g.height != s->geometry.height || g.scan_repeat != s->geometry.scan_repeat ||
                       g.text != s->geometry.text || g.blank != s->geometry.blank;
    window_changed = !s->initialised || !same_rect(&q->window, &s->window) ||
                     !same_format(f, &s->format);
    if (!s->initialised || v->display_changes != s->lut_changes || window_changed) {
        for (i = 0; i < 256; ++i) s->lut[i] = pack(f, v->dac[i][0], v->dac[i][1], v->dac[i][2]);
        s->lut_changes = v->display_changes;
    }
    if (geometry_changed || window_changed) {
        s->width = (uint16_t)w;
        s->height = (uint16_t)h;
        s->src_width = (uint16_t)g.width;
        s->src_height = (uint16_t)(g.height * g.scan_repeat);
        s->repeat = (uint16_t)g.scan_repeat;
        for (x = 0; x < w; ++x) s->x_map[x] = (uint16_t)((uint32_t)x * g.width / w);
        for (y = 0; y < h; ++y)
            s->y_map[y] = (uint16_t)(((uint32_t)y * s->src_height / h) / g.scan_repeat);
        s->geometry = g;
        s->window = q->window;
        s->format = *f;
        s->cursor = 0;
    }
    st->source_width = s->src_width;
    st->source_height = s->src_height;
    if (q->flags & CVP_NO_DAMAGE) {
        /* Compositor-driven exposure: render the clipped window now without
         * consuming damage, so the next damage-limited frame stays exact. */
        for (y = 0; y < h; ++y) {
            int ty = q->window.top + (int)y;
            if (ty < 0 || ty >= (int)f->height) continue;      /* outside the band */
            row = s->y_map[y];
            if (row != last) {
                cvga_render_row8(v, row, s->line8, CVP_MAX_WIDTH, phases);
                last = row;
            }
            convert_row(s, f, (int)g.blank);
            written += blit_row(s, f, q, target, y);
            ++rows_drawn;
        }
        st->complete = 1;
        st->rows_drawn = rows_drawn;
        st->pixels_written = written;
        st->frames_completed = s->frames_completed;
        st->full_redraws = s->full_redraws;
        return st->status = CVP_OK;
    }
    /* Newly exposed parts of the window need pixels even without damage. */
    if (q->clip_count != s->clip_count) window_changed = 1;
    for (i = 0; i < q->clip_count && !window_changed; ++i)
        window_changed = !same_rect(&q->clip[i], &s->clip[i]);
    if (window_changed) {
        s->clip_count = q->clip_count;
        for (i = 0; i < q->clip_count; ++i) s->clip[i] = q->clip[i];
    }
    full = !s->initialised || (q->flags & CVP_FORCE_FULL) || geometry_changed || window_changed ||
           v->display_changes != s->display_changes;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < CVGA_DIRTY_BYTES; ++j) {
            s->taken[i][j] = v->dirty[i][j];
            v->dirty[i][j] = 0;
        }
    if (!full && g.text)
        for (j = 0; j < CVGA_DIRTY_BYTES && !full; ++j) full = s->taken[2][j] != 0;
    if (full) {
        for (row = 0; row < g.height; ++row) set_pending(s, row);
        ++s->full_redraws;
    } else {
        int phase_change = g.text && ((q->flags ^ s->flags_seen) & (CVP_BLINK_VISIBLE | CVP_CURSOR_VISIBLE));
        for (row = 0; row < g.height; ++row)
            if (!is_pending(s, row) &&
                (cvga_row_reads_dirty(v, row, (const uint8_t (*)[CVGA_DIRTY_BYTES])s->taken) ||
                 (phase_change && cvga_row_phase_sensitive(v, row))))
                set_pending(s, row);
    }
    s->display_changes = v->display_changes;
    s->flags_seen = q->flags;
    s->initialised = 1;
    st->complete = 1;
    for (visited = 0, y = s->cursor; visited < h; ++visited, y = y + 1 == h ? 0 : y + 1) {
        if (visited && !y && s->drawn_since_wrap) {
            ++s->passes;                      /* the sweep wrapped after drawing */
            s->drawn_since_wrap = 0;
        }
        row = s->y_map[y];
        if (!is_pending(s, row)) continue;
        if (q->budget_pixels && written && written + w > q->budget_pixels) {
            /* Resume at this source row's first destination line: lines of
             * a scaled row drawn before a later change must not stay stale. */
            while (y && s->y_map[y - 1] == row) --y;
            s->cursor = (uint16_t)y;
            st->complete = 0;
            break;
        }
        if (row != last) {
            cvga_render_row8(v, row, s->line8, CVP_MAX_WIDTH, phases);
            last = row;
        }
        convert_row(s, f, (int)g.blank);
        written += blit_row(s, f, q, target, y);
        ++rows_drawn;
        s->drawn_since_wrap = 1;
        if (y + 1 == h || s->y_map[y + 1] != row) clear_pending(s, row);
    }
    if (st->complete && s->drawn_since_wrap) {
        ++s->passes;                          /* completed frames end a pass */
        s->drawn_since_wrap = 0;
    }
    if (st->complete) {
        s->cursor = 0;
        if (rows_drawn) ++s->frames_completed;
    }
    st->passes = s->passes;
    st->rows_drawn = rows_drawn;
    st->pixels_written = written;
    st->frames_completed = s->frames_completed;
    st->full_redraws = s->full_redraws;
    return st->status = CVP_OK;
}
