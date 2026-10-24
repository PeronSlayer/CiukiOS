/* G28b: formatted documents through the production spooler: PostScript, PCL and text-only printers, page ranges, validation.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../../../apps/libprint/print.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *root, int id, size_t *n)
{
    char path[512]; snprintf(path, sizeof(path), "%s/j%03d/data", root, id);
    FILE *f = fopen(path, "rb"); assert(f);
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)len + 1); assert(fread(b, 1, (size_t)len, f) == (size_t)len); fclose(f);
    b[len] = 0; *n = (size_t)len; return b;
}
static int has(const char *hay, size_t n, const char *needle) { return memmem(hay, n, needle, strlen(needle)) != NULL; }   /* raster data has NUL bytes */
static unsigned count(const char *hay, size_t n, const char *needle)
{
    unsigned c = 0; size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(hay + i, needle, k)) c++;
    return c;
}
static void driver(const char *root, unsigned kind)
{
    struct cp_config *cfg = malloc(sizeof(*cfg)); assert(cfg && !cp_config_load(root, cfg));
    cfg->printers[0].driver = kind; assert(!cp_config_save(root, cfg)); free(cfg);
}
static struct cp_doc make(void)
{
    static const char text[] = "Hello (World) \\ caf\xc3\xa9 \xe2\x82\xac \xe2\x80\xa2 \xe4\xb8\xad";
    struct cp_doc d; memset(&d, 0, sizeof(d));
    d.pages = 2; d.nops = 4;
    d.ops = calloc(4, sizeof(*d.ops));
    d.text = malloc(sizeof(text)); memcpy(d.text, text, sizeof(text)); d.text_bytes = sizeof(text) - 1;
    d.ops[0] = (struct cp_op){.kind = CP_OP_TEXT, .family = 0, .flags = 1, .size = 24, .page = 1, .x = 1080, .y = 1500, .rgb = 0xff0000, .offset = 0, .length = (uint32_t)d.text_bytes};
    d.ops[1] = (struct cp_op){.kind = CP_OP_RULE, .page = 1, .x = 1080, .y = 1600, .w = 3000, .h = 15, .rgb = 0};
    d.images = calloc(1, sizeof(*d.images)); d.nimages = 1;
    d.images[0].width = 2; d.images[0].height = 2; d.images[0].pixels = malloc(16);
    uint32_t px[4] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff}; memcpy(d.images[0].pixels, px, 16);
    d.ops[2] = (struct cp_op){.kind = CP_OP_IMAGE, .page = 1, .x = 1080, .y = 2000, .w = 1440, .h = 1440, .offset = 0};
    d.ops[3] = (struct cp_op){.kind = CP_OP_TEXT, .family = 1, .flags = 3, .size = 21, .page = 2, .x = 1080, .y = 1500, .offset = 7, .length = 5};   /* "World" */
    d.plain = strdup("plain text of the document\r\n"); d.plain_bytes = strlen(d.plain);
    return d;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *root = argv[1];
    assert(!cp_init(root));
    struct cp_doc d = make();
    size_t n; char *out;
    /* PostScript: the base-14 faces re-encoded, escapes, colours, a rule, an image, one showpage per page */
    driver(root, CP_POSTSCRIPT);
    int id = cp_submit_doc(root, CP_PRINTERS, "Test", &d, NULL, NULL); assert(id > 0);
    out = slurp(root, id, &n);
    assert(!strncmp(out, "%!PS-Adobe-3.0\n<< /PageSize [", 29) && strstr(out, "/Helvetica-Bold /F1 relatin") && strstr(out, "/Times-BoldItalic /F7 relatin") && strstr(out, "/Courier /F8 relatin"));
    assert(strstr(out, "/F1 findfont 12.0 scalefont setfont") && strstr(out, "1.000 0.000 0.000 setrgbcolor"));
    assert(strstr(out, "(Hello \\(World\\) \\\\ caf\\351 ? \\267 ?) show"));   /* escapes; e acute; the Euro has no ISOLatin1 glyph; the bullet is a periodcentered */
    assert(strstr(out, " rectfill") && strstr(out, "colorimage\nff000000ff00\n0000ffffffff\n") && strstr(out, "2 2 8 [2 0 0 -2 0 2]"));
    assert(count(out, n, "showpage\n") == 2 && strstr(out, "%%EOF"));
    /* the baseline of the first text: 1500 twips from the top is 75 pt; the page is 842 pt tall or Letter: from the printer's page */
    assert(strstr(out, "54.00 "));
    free(out);
    /* a page range prints only those pages */
    struct cp_page page; cp_page_default(&page); page.first = 2; page.last = 2;
    id = cp_submit_doc(root, CP_PRINTERS, "Range", &d, &page, NULL); assert(id > 0);
    out = slurp(root, id, &n); assert(count(out, n, "showpage\n") == 1 && !strstr(out, "(Hello") && strstr(out, "(World) show") && strstr(out, "/F7 findfont 10.5 scalefont setfont")); free(out);
    /* PCL: scalable faces, positions in dots, a rectangle, a raster, a form feed per page */
    driver(root, CP_PCL);
    id = cp_submit_doc(root, CP_PRINTERS, "Pcl", &d, NULL, NULL); assert(id > 0);
    out = slurp(root, id, &n);
    assert(!strncmp(out, "\033E\033&l0O", 7) && has(out, n, "\033(19U") && has(out, n, "\033(s1p12.0v0s3b4148T") && has(out, n, "\033(s1p10.5v1s3b4101T"));
    assert(has(out, n, "\033*p225x312Y") && has(out, n, "\033*c0P") && has(out, n, "\033*t75R") && count(out, n, "\f") == 2 && n > 100 && out[n - 2] == '\033' && out[n - 1] == 'E');
    assert(has(out, n, "caf\xe9 \x80 \x95 ?"));   /* Windows-1252: e acute, Euro, bullet, and the unknown character */
    free(out);
    /* a text-only printer prints the plain rendering */
    driver(root, CP_TEXT);
    id = cp_submit_doc(root, CP_PRINTERS, "Text", &d, NULL, NULL); assert(id > 0);
    out = slurp(root, id, &n); assert(strstr(out, "plain text of the document") && !strstr(out, "Hello")); free(out);   /* the text path of G31 */
    struct cp_doc noplain = d; noplain.plain = NULL;
    assert(cp_submit_doc(root, CP_PRINTERS, "Text", &noplain, NULL, NULL) < 0 && errno == EINVAL);
    /* validation: unsorted pages, a bad text range, a bad image index, no pages */
    driver(root, CP_POSTSCRIPT);
    struct cp_op swap = d.ops[0]; d.ops[0].page = 2; assert(cp_submit_doc(root, CP_PRINTERS, "x", &d, NULL, NULL) < 0); d.ops[0] = swap;
    d.ops[0].length = 9999; assert(cp_submit_doc(root, CP_PRINTERS, "x", &d, NULL, NULL) < 0); d.ops[0].length = (uint32_t)d.text_bytes;
    d.ops[2].offset = 5; assert(cp_submit_doc(root, CP_PRINTERS, "x", &d, NULL, NULL) < 0); d.ops[2].offset = 0;
    d.pages = 0; assert(cp_submit_doc(root, CP_PRINTERS, "x", &d, NULL, NULL) < 0); d.pages = 2;
    assert(cp_submit_doc(root, CP_PRINTERS, "ok", &d, NULL, NULL) > 0);
    cp_doc_free(&d); cp_doc_free(&d);   /* idempotent */
    puts("PASS printing doc");
    return 0;
}
