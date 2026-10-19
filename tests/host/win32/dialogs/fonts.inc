/* f5-10C font selection: aliases, the Windows 98 dialog-font plan, metrics,
 * enumeration, font resources, the shared budget and parser bounds.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
struct abc {
  int a;
  unsigned b;
  int c;
};
struct kerning {
  uint16_t first, second;
  int amount;
};
struct glyph_metrics {
  unsigned box_x, box_y;
  POINT origin;
  int16_t inc_x, inc_y;
};
struct font_stats {
  DWORD faces, open_faces, face_bytes, face_peak, glyphs, glyph_bytes,
      glyph_peak, total_peak, budget, hits, misses, evictions, face_evictions,
      store_bytes;
};
struct enum_entry {
  struct u32_logfont lf;
  uint8_t full[64], style[32], script[32];
};
struct enum_metrics {
  struct u32_textmetric tm;
  DWORD flags;
  unsigned size_em, cell, average;
};
typedef DWORD(W32API *create_font_t)(int, int, int, int, int, DWORD, DWORD,
                                     DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                                     const char *);
typedef int(W32API *text_face_t)(DWORD, int, char *);
typedef DWORD(W32API *font_data_t)(DWORD, DWORD, DWORD, void *, DWORD);
typedef BOOL(W32API *widths_t)(DWORD, unsigned, unsigned, int *);
typedef BOOL(W32API *abc_t)(DWORD, unsigned, unsigned, struct abc *);
typedef DWORD(W32API *kerning_t)(DWORD, DWORD, struct kerning *);
typedef DWORD(W32API *outline_t)(DWORD, unsigned, unsigned,
                                 struct glyph_metrics *, DWORD, void *,
                                 const void *);
typedef unsigned(W32API *otm_t)(DWORD, unsigned, void *);
typedef int(W32API *enum_proc_t)(const struct enum_entry *,
                                 const struct enum_metrics *, DWORD, intptr_t);
typedef int(W32API *enum_t)(DWORD, struct u32_logfont *, enum_proc_t, intptr_t,
                            DWORD);
typedef int(W32API *add_t)(const char *);
typedef BOOL(W32API *remove_t)(const char *);
typedef BOOL(W32API *fot_t)(DWORD, const char *, const char *, const char *);
typedef void(W32API *stats_t)(struct font_stats *);
typedef BOOL(W32API *backend_t)(const void *);
typedef BOOL(W32API *text_out_t)(DWORD, int, int, const char *, int);
typedef void(W32API *exit_t)(void);
typedef int(W32API *validate_t)(const void *, DWORD);
extern int gdi_font_validate(const void *, DWORD);
extern int gdi_host_backend(void);
static DWORD font_dc;
static DWORD make_font(int height, int weight, BOOL italic, const char *face,
                       DWORD pitch) {
  DWORD f = GDI(create_font_t, CreateFontA)(height, 0, 0, 0, weight, italic, 0,
                                             0, 1, 0, 0, 0, pitch, face);
  CHECK(f);
  return f;
}
static DWORD file_size(const char *name) {
  char path[700];
  snprintf(path, sizeof(path), "%s/%s", fonts_dir, name);
  struct stat st;
  CHECK(!stat(path, &st));
  return (DWORD)st.st_size;
}
/* Selects the font; returns metrics, face and the realized file size. */
static void probe(DWORD font, struct u32_textmetric *tm, char *face,
                  DWORD *bytes) {
  const struct u32_graphics *g = u32_graphics();
  DWORD old = g->select(font_dc, font);
  CHECK(old);
  CHECK(g->metrics(font_dc, tm));
  CHECK(GDI(text_face_t, GetTextFaceA)(font_dc, 32, face) > 0);
  *bytes = GDI(font_data_t, GetFontData)(font_dc, 0, 0, 0, 0);
  g->select(font_dc, old);
}
static int alphabet_units(DWORD font) {
  const struct u32_graphics *g = u32_graphics();
  DWORD old = g->select(font_dc, font);
  U32SIZE size;
  CHECK(g->extent(font_dc,
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52,
                  &size));
  g->select(font_dc, old);
  return ((int)size.cx / 26 + 1) / 2;
}
static UINT enumerated;
static char families[64][32];
static UINT family_count, style_flags;
static int W32API enum_proc(const struct enum_entry *e,
                            const struct enum_metrics *m, DWORD type,
                            intptr_t lp) {
  CHECK(type == 4);
  CHECK(m->tm.height > 0 && m->size_em >= 16);
  enumerated++;
  if (lp == 1 && family_count < 64)
    snprintf(families[family_count++], 32, "%s", e->lf.face);
  if (lp == 2)
    style_flags |= m->flags;
  return 1;
}
static BOOL listed(const char *name) {
  for (UINT i = 0; i < family_count; i++)
    if (!strcmp(families[i], name))
      return 1;
  return 0;
}
static void enumerate_families(void) {
  struct u32_logfont lf = {0};
  lf.charset = 1;
  family_count = 0;
  enumerated = 0;
  CHECK(GDI(enum_t, EnumFontFamiliesExA)(font_dc, &lf, enum_proc, 1, 0));
}
/* Same-length rename of the family in a copy of Liberation Sans: a valid
 * single-style face for the synthesis and font-resource lifecycle tests. */
