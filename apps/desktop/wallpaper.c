/* SPDX-License-Identifier: MIT
 * Wallpaper engine (wallpaper.h). Original procedural designs, integer-only,
 * deterministic for a given resolution; no third-party artwork. */
#include "wallpaper.h"
#include "settings.h"
#include "theme.h"
#include <stdlib.h>
#include <string.h>


const struct desk_builtin_wallpaper desk_builtin_wallpapers[DESK_BUILTIN_WALLPAPERS] = {
    {"dawn", "Platinum Dawn"}, {"pinstripe", "Platinum Pinstripe"}, {"deepblue", "Ciuki Blue"},
    {"twilight", "Twilight"}, {"linen", "Linen"}, {"blueprint", "Blueprint"}};

static const int8_t bayer[4][4] = {{-8, 0, -6, 2}, {4, -4, 6, -2}, {-5, 3, -7, 1}, {7, -1, 5, -3}};

static uint32_t channel_clamp(int v) { return v < 0 ? 0u : v > 255 ? 255u : (uint32_t)v; }

/* c0..c1 at t/4096, plus an ordered-dither offset in 1/16 levels. */
static uint32_t lerp(uint32_t c0, uint32_t c1, int t, int dither)
{
    if (t < 0) t = 0;
    if (t > 4096) t = 4096;
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        int a = (int)((c0 >> shift) & 255), b = (int)((c1 >> shift) & 255);
        int v = (a * 16 * (4096 - t) + b * 16 * t) / 4096 + dither;   /* 1/16 level units */
        out |= channel_clamp((v + 8) / 16) << shift;
    }
    return out;
}

static uint32_t shade(uint32_t c, int delta)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) out |= channel_clamp((int)((c >> shift) & 255) + delta) << shift;
    return out;
}

static uint32_t hash(uint32_t x, uint32_t y)
{
    uint32_t h = x * 0x9e3779b1u ^ y * 0x85ebca77u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

void desk_builtin_paint(const char *id, uint32_t *out, uint32_t width, uint32_t height, uint32_t stride)
{
    int w = (int)width, h = (int)height;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        uint32_t *row = out + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            int d = bayer[y & 3][x & 3];
            uint32_t c;
            if (!strcmp(id, "pinstripe")) {
                /* Platinum pinstripes with a soft vertical light falloff. */
                int t = y * 4096 / (h > 1 ? h - 1 : 1);
                c = lerp(0xe4e8ecu, 0xc9d0d6u, t, d);
                if (y & 1) c = shade(c, -7);
            } else if (!strcmp(id, "deepblue")) {
                int dx = x - w / 2, dy = y - h * 2 / 5;
                int64_t r = (int64_t)dx * dx + (int64_t)dy * dy * 2;
                int64_t m = (int64_t)w * w / 4 + (int64_t)h * h / 2;
                c = lerp(0x4a7898u, 0x152836u, (int)(r * 4096 / (m ? m : 1)), d);
            } else if (!strcmp(id, "twilight")) {
                int t = (x * 3 + y * 5) * 4096 / (w * 3 + h * 5);
                c = t < 2048 ? lerp(0x274a5eu, 0x4f5f80u, t * 2, d) : lerp(0x4f5f80u, 0x7a6585u, (t - 2048) * 2, d);
            } else if (!strcmp(id, "linen")) {
                int n = (int)(hash((uint32_t)x, (uint32_t)y) & 15) - 8;
                int weave = ((x & 3) == 0 ? -5 : 0) + ((y & 3) == 0 ? -4 : 0);
                c = shade(0xd9d3c6u, n / 2 + weave);
            } else if (!strcmp(id, "blueprint")) {
                int t = y * 4096 / (h > 1 ? h - 1 : 1);
                c = lerp(0x2c5570u, 0x1f3f56u, t, d);
                if (x % 48 == 0 || y % 48 == 0) c = shade(c, 22);
                else if (x % 12 == 0 && y % 12 == 0) c = shade(c, 16);
            } else {   /* "dawn" and unknown ids */
                int t = y * 4096 / (h > 1 ? h - 1 : 1);
                c = t < 2600 ? lerp(0xd2dbe2u, 0x9fb2c0u, t * 4096 / 2600, d) : lerp(0x9fb2c0u, 0x6f879au, (t - 2600) * 4096 / 1496, d);
                if (((x + y) % 7) == 0) c = shade(c, 3);
            }
            row[x] = c;
        }
    }
}

