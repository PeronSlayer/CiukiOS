/* Wallpaper placement and the desktop.cfg wallpaper modes on the host:
 * fill (uniform cover scale, centred crop) against the uniform stretch it
 * crops, the other modes' names and values, the defaults, and the owner's
 * Ciuk2 scene when its path is given. Never guest evidence.
 *   wallpaper_test [Ciuk2.png]
 * SPDX-License-Identifier: MIT */
#include "settings.h"
#include "wallpaper.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLOUR 0x123456u

static uint32_t *buffer(size_t pixels) { uint32_t *p = malloc(pixels * 4u); assert(p); return p; }

/* An opaque test card: row and column in the channels, so a shifted or
 * rescaled window never matches by accident. */
static struct desk_image card(uint32_t w, uint32_t h)
{
    struct desk_image m = {buffer((size_t)w * h), w, h};
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++)
        m.pixels[(size_t)y * w + x] = 0xff000000u | (y & 255u) << 16 | ((x * 7u + y * 3u) & 255u) << 8 | (x & 255u);
    return m;
}

/* Fill of src into w*h is the centred w*h window of the stretch to tw*th,
 * the uniform scale max(w/width, h/height) of the image. */
static void check_fill(const struct desk_image *src, uint32_t w, uint32_t h, uint32_t tw, uint32_t th, const char *what)
{
    uint32_t *fill = buffer((size_t)w * h), *full = buffer((size_t)tw * th), *stretch = buffer((size_t)w * h);
    desk_image_scale(src, fill, w, h, CFG_WALL_FILL, COLOUR);
    desk_image_scale(src, full, tw, th, CFG_WALL_STRETCH, COLOUR);
    uint32_t cx = (tw - w) / 2, cy = (th - h) / 2;
    for (uint32_t y = 0; y < h; y++)
        assert(!memcmp(fill + (size_t)y * w, full + (size_t)(y + cy) * tw + cx, (size_t)w * 4u));
    /* Not a non-uniform stretch, unless the aspect ratios already agree. */
    desk_image_scale(src, stretch, w, h, CFG_WALL_STRETCH, COLOUR);
    assert(!memcmp(fill, stretch, (size_t)w * h * 4u) == (tw == w && th == h));
    printf("fill %s: %ux%u into %ux%u: scaled %ux%u, crop %u columns and %u rows per side\n",
           what, src->width, src->height, w, h, tw, th, cx, cy);
    free(fill); free(full); free(stretch);
}

/* desk_wallpaper_render puts the fill in the work area between the bars. */
static void check_render(const struct desk_image *src, uint32_t width, uint32_t height, int mode)
{
    uint32_t area = height - 64, *out = buffer((size_t)width * height), *box = buffer((size_t)width * area);
    assert(!desk_wallpaper_render(out, width, height, 32, 32, "/system/wallpapers/test.png", mode, COLOUR, src, NULL));
    desk_image_scale(src, box, width, area, mode, COLOUR);
    for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++) {
        uint32_t v = out[(size_t)y * width + x];
        if (y < 32 || y >= height - 32) assert(v == COLOUR);
        else assert(v == box[(size_t)(y - 32) * width + x]);
    }
    free(out); free(box);
}