static void write_renamed_face(const char *w32) {
  char path[700], target[1024];
  snprintf(path, sizeof(path), "%s/LiberationSans-Regular.ttf", fonts_dir);
  FILE *f = fopen(path, "rb");
  CHECK(f);
  static uint8_t data[1 << 20];
  size_t n = fread(data, 1, sizeof(data), f);
  fclose(f);
  CHECK(n > 1000);
  static const char from[] = "Liberation Sans", to[] = "CiukiOS TestFnt";
  UINT replaced = 0;
  for (size_t i = 0; i + 30 <= n; i++) {
    BOOL wide = 1, narrow = 1;
    for (UINT k = 0; k < 15; k++) {
      if (data[i + 2 * k] != 0 || data[i + 2 * k + 1] != (uint8_t)from[k])
        wide = 0;
      if (data[i + k] != (uint8_t)from[k])
        narrow = 0;
    }
    if (wide) {
      for (UINT k = 0; k < 15; k++)
        data[i + 2 * k + 1] = (uint8_t)to[k];
      replaced++;
    } else if (narrow) {
      memcpy(data + i, to, 15);
      replaced++;
    }
  }
  CHECK(replaced >= 2);
  CHECK(host_path(w32, target, sizeof(target), 1));
  f = fopen(target, "wb");
  CHECK(f && fwrite(data, 1, n, f) == n);
  fclose(f);
}
static void draw_load(DWORD *fonts, UINT count) {
  static uint8_t pixels[256 * 64 * 4];
  struct gdi_dib dib = {pixels, 256, 64, 1024, sizeof(pixels), 32, 1,
                        {0, 0, 0}, {0}, 0};
  DWORD dc = GDI(surface_dc_t, w32_gdi_surface_dc)(&dib, (POINT){0, 0},
                                                   no_damage, 0);
  CHECK(dc);
  const struct u32_graphics *g = u32_graphics();
  static const char text[] =
      "The quick brown fox jumps over the lazy dog 0123456789 "
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz !?";
  for (UINT k = 0; k < count; k++) {
    DWORD old = g->select(dc, fonts[k]);
    CHECK(GDI(text_out_t, TextOutA)(dc, 0, 0, text, (int)strlen(text)));
    g->select(dc, old);
  }
  g->delete_dc(dc);
}
/* Glyph placement: an 'H' at TA_TOP sits between the cell top and the
 * baseline (ascent rows), nothing below the baseline. */