uint32_t desk_wallpaper_over(uint32_t argb, uint32_t back)
{
    unsigned a = argb >> 24;
    if (a == 255) return argb & 0xffffffu;
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
        out |= ((((argb >> shift) & 255) * a + ((back >> shift) & 255) * (255 - a) + 127) / 255) << shift;
    return out;
}

/* Area average (downscale) or bilinear (upscale) of src scaled to tw*th;
 * only the w*h window whose top-left corner is cx,cy in that scaled image
 * is written to dst (the whole image for stretch and fit, the centred crop
 * for fill), so every mode samples a pixel the same way. */
struct sample_axis { uint32_t first, second, weight; };
static struct sample_axis sample_axis(uint64_t position, uint32_t source, uint32_t target, int up)
{
    struct sample_axis a={0};
    if (up) {
        int64_t fixed=((int64_t)(2*position+1)*source*128)/target-128;
        if (fixed<0) fixed=0;
        a.first=(uint32_t)(fixed>>8); a.weight=(uint32_t)(fixed&255);
        a.second=a.first+1<source ? a.first+1 : a.first;
        if (a.first>=source) a.first=a.second=source-1;
    } else {
        a.first=(uint32_t)(position*source/target);
        a.second=(uint32_t)((position+1)*source/target);
        if (a.second<=a.first) a.second=a.first+1;
        if (a.second>source) a.second=source;
    }
    return a;
}
static void resample(const struct desk_image *s, uint32_t *dst, uint32_t tw, uint32_t th, uint32_t cx, uint32_t cy,
                     uint32_t w, uint32_t h, uint32_t stride, uint32_t colour, int layer)
{
    if (!tw || !th || !s->width || !s->height) return;
    int up=tw>=s->width && th>=s->height;
    /* Compute wide divisions once per axis, in this preemptible user-space
     * worker. Allocation failure retains the same pixel calculation. */
    struct sample_axis *columns=malloc((size_t)w*sizeof(*columns));
    if (columns) for (uint32_t x=0;x<w;x++) columns[x]=sample_axis((uint64_t)x+cx,s->width,tw,up);
    for (uint32_t y=0;y<h;y++) {
        struct sample_axis ay=sample_axis((uint64_t)y+cy,s->height,th,up);
        for (uint32_t x=0;x<w;x++) {
            struct sample_axis ax=columns ? columns[x] : sample_axis((uint64_t)x+cx,s->width,tw,up);
            uint32_t px=0;
            if (up) {
                uint32_t p00=s->pixels[(size_t)ay.first*s->width+ax.first];
                uint32_t p10=s->pixels[(size_t)ay.first*s->width+ax.second];
                uint32_t p01=s->pixels[(size_t)ay.second*s->width+ax.first];
                uint32_t p11=s->pixels[(size_t)ay.second*s->width+ax.second];
                for (unsigned shift=0;shift<32;shift+=8) {
                    uint32_t a=(p00>>shift)&255,b=(p10>>shift)&255,c=(p01>>shift)&255,e=(p11>>shift)&255;
                    uint32_t top=a*(256-ax.weight)+b*ax.weight,bot=c*(256-ax.weight)+e*ax.weight;
                    px|=((top*(256-ay.weight)+bot*ay.weight+32768)>>16)<<shift;
                }
            } else {
                /* Decoder bounds: <=4096*4096 samples, so even 255*n+n/2
                 * fits uint32_t. Avoid four software 64-bit divides/pixel
                 * on the Pentium III while preserving rounded area averages. */
                uint32_t sum[4]={0},n=0;
                for (uint32_t yy=ay.first;yy<ay.second;yy++)
                    for (uint32_t xx=ax.first;xx<ax.second;xx++) {
                        uint32_t p=s->pixels[(size_t)yy*s->width+xx];
                        for (unsigned k=0;k<4;k++) sum[k]+=(p>>(k*8))&255;
                        n++;
                    }
                if (n) for (unsigned k=0;k<4;k++) px|=((sum[k]+n/2)/n)<<(k*8);
            }
            dst[(size_t)y*stride+x]=layer ? px : desk_wallpaper_over(px,colour)|(px&0xff000000u);
        }
    }
    free(columns);
}