static void settings(void)
{
    struct ciuki_settings s, t;
    char text[CFG_TEXT_MAX];
    /* Values and names are stable: files written before fill still parse. */
    assert(CFG_WALL_CENTER == 0 && CFG_WALL_STRETCH == 1 && CFG_WALL_TILE == 2 && CFG_WALL_FIT == 3 && CFG_WALL_FILL == 4);
    static const char *const names[] = {"center", "stretch", "tile", "fit", "fill"};
    assert(CFG_WALL_MODES == 5);
    for (int i = 0; i < CFG_WALL_MODES; i++) {
        assert(!strcmp(cfg_mode_names[i], names[i]));
        cfg_defaults(&s);
        snprintf(text, sizeof(text), "version=1\nwallpaper_mode=%s\n", names[i]);
        assert(cfg_parse(&s, text, strlen(text)) == 0 && s.wallpaper_mode == i);
    }
    /* The default: the owner's Ciuk2 scene, filled. */
    cfg_defaults(&s);
    assert(!strcmp(s.wallpaper, "/system/wallpapers/Ciuk2.png") && s.wallpaper_mode == CFG_WALL_FILL);
    size_t n = cfg_format(&s, text, sizeof(text));
    assert(n && strstr(text, "\nwallpaper=/system/wallpapers/Ciuk2.png\nwallpaper_mode=fill\n"));
    /* Round trip of fill over a file that said center. */
    cfg_defaults(&t);
    static const char old[] = "wallpaper=/system/wallpapers/Ciuk1.png\nwallpaper_mode=center\n";
    assert(!cfg_parse(&t, old, sizeof(old) - 1) && t.wallpaper_mode == CFG_WALL_CENTER);
    t.wallpaper_mode = CFG_WALL_FILL;
    char again[CFG_TEXT_MAX];
    n = cfg_format(&t, text, sizeof(text));
    assert(n && strstr(text, "\nwallpaper=/system/wallpapers/Ciuk1.png\nwallpaper_mode=fill\n"));
    cfg_defaults(&s); s.wallpaper_mode = CFG_WALL_TILE;
    assert(!cfg_parse(&s, text, n) && s.wallpaper_mode == CFG_WALL_FILL && !strcmp(s.wallpaper, t.wallpaper));
    assert(cfg_format(&s, again, sizeof(again)) == n && !memcmp(text, again, n));
    /* Unknown or differently spelt modes are rejected and keep the default. */
    static const char bad[] = "wallpaper_mode=cover\nwallpaper_mode=Fill\nwallpaper_mode=\n";
    cfg_defaults(&s);
    assert(cfg_parse(&s, bad, sizeof(bad) - 1) == 3 && s.wallpaper_mode == CFG_WALL_FILL);
    /* An out-of-range value in memory is written as the first mode. */
    s.wallpaper_mode = CFG_WALL_MODES;
    assert(cfg_format(&s, text, sizeof(text)) && strstr(text, "\nwallpaper_mode=center\n"));
    cfg_defaults(&s); s.icon_auto_arrange=1; s.icon_sort=3;
    assert(cfg_icon_label_set(&s,"/Computer","My Computer"));
    char label[64]; assert(cfg_icon_label(&s,"/Computer",label,sizeof(label)) && !strcmp(label,"My Computer"));
    n=cfg_format(&s,text,sizeof(text)); assert(n && strstr(text,"icon_auto_arrange=1\nicon_sort=3\n"));
    cfg_defaults(&t); assert(!cfg_parse(&t,text,n));
    assert(t.icon_auto_arrange && t.icon_sort==3 && cfg_icon_label(&t,"/Computer",label,sizeof(label)) && !strcmp(label,"My Computer"));
    assert(cfg_icon_label_set(&t,"/Computer","Computer") && cfg_icon_label(&t,"/Computer",label,sizeof(label)) && !strcmp(label,"Computer"));
    cfg_icon_label_forget(&t,"/Computer"); assert(!cfg_icon_label(&t,"/Computer",label,sizeof(label)));
    printf("settings: wallpaper modes, G19 sort/auto-arrange and system icon label round trips\n");
}