static void glyph_placement(void) {
  static uint8_t pixels[64 * 32 * 4];
  memset(pixels, 0xff, sizeof(pixels));
  struct gdi_dib dib = {pixels, 64, 32, 256, sizeof(pixels), 32, 1,
                        {0, 0, 0}, {0}, 0};
  DWORD dc = GDI(surface_dc_t, w32_gdi_surface_dc)(&dib, (POINT){0, 0},
                                                   no_damage, 0);
  const struct u32_graphics *g = u32_graphics();
  DWORD f = make_font(-16, 400, 0, "Arial", 0);
  g->select(dc, f);
  g->bk_mode(dc, 1);
  g->text_color(dc, 0);
  CHECK(GDI(text_out_t, TextOutA)(dc, 2, 2, "H", 1));
  struct u32_textmetric tm;
  CHECK(g->metrics(dc, &tm));
  int baseline = 2 + tm.ascent;
  UINT above = 0, below = 0;
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 64; x++)
      if (pixels[(y * 64 + x) * 4] < 128)
        *(y < baseline ? &above : &below) += 1;
  CHECK(above > 20 && below == 0);
  g->select(dc, g->stock(13));
  g->delete_object(f);
  g->delete_dc(dc);
}
static void test_fonts(void) {
  const struct u32_graphics *g = u32_graphics();
  font_dc = g->memory_dc(0);
  CHECK(font_dc);
  glyph_placement();
  struct u32_textmetric tm;
  char face[32];
  DWORD bytes;
  /* Stock objects: System is the Windows 98 16-pixel cell (8x16 units). */
  probe(g->stock(13), &tm, face, &bytes);
  CHECK(tm.height == 16 && tm.ascent == 13 && tm.descent == 3);
  CHECK(!strcmp(face, "System") && tm.weight == 700);
  CHECK(bytes == file_size("LiberationSans-Bold.ttf"));
  CHECK(GetDialogBaseUnits() == (LONG)(8 | (16 << 16)));
  printf("FONT System cell %ld base %ldx%ld\n", (long)tm.height,
         (long)(GetDialogBaseUnits() & 0xffff),
         (long)(GetDialogBaseUnits() >> 16));
  probe(g->stock(17), &tm, face, &bytes);
  printf("FONT DEFAULT_GUI %s %ld\n", face, (long)tm.height);
  CHECK(tm.height == 13 && !strcmp(face, "MS Sans Serif"));
  probe(g->stock(16), &tm, face, &bytes);
  CHECK(!strcmp(face, "Fixedsys") && tm.height == 15 && tm.average == 8);
  probe(g->stock(10), &tm, face, &bytes);
  CHECK(!strcmp(face, "Terminal") && tm.charset == 255 && tm.height == 12);
  /* Dialog-font plan: 8 pt MS Sans Serif and its names give 6x13. */
  static const char *const dialog[][2] = {{"MS Sans Serif", "MS Sans Serif"},
                                          {"MS Shell Dlg", "MS Sans Serif"},
                                          {"Helv", "MS Sans Serif"},
                                          {"Tahoma", "Tahoma"}};
  for (UINT i = 0; i < 4; i++) {
    DWORD f = make_font(-11, 400, 0, dialog[i][0], 0);
    probe(f, &tm, face, &bytes);
    CHECK(tm.height == 13 && !strcmp(face, dialog[i][1]));
    CHECK(bytes == file_size("LiberationSans-Regular.ttf"));
    CHECK(alphabet_units(f) == 6);
    printf("FONT %s 8pt -> %s cell %ld ascent %ld internal %ld avg %ld base "
           "%dx%ld\n",
           dialog[i][0], face, (long)tm.height, (long)tm.ascent,
           (long)tm.internal, (long)tm.average, alphabet_units(f),
           (long)tm.height);
    g->delete_object(f);
  }
  DWORD cell = make_font(13, 400, 0, "MS Sans Serif", 0);
  probe(cell, &tm, face, &bytes);
  CHECK(tm.height == 13 && alphabet_units(cell) == 6);
  g->delete_object(cell);
  /* Metric-compatible aliases and styles from the real Liberation faces. */
  static const struct {
    const char *name, *file;
    int weight;
    BOOL italic;
  } aliases[] = {{"Arial", "LiberationSans-Regular.ttf", 400, 0},
                 {"Arial", "LiberationSans-Bold.ttf", 700, 0},
                 {"Arial", "LiberationSans-Italic.ttf", 400, 1},
                 {"Arial", "LiberationSans-BoldItalic.ttf", 700, 1},
                 {"Times New Roman", "LiberationSerif-Regular.ttf", 400, 0},
                 {"Times New Roman", "LiberationSerif-Bold.ttf", 700, 0},
                 {"Courier New", "LiberationMono-Regular.ttf", 400, 0},
                 {"Courier New", "LiberationMono-BoldItalic.ttf", 700, 1},
                 {"Arial Bold", "LiberationSans-Bold.ttf", 400, 0},
                 {"Liberation Serif", "LiberationSerif-Regular.ttf", 400, 0}};
  for (UINT i = 0; i < sizeof(aliases) / sizeof(*aliases); i++) {
    DWORD f = make_font(-16, aliases[i].weight, aliases[i].italic,
                        aliases[i].name, 0);
    probe(f, &tm, face, &bytes);
    CHECK(bytes == file_size(aliases[i].file));
    CHECK(tm.weight == (aliases[i].weight >= 600 || i == 8 ? 700 : 400));
    CHECK(!!tm.italic == aliases[i].italic);
    CHECK(tm.pitch & 4);
    g->delete_object(f);
  }
  DWORD mono = make_font(-16, 400, 0, "Courier New", 0);
  probe(mono, &tm, face, &bytes);
  CHECK(!(tm.pitch & 1) && (tm.pitch & 0xf0) == 0x30);
  /* Penalty order: missing faces fall back by family and pitch. */
  static const struct {
    const char *name;
    DWORD pitch;
    const char *expected;
  } fallback[] = {{"Verdana", 0x20, "Arial"},
                  {"Book Antiqua", 0x10, "Times New Roman"},
                  {"No Such Face", 1, "Courier New"},
                  {"", 0x30, "Courier New"},
                  {"Helvetica", 0, "Arial"},
                  {"Times", 0, "Times New Roman"}};
  for (UINT i = 0; i < sizeof(fallback) / sizeof(*fallback); i++) {
    DWORD f = make_font(-12, 400, 0, fallback[i].name, fallback[i].pitch);
    probe(f, &tm, face, &bytes);
    printf("FONT request '%s' pitch/family 0x%02x -> %s\n", fallback[i].name,
           (unsigned)fallback[i].pitch, face);
    CHECK(!strcmp(face, fallback[i].expected));
    g->delete_object(f);
  }
  strcpy(profile_substitute[0], "Fixed Test");
  strcpy(profile_substitute[1], "Courier New");
  DWORD subst = make_font(-12, 400, 0, "Fixed Test", 0);
  probe(subst, &tm, face, &bytes);
  CHECK(!strcmp(face, "Courier New"));
  g->delete_object(subst);
  /* Widths, ABC widths, extents, kerning, outlines and font data. */
  DWORD arial = make_font(-16, 400, 0, "Arial", 0);
  CHECK(g->select(font_dc, arial));
  int widths[26];
  struct abc abc[26];
  CHECK(GDI(widths_t, GetCharWidth32A)(font_dc, 'A', 'Z', widths));
  CHECK(GDI(abc_t, GetCharABCWidthsA)(font_dc, 'A', 'Z', abc));
  int total = 0;
  for (UINT i = 0; i < 26; i++) {
    total += widths[i];
    CHECK(abc[i].a + (int)abc[i].b + abc[i].c == widths[i]);
  }
  U32SIZE size;
  CHECK(g->extent(font_dc, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26, &size));
  CHECK(size.cx == total);
  DWORD pairs = GDI(kerning_t, GetKerningPairsA)(font_dc, 0, 0);
  CHECK(pairs > 100);
  struct kerning *k = calloc(pairs, sizeof(*k));
  CHECK(GDI(kerning_t, GetKerningPairsA)(font_dc, pairs, k) == pairs);
  BOOL av = 0;
  for (DWORD i = 0; i < pairs; i++)
    av |= k[i].first == 'A' && k[i].second == 'V' && k[i].amount < 0;
  CHECK(av);
  free(k);
  struct glyph_metrics gm;
  static const int identity[4] = {0x10000, 0, 0, 0x10000};
  CHECK(GDI(outline_t, GetGlyphOutlineA)(font_dc, 'A', 0, &gm, 0, 0,
                                         identity) == 0);
  CHECK(gm.inc_x == widths[0] && gm.box_x > 0 && gm.box_y > 0);
  DWORD need = GDI(outline_t, GetGlyphOutlineA)(font_dc, 'A', 6, &gm, 0, 0,
                                                identity);
  CHECK(need && need != 0xffffffffu && need == ((gm.box_x + 3) & ~3u) * gm.box_y);
  uint8_t *gray = malloc(need);
  CHECK(GDI(outline_t, GetGlyphOutlineA)(font_dc, 'A', 6, &gm, need, gray,
                                         identity) == need);
  UINT ink = 0;
  for (DWORD i = 0; i < need; i++) {
    CHECK(gray[i] <= 64);
    ink += gray[i] == 64;
  }
  CHECK(ink > 0);
  free(gray);
  CHECK(GDI(outline_t, GetGlyphOutlineA)(font_dc, 'A', 2, &gm, 0, 0,
                                         identity) == 0xffffffffu);
  unsigned otm = GDI(otm_t, GetOutlineTextMetricsA)(font_dc, 0, 0);
  CHECK(otm > 212);
  uint8_t *outline = calloc(1, otm);
  CHECK(GDI(otm_t, GetOutlineTextMetricsA)(font_dc, otm, outline) == otm);
  CHECK(w32_u32(outline + 92) == 2048);
  CHECK(!strcmp((char *)outline + w32_u32(outline + 196), "Arial"));
  free(outline);
  uint8_t head[54];
  CHECK(GDI(font_data_t, GetFontData)(font_dc, 0x64616568u, 0, 0, 0) == 54);
  CHECK(GDI(font_data_t, GetFontData)(font_dc, 0x64616568u, 0, head, 54) == 54);
  CHECK(head[12] == 0x5f && head[13] == 0x0f && head[14] == 0x3c);
  CHECK(GDI(font_data_t, GetFontData)(font_dc, 0x6c6c756eu, 0, 0, 0) ==
        0xffffffffu);
  g->select(font_dc, g->stock(13));
  CHECK(g->select(font_dc, mono));
  CHECK(GDI(kerning_t, GetKerningPairsA)(font_dc, 0, 0) == 0);
  g->select(font_dc, g->stock(13));
  g->delete_object(mono);
  /* Enumeration: virtual families, physical faces, styles. */
  enumerate_families();
  static const char *const expected[] = {
      "Arial",    "Times New Roman", "Courier New",     "MS Sans Serif",
      "System",   "Tahoma",          "Liberation Sans", "Liberation Serif",
      "Liberation Mono"};
  for (UINT i = 0; i < sizeof(expected) / sizeof(*expected); i++)
    CHECK(listed(expected[i]));
  printf("FONT families %u:", family_count);
  for (UINT i = 0; i < family_count; i++)
    printf(" [%s]", families[i]);
  printf("\n");
  struct u32_logfont named = {0};
  named.charset = 1;
  strcpy(named.face, "Arial");
  enumerated = 0;
  style_flags = 0;
  CHECK(GDI(enum_t, EnumFontFamiliesExA)(font_dc, &named, enum_proc, 2, 0));
  CHECK(enumerated == 4 && (style_flags & 0x61) == 0x61);
  /* Font resources: copy into the writable store, registry registration,
   * synthesis for a single-style face, removal and persistence. */
  write_renamed_face("C:\\Program Files\\Sample\\CIUKITST.TTF");
  CHECK(GDI(add_t, AddFontResourceA)("C:\\Program Files\\Sample\\CIUKITST.TTF") ==
        1);
  char installed[700];
  snprintf(installed, sizeof(installed), "%s/CIUKITST.TTF", install_dir);
  struct stat st;
  CHECK(!stat(installed, &st));
  const char *value = reg_string("Software\\Microsoft\\Windows\\CurrentVersion\\Fonts",
                                 "CiukiOS TestFnt (TrueType)");
  CHECK(value && !strcmp(value, "CIUKITST.TTF"));
  HWND listener = owner_window("Fonts", WS_CAPTION, 200, 100);
  owner_fontchange = 0;
  SendMessageA(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);
  CHECK(owner_fontchange == 1);
  DestroyWindow(listener);
  enumerate_families();
  CHECK(listed("CiukiOS TestFnt"));
  DWORD regular = make_font(-16, 400, 0, "CiukiOS TestFnt", 0),
        bold = make_font(-16, 700, 0, "CiukiOS TestFnt", 0),
        italic = make_font(-16, 400, 1, "CiukiOS TestFnt", 0);
  struct u32_textmetric tr, tb, ti;
  probe(regular, &tr, face, &bytes);
  CHECK(!strcmp(face, "CiukiOS TestFnt") && tr.weight == 400);
  probe(bold, &tb, face, &bytes);
  CHECK(tb.weight == 700 && tb.average == tr.average + 1);
  probe(italic, &ti, face, &bytes);
  CHECK(ti.italic);
  int wr, wb;
  g->select(font_dc, regular);
  GDI(widths_t, GetCharWidth32A)(font_dc, 'H', 'H', &wr);
  g->select(font_dc, bold);
  GDI(widths_t, GetCharWidth32A)(font_dc, 'H', 'H', &wb);
  CHECK(wb == wr + 1);
  g->select(font_dc, g->stock(13));
  /* Removal while realized fonts exist is deferred until they go away. */
  CHECK(GDI(remove_t, RemoveFontResourceA)("C:\\Program Files\\Sample\\CIUKITST.TTF"));
  CHECK(!stat(installed, &st));
  g->delete_object(regular);
  g->delete_object(bold);
  g->delete_object(italic);
  CHECK(stat(installed, &st));
  CHECK(!reg_string("Software\\Microsoft\\Windows\\CurrentVersion\\Fonts",
                    "CiukiOS TestFnt (TrueType)"));
  enumerate_families();
  CHECK(!listed("CiukiOS TestFnt"));
  /* Persistence: faces registered in HKLM\...\Fonts load in a new session. */
  CHECK(GDI(add_t, AddFontResourceA)("C:\\Program Files\\Sample\\CIUKITST.TTF") ==
        1);
  g->delete_dc(font_dc);
  GDI(exit_t, w32_gdi_process_exit)();
  font_dc = g->memory_dc(0);
  enumerate_families();
  CHECK(listed("CiukiOS TestFnt"));
  CHECK(GDI(remove_t, RemoveFontResourceA)("C:\\Program Files\\Sample\\CIUKITST.TTF"));
  CHECK(stat(installed, &st));
  /* CreateScalableFontResource and its reference file. */
  CHECK(GDI(fot_t, CreateScalableFontResourceA)(
      0, "C:\\Program Files\\Sample\\CIUKITST.FOT", "CIUKITST.TTF",
      "C:\\Program Files\\Sample"));
  CHECK(GDI(add_t, AddFontResourceA)("C:\\Program Files\\Sample\\CIUKITST.FOT") ==
        1);
  enumerate_families();
  CHECK(listed("CiukiOS TestFnt"));
  CHECK(GDI(remove_t, RemoveFontResourceA)("C:\\Program Files\\Sample\\CIUKITST.TTF"));
  /* Invalid resources: truncated TrueType and NE raster fonts. */
  char bad[1024];
  CHECK(host_path("C:\\Program Files\\Sample\\BAD.TTF", bad, sizeof(bad), 1));
  FILE *f = fopen(bad, "wb");
  CHECK(f && fwrite("\0\1\0\0\0\x05", 1, 6, f) == 6);
  fclose(f);
  CHECK(!GDI(add_t, AddFontResourceA)("C:\\Program Files\\Sample\\BAD.TTF"));
  CHECK(host_path("C:\\Program Files\\Sample\\SMALL.FON", bad, sizeof(bad), 1));
  f = fopen(bad, "wb");
  static uint8_t ne[256] = {'M', 'Z'};
  CHECK(f && fwrite(ne, 1, sizeof(ne), f) == sizeof(ne));
  fclose(f);
  CHECK(!GDI(add_t, AddFontResourceA)("C:\\Program Files\\Sample\\SMALL.FON") &&
        GetLastError() == 13);
  /* Budget: many sizes and faces through the shared 2 MiB budget. */
  static const char *const names[] = {"Arial", "Times New Roman",
                                      "Courier New"};
  DWORD fonts[48];
  UINT count = 0;
  for (UINT n = 0; n < 3; n++)
    for (int size = 8; size <= 36; size += 2)
      fonts[count++] = make_font(-size, n == 1 ? 700 : 400, size % 4 == 0,
                                 names[n], 0);
  draw_load(fonts, count);
  draw_load(fonts, count);
  struct font_stats stats;
  GDI(stats_t, w32_gdi_font_statistics)(&stats);
  printf("FONTSTATS faces=%u open_faces=%u face_bytes=%u face_peak=%u "
         "glyphs=%u glyph_bytes=%u glyph_peak=%u total_peak=%u budget=%u "
         "hits=%u misses=%u evictions=%u face_evictions=%u store_bytes=%u\n",
         stats.faces, stats.open_faces, stats.face_bytes, stats.face_peak,
         stats.glyphs, stats.glyph_bytes, stats.glyph_peak, stats.total_peak,
         stats.budget, stats.hits, stats.misses, stats.evictions,
         stats.face_evictions, stats.store_bytes);
  CHECK(stats.total_peak <= 2u * 1024 * 1024 && stats.evictions > 0 &&
        stats.face_evictions > 0 && stats.glyph_peak <= 256u * 1024);
  for (UINT i = 0; i < count; i++)
    g->delete_object(fonts[i]);
  /* Metric-only mode without a native backend: layout still works. */
  CHECK(GDI(backend_t, w32_gdi_set_face_backend)(0));
  DWORD plain = make_font(-11, 400, 0, "MS Sans Serif", 0);
  probe(plain, &tm, face, &bytes);
  CHECK(tm.height == 13);
  CHECK(g->select(font_dc, plain));
  CHECK(g->extent(font_dc, "Setup", 5, &size) && size.cx > 20);
  g->select(font_dc, g->stock(13));
  g->delete_object(plain);
  CHECK(gdi_host_backend());
  g->delete_object(arial);
  g->delete_dc(font_dc);
}
static void test_parser(void) {
  char path[700];
  snprintf(path, sizeof(path), "%s/LiberationSans-Regular.ttf", fonts_dir);
  FILE *f = fopen(path, "rb");
  CHECK(f);
  static uint8_t data[1 << 20], copy[1 << 20];
  size_t n = fread(data, 1, sizeof(data), f);
  fclose(f);
  CHECK(gdi_font_validate(data, (DWORD)n));
  unsigned tables = (unsigned)(data[4] << 8 | data[5]);
  DWORD end = 0;
  for (unsigned i = 0; i < tables; i++) {
    const uint8_t *r = data + 12 + 16 * i;
    DWORD o = (DWORD)r[8] << 24 | (DWORD)r[9] << 16 | (DWORD)r[10] << 8 | r[11],
          l = (DWORD)r[12] << 24 | (DWORD)r[13] << 16 | (DWORD)r[14] << 8 | r[15];
    if (o + l > end)
      end = o + l;
  }
  UINT rejected = 0, checked = 0;
  for (DWORD length = 0; length < n; length += length < 512 ? 1 : 997) {
    int ok = gdi_font_validate(data, length);
    checked++;
    if (length < end)
      CHECK(!ok);
    rejected += !ok;
  }
  /* Overlong table lengths and offsets past the end. */
  for (unsigned i = 0; i < tables; i++) {
    memcpy(copy, data, n);
    uint8_t *r = copy + 12 + 16 * i;
    r[12] = 0xff, r[13] = 0xff, r[14] = 0xff, r[15] = 0xf0;
    CHECK(!gdi_font_validate(copy, (DWORD)n));
    memcpy(copy, data, n);
    r = copy + 12 + 16 * i;
    r[8] = 0x7f;
    CHECK(!gdi_font_validate(copy, (DWORD)n));
    checked += 2;
    rejected += 2;
  }
  /* Header refusals: zero/oversized table counts, CFF and collections. */
  static const uint8_t headers[][4] = {
      {'O', 'T', 'T', 'O'}, {'t', 't', 'c', 'f'}, {'M', 'Z', 0, 0}};
  for (UINT i = 0; i < 3; i++) {
    memcpy(copy, data, n);
    memcpy(copy, headers[i], 4);
    CHECK(!gdi_font_validate(copy, (DWORD)n));
  }
  memcpy(copy, data, n);
  copy[4] = 0, copy[5] = 0;
  CHECK(!gdi_font_validate(copy, (DWORD)n));
  copy[4] = 0, copy[5] = 65;
  CHECK(!gdi_font_validate(copy, (DWORD)n));
  /* Deterministic corruption of the directory and every table header. */
  uint32_t seed = 0x5eed10c;
  for (UINT round = 0; round < 3000; round++) {
    memcpy(copy, data, n);
    UINT flips = 1 + round % 8;
    for (UINT k = 0; k < flips; k++) {
      seed = seed * 1103515245u + 12345u;
      DWORD at = round % 3 ? (seed >> 8) % 4096 : (seed >> 4) % (DWORD)n;
      seed = seed * 1103515245u + 12345u;
      copy[at] ^= (uint8_t)(seed >> 16);
    }
    gdi_font_validate(copy, (DWORD)n);
    checked++;
  }
  printf("PARSER checked=%u rejected=%u tables=%u end=%u size=%zu\n", checked,
         rejected, tables, end, n);
}