static void image_scale(const struct desk_image *src, uint32_t *dst, uint32_t w, uint32_t h, int mode, uint32_t colour, int layer)
{
    for (size_t i = 0; i < (size_t)w * h; i++) dst[i] = colour;
    if (!src || !src->pixels || !src->width || !src->height || !w || !h) return;
    if (mode == CFG_WALL_TILE) {
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++)
            dst[(size_t)y * w + x] = layer ? src->pixels[(size_t)(y % src->height) * src->width + x % src->width] :
                desk_wallpaper_over(src->pixels[(size_t)(y % src->height) * src->width + x % src->width], colour);
        return;
    }
    if (mode == CFG_WALL_CENTER) {
        int32_t ox = ((int32_t)w - (int32_t)src->width) / 2, oy = ((int32_t)h - (int32_t)src->height) / 2;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            int32_t sx = (int32_t)x - ox, sy = (int32_t)y - oy;
            if (sx >= 0 && sy >= 0 && sx < (int32_t)src->width && sy < (int32_t)src->height)
                dst[(size_t)y * w + x] = layer ? src->pixels[(size_t)sy * src->width + (uint32_t)sx] :
                    desk_wallpaper_over(src->pixels[(size_t)sy * src->width + (uint32_t)sx], colour);
        }
        return;
    }
    if (mode == CFG_WALL_FILL) {
        /* Uniform scale max(w/width, h/height): the image covers the whole
         * box and the overflow on one axis is cropped equally on both sides. */
        uint32_t tw = w, th = h;
        if ((uint64_t)w * src->height >= (uint64_t)h * src->width) th = (uint32_t)((uint64_t)src->height * w / src->width);
        else tw = (uint32_t)((uint64_t)src->width * h / src->height);
        if (th < h) th = h;
        if (tw < w) tw = w;
        resample(src, dst, tw, th, (tw - w) / 2, (th - h) / 2, w, h, w, colour, layer);
        if (!layer) for (size_t i = 0; i < (size_t)w * h; i++) dst[i] &= 0xffffffu;
        return;
    }
    uint32_t tw = w, th = h, ox = 0, oy = 0;
    if (mode == CFG_WALL_FIT) {
        if ((uint64_t)src->width * h > (uint64_t)src->height * w) th = (uint32_t)((uint64_t)src->height * w / src->width);
        else tw = (uint32_t)((uint64_t)src->width * h / src->height);
        if (!tw) tw = 1;
        if (!th) th = 1;
        ox = (w - tw) / 2; oy = (h - th) / 2;
    }
    resample(src, dst + (size_t)oy * w + ox, tw, th, 0, 0, tw, th, w, colour, layer);
    if (!layer) for (uint32_t y = oy; y < oy + th; y++) for (uint32_t x = ox; x < ox + tw; x++) dst[(size_t)y * w + x] &= 0xffffffu;
}
void desk_image_scale(const struct desk_image *src,uint32_t *dst,uint32_t w,uint32_t h,int mode,uint32_t colour)
{ image_scale(src,dst,w,h,mode,colour,0); }
void desk_image_scale_layer(const struct desk_image *src,uint32_t *dst,uint32_t w,uint32_t h,int mode)
{ image_scale(src,dst,w,h,mode,0,1); }

static void caption(uint32_t *out, uint32_t width, uint32_t height, int32_t y0, int font, uint32_t colour, int aa)
{
    /* "CiukiOS" under the portrait in the theme's large face and desktop
     * label colour, where the plain compositor puts it (compositor.c portrait). */
    struct desk_paint p = {out, (int)width, (int)height, 0, 0, (int)width, (int)height};
    int w = desk_text_measure(font, "CiukiOS");
    desk_paint_text_n_aa(&p, font, ((int32_t)width - w) / 2, y0 + 256 + 28, "CiukiOS", (size_t)-1, colour, aa);
}

