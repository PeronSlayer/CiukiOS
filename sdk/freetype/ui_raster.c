/* SPDX-License-Identifier: MIT
 * Build-time UI atlas converter. Uses exactly the SDK's minimal FreeType. */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_DRIVER_H
#include FT_MULTIPLE_MASTERS_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void word(int32_t v)
{
    uint32_t n = (uint32_t)v;
    for (unsigned i = 0; i < 4; i++) if (putchar((int)(n >> (i * 8) & 255)) == EOF) exit(1);
}
static int code(unsigned i)
{ return i < 95 ? (int)i + 32 : i < 191 ? (int)i + 160 - 95 : i == 191 ? 0x2022 : 0x2026; }
int main(int argc, char **argv)
{
    if (argc != 4) return 1;
    unsigned size = (unsigned)strtoul(argv[2], NULL, 10), weight = (unsigned)strtoul(argv[3], NULL, 10);
    if (!size || size > 128 || weight < 100 || weight > 900) return 1;
    FT_Library library = NULL; FT_Face face = NULL; FT_MM_Var *axes = NULL;
    unsigned interpreter = TT_INTERPRETER_VERSION_35;
    if (FT_Init_FreeType(&library) || FT_Property_Set(library, "truetype", "interpreter-version", &interpreter) ||
        FT_New_Face(library, argv[1], 0, &face) || FT_Get_MM_Var(face, &axes)) return 1;
    FT_Fixed *coords = calloc(axes->num_axis, sizeof(*coords));
    if (!coords) return 1;
    for (unsigned i = 0; i < axes->num_axis; i++) {
        FT_Var_Axis *axis = &axes->axis[i]; FT_Fixed value = axis->def;
        if (axis->tag == FT_MAKE_TAG('w','g','h','t')) value = (FT_Fixed)weight * 65536;
        else if (axis->tag == FT_MAKE_TAG('w','d','t','h')) value = 100 * 65536;
        else if (axis->tag == FT_MAKE_TAG('o','p','s','z')) value = (FT_Fixed)size * 65536;
        if (value < axis->minimum) value = axis->minimum;
        if (value > axis->maximum) value = axis->maximum;
        coords[i] = value;
    }
    if (FT_Set_Var_Design_Coordinates(face, axes->num_axis, coords) || FT_Set_Pixel_Sizes(face, 0, size)) return 1;
    free(coords); FT_Done_MM_Var(library, axes);
    word((int32_t)(face->size->metrics.ascender / 64));
    word((int32_t)(-face->size->metrics.descender / 64));
    for (unsigned i = 0; i < 193; i++) {
        if (FT_Load_Char(face, (FT_ULong)code(i), FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL) ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return 1;
        FT_GlyphSlot slot = face->glyph; FT_Bitmap *b = &slot->bitmap;
        if (b->pitch < 0 || b->width > 512 || b->rows > 512) return 1;
        unsigned x0 = b->width, x1 = 0, y0 = b->rows, y1 = 0;
        for (unsigned y = 0; y < b->rows; y++) for (unsigned x = 0; x < b->width; x++) if (b->buffer[y * (unsigned)b->pitch + x]) {
            if (x < x0) x0 = x;
            if (x + 1 > x1) x1 = x + 1;
            if (y < y0) y0 = y;
            if (y + 1 > y1) y1 = y + 1;
        }
        if (!x1) x0 = y0 = y1 = 0;
        word((int32_t)(x1 - x0)); word((int32_t)(y1 - y0));
        word(x1 ? slot->bitmap_left + (int32_t)x0 : 0);
        word(x1 ? -slot->bitmap_top + (int32_t)y0 : 0);
        word((int32_t)((slot->advance.x + 32) / 64));
        for (unsigned y = y0; y < y1; y++)
            if (fwrite(b->buffer + y * (unsigned)b->pitch + x0, 1, x1 - x0, stdout) != x1 - x0) return 1;
    }
    FT_Done_Face(face); FT_Done_FreeType(library);
    return fflush(stdout) ? 1 : 0;
}
