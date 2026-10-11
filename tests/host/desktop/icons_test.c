/* SPDX-License-Identifier: MIT
 * Host harness for libdeskui's Tango icon store reader and icon map
 * (test_icons.py). Modes:
 *   dump OUT              every store name, then "ciuki", at 16/22/32/48:
 *                         name NUL size-byte R,G,B,A... (the decoded_sha256 stream)
 *   map                   one line per icon_map.c entry: object tango s16 s22 s32 s48
 *   probe NAME SIZE       "OK <fnv1a32>" or "NULL"
 *   notice                the store's licence notice
 *   sheet LIST OUT.ppm    contact sheet: LIST lines "label<TAB>object", icons at 32 px
 * The store and portrait paths come from -DDU_ICON_STORE_PATH/-DDU_ICON_PORTRAIT_PATH
 * or, when ICONS_STORE/ICONS_PORTRAIT are set, from du_icon_paths(). */
#include "deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned sizes[4] = {16, 22, 32, 48};

static void put_rgba(FILE *f, const struct du_icon *icon)
{
    for (unsigned i = 0; i < icon->size * icon->size; i++) {
        uint32_t p = icon->argb[i];
        unsigned char b[4] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p, (unsigned char)(p >> 24)};
        assert(fwrite(b, 1, 4, f) == 4);
    }
}

static int dump(const char *out)
{
    FILE *f = fopen(out, "wb");
    assert(f);
    unsigned count = du_icon_count(), failures = 0;
    for (unsigned i = 0; i <= count; i++) {
        const char *name = i < count ? du_icon_store_name(i) : DU_ICON_CIUKI;
        for (unsigned s = 0; s < 4; s++) {
            const struct du_icon *icon = du_icon(name, sizes[s]);
            if (!icon || icon->size != sizes[s] || icon != du_icon(name, sizes[s])) { printf("FAIL %s %u\n", name, sizes[s]); failures++; continue; }
            assert(fwrite(name, 1, strlen(name) + 1, f) == strlen(name) + 1);
            assert(fputc((int)sizes[s], f) != EOF);
            put_rgba(f, icon);
        }
    }
    assert(!fclose(f));
    printf("DUMP names=%u failures=%u\n", count, failures);
    return failures != 0;
}

static int map(void)
{
    for (unsigned i = 0; i < du_icon_map_count; i++) {
        const char *object = du_icon_map[i].object, *name = du_icon_name(object);
        printf("%s %s", object, name ? name : "-");
        for (unsigned s = 0; s < 4; s++) {
            const struct du_icon *icon = du_icon_for(object, sizes[s]);
            printf(" %u", icon && icon->size == sizes[s] && icon->argb ? sizes[s] : 0u);
        }
        printf("\n");
    }
    return 0;
}

static uint32_t fnv(const struct du_icon *icon)
{
    uint32_t h = 0x811c9dc5u;
    for (unsigned i = 0; i < icon->size * icon->size; i++) {
        uint32_t p = icon->argb[i];
        unsigned char b[4] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p, (unsigned char)(p >> 24)};
        for (int k = 0; k < 4; k++) h = (h ^ b[k]) * 0x01000193u;
    }
    return h;
}

/* Contact sheet: four columns of 32-pixel icons with their labels. */
static int sheet(const char *list, const char *out)
{
    enum { COLS = 4, CELL_W = 300, CELL_H = 44, TOP = 30 };
    char line[256], labels[512][96], objects[512][96];
    unsigned n = 0;
    FILE *f = fopen(list, "r");
    assert(f);
    while (n < 512 && fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
        if (!tab) continue;
        if (nl) *nl = 0;
        *tab = 0;
        snprintf(labels[n], sizeof(labels[n]), "%s", line);
        snprintf(objects[n], sizeof(objects[n]), "%s", tab + 1);
        n++;
    }
    fclose(f);
    int width = COLS * CELL_W, height = TOP + (int)((n + COLS - 1) / COLS) * CELL_H + 8;
    uint32_t *pixels = malloc((size_t)width * (size_t)height * 4);
    assert(pixels);
    struct du_canvas c = {pixels, width, height, {0, 0, width, height}};
    du_fill(&c, (struct du_rect){0, 0, width, height}, DU_FACE);
    du_text(&c, 8, 10, "CiukiOS system icons: Tango Icon Theme 0.8.90 (Public Domain), 32 px, from /system/icons/tango.cis", DU_INK);
    unsigned missing = 0;
    for (unsigned i = 0; i < n; i++) {
        int x = (int)(i % COLS) * CELL_W + 8, y = TOP + (int)(i / COLS) * CELL_H;
        du_fill(&c, (struct du_rect){x - 2, y, CELL_W - 12, CELL_H - 4}, DU_WINDOW);
        const char *name = du_icon_name(objects[i]);
        const struct du_icon *icon = du_icon(name, 32);
        if (!icon) missing++;
        du_icon_draw(&c, x + 2, y + 4, icon, DU_ICON_NORMAL);
        du_text_fit(&c, x + 42, y + 8, CELL_W - 56, labels[i], DU_INK);
        du_text_fit(&c, x + 42, y + 22, CELL_W - 56, name ? name : "(none)", DU_GRAY);
    }
    f = fopen(out, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        unsigned char rgb[3] = {(unsigned char)(pixels[i] >> 16), (unsigned char)(pixels[i] >> 8), (unsigned char)pixels[i]};
        assert(fwrite(rgb, 1, 3, f) == 3);
    }
    assert(!fclose(f));
    free(pixels);
    printf("SHEET cells=%u missing=%u %dx%d\n", n, missing, width, height);
    return missing != 0;
}

int main(int argc, char **argv)
{
    const char *store = getenv("ICONS_STORE"), *portrait = getenv("ICONS_PORTRAIT");
    if (store || portrait) assert(!du_icon_paths(store, portrait));
    if (argc == 3 && !strcmp(argv[1], "dump")) return dump(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "map")) return map();
    if (argc == 4 && !strcmp(argv[1], "probe")) {
        const struct du_icon *icon = du_icon(argv[2], (unsigned)atoi(argv[3]));
        if (icon) printf("OK %08x\n", fnv(icon)); else printf("NULL\n");
        printf("COUNT %u\n", du_icon_count());
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "notice")) { const char *n = du_icon_notice(); printf("%s\n", n ? n : "(none)"); return !n; }
    if (argc == 4 && !strcmp(argv[1], "sheet")) return sheet(argv[2], argv[3]);
    fprintf(stderr, "usage: icons_test dump OUT | map | probe NAME SIZE | notice | sheet LIST OUT.ppm\n");
    return 2;
}