/* Original geometric 8x8 masks, authored for CiukiOS; no vendor artwork. */
static const uint8_t pattern_rows[DESK_WALL_PATTERNS][8]={
    {0,0,0,0,0,0,0,0},
    {255,8,8,8,255,128,128,128},
    {0,60,66,90,90,66,60,0},
    {240,240,240,240,15,15,15,15},
    {60,66,129,129,129,129,66,60},
    {255,129,129,129,129,129,129,255}
};
uint32_t desk_wallpaper_pattern_pixel(int pattern, uint32_t x, uint32_t y, uint32_t colour)
{
    if (pattern<=0 || pattern>=DESK_WALL_PATTERNS) return colour&0xffffffu;
    return pattern_rows[pattern][y&7u]&(128u>>(x&7u)) ? 0u : colour&0xffffffu;
}
int desk_wallpaper_render_pattern(uint32_t *out, uint32_t width, uint32_t height, uint32_t top, uint32_t bottom,
                          const char *spec, int mode, uint32_t colour,
                          const struct desk_image *image, const uint32_t *portrait, int pattern)
{
    return desk_wallpaper_render_pattern_styled(out,width,height,top,bottom,spec,mode,colour,image,portrait,
                                       desk_theme->font_large,desk_theme->desktop_label,desk_theme->text_aa,pattern);
}
int desk_wallpaper_render(uint32_t *out, uint32_t width, uint32_t height, uint32_t top, uint32_t bottom,
                          const char *spec, int mode, uint32_t colour,
                          const struct desk_image *image, const uint32_t *portrait)
{
    return desk_wallpaper_render_styled(out,width,height,top,bottom,spec,mode,colour,image,portrait,
                                       desk_theme->font_large,desk_theme->desktop_label,desk_theme->text_aa);
}
int desk_wallpaper_render_styled(uint32_t *out, uint32_t width, uint32_t height, uint32_t top, uint32_t bottom,
                          const char *spec, int mode, uint32_t colour,
                          const struct desk_image *image, const uint32_t *portrait, int font, uint32_t label, int aa)
{
    return desk_wallpaper_render_pattern_styled(out,width,height,top,bottom,spec,mode,colour,image,portrait,font,label,aa,0);
}
int desk_wallpaper_render_pattern_styled(uint32_t *out, uint32_t width, uint32_t height, uint32_t top, uint32_t bottom,
                          const char *spec, int mode, uint32_t colour,
                          const struct desk_image *image, const uint32_t *portrait, int font, uint32_t label, int aa, int pattern)
{
    colour &= 0xffffffu;
    for (uint32_t y=0;y<height;y++) for (uint32_t x=0;x<width;x++)
        out[(size_t)y*width+x]=desk_wallpaper_pattern_pixel(pattern,x,y,colour);
    if (!spec || !strcmp(spec, "none")) return 0;
    if (!strcmp(spec, "ciuki")) {
        /* Exactly where the plain compositor puts the approved portrait. */
        if (!portrait || width < 256) return portrait ? 0 : -1;
        int32_t x0 = ((int32_t)width - 256) / 2;
        int32_t y0 = (int32_t)top + ((int32_t)height - (int32_t)top - 256 - 36) / 2;
        if (y0 < (int32_t)top) y0 = (int32_t)top;
        for (int32_t y = 0; y < 256; y++) {
            if (y0 + y < 0 || (uint32_t)(y0 + y) >= height) continue;
            for (int32_t x = 0; x < 256; x++) {
                uint32_t p = portrait[y * 256 + x] & 0xffffffu;
                /* The converted array is composited over #375564; its fully
                 * transparent surround takes the chosen desktop colour. */
                out[(size_t)(y0 + y) * width + (uint32_t)(x0 + x)] = p == 0x375564u ? out[(size_t)(y0+y)*width+(uint32_t)(x0+x)] : p;
            }
        }
        caption(out, width, height, y0, font, label, aa);
        return 0;
    }
    uint32_t area_h = height > top + bottom ? height - top - bottom : height;
    uint32_t oy = height > top + bottom ? top : 0;
    if (!strncmp(spec, "builtin:", 8)) {
        desk_builtin_paint(spec + 8, out, width, height, width);
        return 0;
    }
    if (!image || !image->pixels) return -1;
    if (pattern>0 && pattern<DESK_WALL_PATTERNS) {
        uint32_t image_h=mode==CFG_WALL_TILE ? height : area_h, image_y=mode==CFG_WALL_TILE ? 0 : oy;
        uint32_t *layer=malloc((size_t)width*image_h*4u); if (!layer) return -1;
        desk_image_scale_layer(image,layer,width,image_h,mode);
        for (size_t i=0;i<(size_t)width*image_h;i++) out[(size_t)image_y*width+i]=desk_wallpaper_over(layer[i],out[(size_t)image_y*width+i]);
        free(layer); return 0;
    }
    if (mode == CFG_WALL_TILE) {
        desk_image_scale(image, out, width, height, mode, colour);
        return 0;
    }
    /* Centre, stretch, fit and fill use the area between the top bar and the taskbar. */
    uint32_t *box = malloc((size_t)width * area_h * 4u);
    if (!box) return -1;
    desk_image_scale(image, box, width, area_h, mode, colour);
    memcpy(out + (size_t)oy * width, box, (size_t)width * area_h * 4u);
    free(box);
    return 0;
}