static void check_layer(void)
{
    struct desk_image src=card(17,11);
    for (unsigned i=0;i<17*11;i++) src.pixels[i]=(src.pixels[i]&0xffffffu)|((i*37u&255u)<<24);
    const unsigned sizes[][2]={{9,7},{40,30},{17,11}};
    for (unsigned n=0;n<3;n++) for (int mode=0;mode<CFG_WALL_MODES;mode++) {
        unsigned w=sizes[n][0],h=sizes[n][1];
        uint32_t *layer=buffer(w*h),*expected=buffer(w*h);
        desk_image_scale_layer(&src,layer,w,h,mode);
        for (unsigned colour=0;colour<3;colour++) {
            uint32_t background=colour==0 ? COLOUR : colour==1 ? 0xffffff : 0;
            desk_image_scale(&src,expected,w,h,mode,background);
            for (unsigned i=0;i<w*h;i++) assert(desk_wallpaper_over(layer[i],background)==expected[i]);
        }
        free(layer); free(expected);
    }
    desk_image_free(&src);
    puts("PASS colour-independent layer: transparent pixels, all five modes, up/down/same size, three colours");
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 2);
    settings();
    check_layer();
    /* The Ciuki scenes' 4:3 size in the 1024x704 work area of 1024x768:
     * scale 1024/1448 (768 rows), 32 rows cropped at the top and the bottom. */
    struct desk_image scene = card(1448, 1086);
    check_fill(&scene, 1024, 704, 1024, 768, "4:3 down");
    check_fill(&scene, 800, 536, 800, 600, "4:3 down at 800x600");
    check_fill(&scene, 1280, 960, 1280, 960, "4:3 at 1280x1024 (same aspect)");
    check_fill(&scene, 1920, 1016, 1920, 1440, "4:3 at 1920x1080");
    check_render(&scene, 1024, 768, CFG_WALL_FILL);
    check_render(&scene, 1024, 768, CFG_WALL_FIT);
    desk_image_free(&scene);
    /* A wide image crops columns, a tall one crops rows (bilinear upscale). */
    struct desk_image wide = card(2000, 1000), tall = card(600, 1200), odd = card(1000, 700), small = card(64, 48);
    check_fill(&wide, 1024, 704, 1408, 704, "wide");
    check_fill(&tall, 1024, 704, 1024, 2048, "tall up");
    check_fill(&odd, 1024, 704, 1024, 716, "1000x700 up");
    check_fill(&small, 1024, 704, 1024, 768, "64x48 up");
    desk_image_free(&wide); desk_image_free(&tall); desk_image_free(&odd); desk_image_free(&small);
    /* Same size: an exact copy. */
    struct desk_image same = card(1024, 704);
    uint32_t *copy = buffer(1024u * 704u);
    desk_image_scale(&same, copy, 1024, 704, CFG_WALL_FILL, COLOUR);
    for (size_t i = 0; i < 1024u * 704u; i++) assert(copy[i] == (same.pixels[i] & 0xffffffu));
    free(copy); desk_image_free(&same);
    /* A zero-sized image leaves the colour. */
    uint32_t dot = 0xff00ff00u, *plain = buffer(16 * 16);
    struct desk_image empty = {&dot, 0, 1};
    desk_image_scale(&empty, plain, 16, 16, CFG_WALL_FILL, COLOUR);
    for (size_t i = 0; i < 16 * 16; i++) assert(plain[i] == COLOUR);
    free(plain);
    if (argc == 2) {
        /* The owner's Ciuk2 scene as the default desktop decodes it. */
        FILE *f = fopen(argv[1], "rb"); assert(f);
        assert(!fseek(f, 0, SEEK_END)); long size = ftell(f); assert(size > 0 && !fseek(f, 0, SEEK_SET));
        uint8_t *data = malloc((size_t)size); assert(data && fread(data, 1, (size_t)size, f) == (size_t)size); fclose(f);
        struct desk_image ciuk2;
        assert(!desk_image_decode(data, (size_t)size, &ciuk2) && ciuk2.width == 1448 && ciuk2.height == 1086);
        free(data);
        check_fill(&ciuk2, 1024, 704, 1024, 768, "Ciuk2");
        check_render(&ciuk2, 1024, 768, CFG_WALL_FILL);
        desk_image_free(&ciuk2);
    }
    printf("PASS wallpaper fill\n");
    return 0;
}
