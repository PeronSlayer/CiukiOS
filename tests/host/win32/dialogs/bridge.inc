/* f5-10C2 PE-to-native font bridge on the host: the production gdi32
 * client (sdk/win32/gdi32/bridge.c) over the production w32srv packet
 * protocol (wr_packet, 256-byte stop-and-wait fragments, the algorithm of
 * ntdll's w32_server_exchange) to the production font service
 * (apps/w32srv/fonts.c) over the pinned libfont/FreeType 2.13.3.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../../apps/w32srv/fonts.h"
#include "../../../../src/probes/win32_dialogs/exercise.h"
#include <stdarg.h>
#include <time.h>
#include <setjmp.h>
#include <w32/font_stats.h>
extern int gdi_host_bridge(uint32_t (*)(const void *, uint32_t, void *,
                                        uint32_t, uint32_t *));
extern int gdi_host_backend(void);
extern int gdi_host_render(const char *, int, int, int, const char *,
                           uint32_t *, unsigned, unsigned);
static struct wr_session bridge_session;
static UINT bridge_packets, bridge_transactions;
static struct wf_service *font_service;
static UINT bridge_unready;

static uint32_t bridge_exchange(const void *input, uint32_t n, void *output,
                                uint32_t cap, uint32_t *used) {
  if (bridge_unready) {
    bridge_unready--;
    *used = 0;
    return 1062;
  }
  static uint32_t serial;
  uint32_t id = ++serial;
  if (!id)
    id = ++serial;
  struct w32r_packet in, out;
  uint32_t at = 0, total = 0;
  bridge_session.pid = ui_kernel_pid();
  bridge_transactions++;
  if (n > W32R_MAX)
    return 87;
  while (at < n) {
    memset(&in, 0, sizeof(in));
    in.version = 1, in.id = id, in.kind = at ? W32R_WRITE : W32R_BEGIN;
    in.total = n, in.offset = at;
    uint32_t chunk = n - at > 232 ? 232 : n - at;
    memcpy(in.bytes, (const uint8_t *)input + at, chunk);
    bridge_packets++;
    wr_packet(&server, &bridge_session, &in, &out);
    if (out.version != 1 || out.id != in.id || out.kind != in.kind)
      return 13;
    if (out.error)
      return out.error;
    at += chunk;
    if (out.offset != at)
      return 13;
    total = out.total;
  }
  if (total < sizeof(struct w32r_reply) || total > cap)
    return 13;
  at = 0;
  while (at < total) {
    memset(&in, 0, sizeof(in));
    in.version = 1, in.id = id, in.kind = W32R_READ, in.total = total;
    in.offset = at;
    bridge_packets++;
    wr_packet(&server, &bridge_session, &in, &out);
    if (out.error)
      return out.error;
    uint32_t chunk = total - at > 232 ? 232 : total - at;
    if (out.total != total || out.offset != at + chunk)
      return 13;
    memcpy((uint8_t *)output + at, out.bytes, chunk);
    at += chunk;
  }
  *used = total;
  return 0;
}
static void bridge_attach(uint32_t budget) {
  if (font_service)
    wf_destroy(font_service);
  struct wf_config c = {fonts_dir, install_dir, budget};
  font_service = wf_create(&c);
  CHECK(font_service);
  server.font_call = wf_call;
  server.fonts = font_service;
  CHECK(gdi_host_bridge(bridge_exchange));
}
static void bridge_detach(void) {
  CHECK(gdi_host_bridge(0));
  server.font_call = 0;
  server.fonts = 0;
  wf_destroy(font_service);
  font_service = 0;
  wr_session_close(&bridge_session);
}
/* A raw request with an explicit envelope, for the typed refusals. */
static uint32_t raw(uint32_t op, const char *path, uint32_t index,
                    uint32_t type, const void *data, uint32_t bytes,
                    uint32_t key, struct w32r_reply *r, uint8_t *reply,
                    uint32_t cap) {
  uint8_t request[sizeof(struct w32r_request) + 1024 + 1 + 4 * 80];
  uint32_t path_bytes = (uint32_t)strlen(path) + 1;
  CHECK(path_bytes <= 1024 && bytes <= 4 * 80);
  struct w32r_request a = {W32R_VERSION, op, key, index, type, 0, 0,
                           path_bytes, 1, bytes};
  memcpy(request, &a, sizeof(a));
  memcpy(request + sizeof(a), path, path_bytes);
  request[sizeof(a) + path_bytes] = 0;
  if (bytes)
    memcpy(request + sizeof(a) + path_bytes + 1, data, bytes);
  uint32_t used = 0;
  uint32_t e = bridge_exchange(request, sizeof(a) + path_bytes + 1 + bytes,
                               reply, cap, &used);
  if (e)
    return 0x10000u | e;
  memcpy(r, reply, sizeof(*r));
  CHECK(r->version == W32R_VERSION);
  return r->error;
}
static void face_path(char *out, size_t cap, const char *dir,
                      const char *name) {
  snprintf(out, cap, "%s/%s", dir, name);
}
static void write_file(const char *name, const void *data, size_t n) {
  char path[1024];
  face_path(path, sizeof(path), install_dir, name);
  FILE *f = fopen(path, "wb");
  CHECK(f && fwrite(data, 1, n, f) == n);
  fclose(f);
}
static void be_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8),
  p[3] = (uint8_t)v;
}
static uint64_t micros(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static const struct sample {
  const char *face;
  int height, weight, italic;
  const char *text;
} samples[] = {
    {"MS Sans Serif", -11, 400, 0, "Ciuki OK Cancel &Name: \x80\xe9"},
    {"System", 16, 700, 0, "Control Gallery 0123456789"},
    {"MS Shell Dlg", -11, 400, 0, "Classic computing, one woof at a time."},
    {"Arial", -16, 700, 1, "Wg Bold Italic \x93quoted\x94"},
    {"Times New Roman", -24, 400, 0, "Serif fjord AVA"},
    {"Courier New", -13, 400, 0, "Mono {}[]()<>"},
    {"Arial", -96, 400, 0, "W@"}, /* glyphs above the 4 KiB stack reply */
};
#define SAMPLES (sizeof(samples) / sizeof(*samples))
static uint32_t pixels_native[SAMPLES][320 * 128],
    pixels_bridge[SAMPLES][320 * 128];
static int widths_native[SAMPLES], widths_bridge[SAMPLES];

static void bridge_equality(void) {
  CHECK(gdi_host_backend());
  uint64_t start = micros();
  for (UINT i = 0; i < SAMPLES; i++) {
    const struct sample *s = &samples[i];
    widths_native[i] = gdi_host_render(s->face, s->height, s->weight,
                                       s->italic, s->text, pixels_native[i],
                                       320, 128);
    CHECK(widths_native[i] > 0);
  }
  uint64_t native_us = micros() - start;
  bridge_attach(0);
  UINT packets = bridge_packets, transactions = bridge_transactions;
  start = micros();
  for (UINT i = 0; i < SAMPLES; i++) {
    const struct sample *s = &samples[i];
    widths_bridge[i] = gdi_host_render(s->face, s->height, s->weight,
                                       s->italic, s->text, pixels_bridge[i],
                                       320, 128);
    CHECK(widths_bridge[i] == widths_native[i]);
    UINT ink = 0;
    for (UINT k = 0; k < 320 * 128; k++)
      ink += pixels_bridge[i][k] != 0;
    CHECK(ink > 0);
    CHECK(!memcmp(pixels_bridge[i], pixels_native[i], sizeof(pixels_native[i])));
  }
  uint64_t bridge_us = micros() - start;
  struct w32_gdi_bridge_stats b;
  CHECK(w32_gdi_bridge_statistics(&b));
  CHECK(b.glyphs > 60 && !b.refusals && !b.errors && b.cmap_pages >= 2);
  CHECK(b.service_glyphs >= b.glyphs && b.service_open_faces >= 1);
  CHECK(b.service_face_peak <= b.service_budget &&
        b.service_budget == WF_BUDGET);
  printf("BRIDGE equality samples=%u glyphs=%u cmap_pages=%u transactions=%u "
         "packets=%u packets_per_transaction=%.2f native_us=%llu bridge_us=%llu "
         "service_peak=%u service_budget=%u\n",
         (unsigned)SAMPLES, b.glyphs, b.cmap_pages,
         bridge_transactions - transactions, bridge_packets - packets,
         (double)(bridge_packets - packets) /
             (double)(bridge_transactions - transactions),
         (unsigned long long)native_us, (unsigned long long)bridge_us,
         b.service_face_peak, b.service_budget);
  /* Cache hits: the same text again crosses no glyph request. */
  struct w32_gdi_font_stats f0, f1;
  w32_gdi_font_statistics(&f0);
  transactions = bridge_transactions;
  for (UINT i = 0; i < SAMPLES; i++) {
    const struct sample *s = &samples[i];
    CHECK(gdi_host_render(s->face, s->height, s->weight, s->italic, s->text,
                          pixels_bridge[i], 320, 128) == widths_native[i]);
    CHECK(!memcmp(pixels_bridge[i], pixels_native[i], sizeof(pixels_native[i])));
  }
  w32_gdi_font_statistics(&f1);
  CHECK(bridge_transactions == transactions);
  CHECK(f1.hits > f0.hits && f1.misses == f0.misses);
  printf("BRIDGE cache hits=%u misses=%u transactions=%u\n", f1.hits - f0.hits,
         f1.misses - f0.misses, bridge_transactions - transactions);
  /* One 8 pt glyph miss: one BEGIN and one or two READ fragments. */
  w32_gdi_bridge_statistics(&b);
  UINT before = b.glyphs;
  packets = bridge_packets, transactions = bridge_transactions;
  uint32_t one[320 * 128];
  CHECK(gdi_host_render("MS Sans Serif", -11, 400, 0, "Q", one, 320, 128) > 0);
  w32_gdi_bridge_statistics(&b);
  CHECK(b.glyphs == before + 1);
  /* The statistics call itself is one more transaction (BEGIN + one READ). */
  UINT glyph_packets = bridge_packets - packets - 2;
  CHECK(glyph_packets >= 2 && glyph_packets <= 3);
  printf("BRIDGE miss packets=%u\n", glyph_packets);
  bridge_detach();
}
/* LRU faces inside the service budget, and a face larger than the budget. */
static void bridge_budget(void) {
  bridge_attach(700u << 10);
  uint32_t px[320 * 128];
  static const char *const faces[] = {"Arial", "Times New Roman",
                                      "Courier New", "Arial"};
  /* The last text needs new glyphs, so the service reopens the first face
   * (the process still has "Budget" cached). */
  for (UINT i = 0; i < 4; i++)
    CHECK(gdi_host_render(faces[i], -13, 400, 0, i < 3 ? "Budget" : "Reopened",
                          px, 320, 128) > 0);
  struct w32_gdi_bridge_stats b;
  CHECK(w32_gdi_bridge_statistics(&b));
  CHECK(b.service_budget == 700u << 10 && b.service_face_peak <= 700u << 10);
  CHECK(b.service_open_faces == 1 && b.service_evictions >= 3 &&
        b.service_opens >= 4 && !b.refusals);
  printf("BRIDGE budget service_peak=%u budget=%u opens=%u evictions=%u\n",
         b.service_face_peak, b.service_budget, b.service_opens,
         b.service_evictions);
  /* Liberation Sans (410,712 bytes + 128 KiB) cannot fit 400 KiB: typed
   * refusal 8, then metric-only text (extents, no coverage). */
  bridge_attach(400u << 10);
  int width = gdi_host_render("Arial", -13, 400, 0, "Refused", px, 320, 128);
  CHECK(width > 0);
  for (UINT k = 0; k < 320 * 128; k++)
    CHECK(!px[k]);
  CHECK(w32_gdi_bridge_statistics(&b));
  CHECK(b.refusals >= 1 && b.last_error == 8 && b.service_refusals >= 1 &&
        !b.service_open_faces);
  bridge_detach();
  /* The per-process selection layer stays inside the 2 MiB envelope over
   * many logical fonts through the bridge (glyph and face evictions). */
  bridge_attach(0);
  static const char *const families[] = {"Arial", "Times New Roman",
                                         "Courier New"};
  struct w32_gdi_font_stats f0, f1;
  struct w32_gdi_bridge_stats b0;
  w32_gdi_font_statistics(&f0);
  CHECK(w32_gdi_bridge_statistics(&b0));
  for (UINT k = 0; k < 3; k++)
    for (int size = 8; size <= 36; size += 2)
      CHECK(gdi_host_render(families[k], -size, k == 1 ? 700 : 400, k == 2,
                            "The quick brown fox jumps over 13 lazy dogs!",
                            px, 320, 128) > 0);
  w32_gdi_font_statistics(&f1);
  CHECK(w32_gdi_bridge_statistics(&b));
  CHECK(f1.total_peak <= 2u * 1024u * 1024u && f1.evictions > f0.evictions);
  CHECK(b.service_face_peak <= b.service_budget && b.errors == b0.errors &&
        b.refusals == b0.refusals);
  printf("BRIDGE envelope fonts=45 total_peak=%u glyph_peak=%u evictions=%u "
         "service_peak=%u service_evictions=%u\n",
         f1.total_peak, f1.glyph_peak, f1.evictions - f0.evictions,
         b.service_face_peak, b.service_evictions);
  bridge_detach();
}
static void bridge_refusals(void) {
  bridge_attach(0);
  struct w32r_reply r;
  static uint8_t reply[W32R_DATA + 64];
  char sans[1024], path[1024];
  face_path(sans, sizeof(sans), fonts_dir, "LiberationSans-Regular.ttf");
  uint32_t glyph = 36;
  /* Sizes outside 1..128 px em: 50 (ERROR_NOT_SUPPORTED). */
  CHECK(raw(W32R_FONT_GLYPHS, sans, 0, 1, &glyph, 4, 0, &r, reply, sizeof(reply)) == 50);
  CHECK(raw(W32R_FONT_GLYPHS, sans, 63, 1, &glyph, 4, 0, &r, reply, sizeof(reply)) == 50);
  CHECK(raw(W32R_FONT_GLYPHS, sans, 8193, 1, &glyph, 4, 0, &r, reply, sizeof(reply)) == 50);
  CHECK(raw(W32R_FONT_GLYPHS, sans, 8192, 1, &glyph, 4, 0, &r, reply, sizeof(reply)) == 0 &&
        r.count == 1);
  /* Paths outside the two stores or not a plain .ttf name: 5. */
  CHECK(raw(W32R_FONT_CMAP, "/etc/hostname", 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 5);
  face_path(path, sizeof(path), fonts_dir, "../liberation/LiberationSans-Regular.ttf");
  CHECK(raw(W32R_FONT_CMAP, path, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 5);
  face_path(path, sizeof(path), fonts_dir, "LICENSE");
  CHECK(raw(W32R_FONT_CMAP, path, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 5);
  face_path(path, sizeof(path), fonts_dir, ".hidden.ttf");
  CHECK(raw(W32R_FONT_CMAP, path, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 5);
  face_path(path, sizeof(path), fonts_dir, "Missing.ttf");
  CHECK(raw(W32R_FONT_CMAP, path, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 2);
  /* Installed files that are not TrueType store faces: 13 (W21). */
  uint8_t bad[64] = {0};
  memcpy(bad, "OTTO", 4);
  bad[5] = 1;
  write_file("cff.ttf", bad, sizeof(bad));
  memcpy(bad, "ttcf", 4);
  write_file("collection.ttf", bad, sizeof(bad));
  be_put32(bad, 0x00010000u);
  bad[4] = 0, bad[5] = 20; /* 20 tables in 64 bytes */
  write_file("truncated.ttf", bad, sizeof(bad));
  bad[5] = 1;
  memcpy(bad + 12, "head", 4);
  be_put32(bad + 20, 40), be_put32(bad + 24, 54); /* table past the end */
  write_file("overlong.ttf", bad, sizeof(bad));
  static const char *const refused[] = {"cff.ttf", "collection.ttf",
                                        "truncated.ttf", "overlong.ttf"};
  for (UINT i = 0; i < 4; i++) {
    face_path(path, sizeof(path), install_dir, refused[i]);
    CHECK(raw(W32R_FONT_CMAP, path, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 13);
    /* The refusal is remembered: no second parse. */
    CHECK(raw(W32R_FONT_CMAP, path, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 13);
  }
  /* A real face copied into the install store is served. */
  char copy[1024];
  face_path(copy, sizeof(copy), install_dir, "Installed Face.ttf");
  CHECK(copy_host(sans, copy, 0));
  CHECK(raw(W32R_FONT_CMAP, copy, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 0 &&
        r.data_bytes == 512 && r.key > 400000);
  /* RemoveFontResource then AddFontResource under the same name: the
   * service notices the replaced file, both ways. */
  face_path(path, sizeof(path), install_dir, "cff.ttf");
  CHECK(copy_host(sans, path, 0));
  CHECK(raw(W32R_FONT_CMAP, path, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 0);
  memcpy(bad, "OTTO", 4);
  write_file("Installed Face.ttf", bad, sizeof(bad));
  CHECK(raw(W32R_FONT_CMAP, copy, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 13);
  /* Envelope errors: 87. */
  uint32_t many[80];
  for (UINT i = 0; i < 80; i++)
    many[i] = 36 + i;
  CHECK(raw(W32R_FONT_GLYPHS, sans, 768, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 87);
  CHECK(raw(W32R_FONT_GLYPHS, sans, 768, 65, many, 4 * 65, 0, &r, reply, sizeof(reply)) == 87);
  CHECK(raw(W32R_FONT_GLYPHS, sans, 768, 2, many, 4, 0, &r, reply, sizeof(reply)) == 87);
  CHECK(raw(W32R_FONT_GLYPHS, sans, 768, 1, many, 4, 7, &r, reply, sizeof(reply)) == 87);
  uint32_t beyond = 0xffffu;
  CHECK(raw(W32R_FONT_GLYPHS, sans, 768, 1, &beyond, 4, 0, &r, reply, sizeof(reply)) == 87);
  CHECK(raw(W32R_FONT_CMAP, sans, 17, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 87);
  CHECK(raw(W32R_FONT_CMAP, sans, 0x110000u, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 87);
  CHECK(raw(W32R_FONT_STATS, sans, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 87);
  /* Batches: 64 glyphs fit at 12 px; at 128 px the reply stops at
   * W32R_DATA with fewer records, each complete. */
  CHECK(raw(W32R_FONT_GLYPHS, sans, 768, 64, many, 4 * 64, 0, &r, reply, sizeof(reply)) == 0 &&
        r.count == 64 && r.data_bytes <= W32R_DATA);
  CHECK(raw(W32R_FONT_GLYPHS, sans, 8192, 64, many, 4 * 64, 0, &r, reply, sizeof(reply)) == 0 &&
        r.count > 0 && r.count < 64 && r.data_bytes <= W32R_DATA);
  uint32_t at = sizeof(r);
  for (UINT i = 0; i < r.count; i++) {
    struct w32r_glyph g;
    memcpy(&g, reply + at, sizeof(g));
    CHECK(g.glyph == many[i] && g.top > 0 && g.advance > 0);
    at += sizeof(g) + (uint32_t)g.width * g.height;
  }
  CHECK(at == sizeof(r) + r.data_bytes);
  printf("BRIDGE batch at 128px returned=%u bytes=%u\n", r.count, r.data_bytes);
  /* The reply's top is the Win32 distance above the baseline: 'H' at 12 px
   * rises 9 rows and sits on the baseline (cf. the gdi32 baseline test). */
  uint8_t cmap[sizeof(r) + 512];
  CHECK(raw(W32R_FONT_CMAP, sans, 0, 0, 0, 0, 0, &r, cmap, sizeof(cmap)) == 0);
  uint32_t h = (uint32_t)(cmap[sizeof(r) + 2 * 'H'] | cmap[sizeof(r) + 2 * 'H' + 1] << 8);
  CHECK(raw(W32R_FONT_GLYPHS, sans, 768, 1, &h, 4, 0, &r, reply, sizeof(reply)) == 0);
  struct w32r_glyph g;
  memcpy(&g, reply + sizeof(r), sizeof(g));
  CHECK(g.top == 9 && g.height == 9);
  /* The client refuses replies larger than its buffer (transport 13). */
  CHECK(raw(W32R_FONT_GLYPHS, sans, 768, 64, many, 4 * 64, 0, &r, reply, 600) == (0x10000u | 13));
  struct w32_gdi_bridge_stats b;
  CHECK(w32_gdi_bridge_statistics(&b));
  CHECK(b.service_refusals >= 8);
  printf("BRIDGE refusals service=%u\n", b.service_refusals);
  /* Without the engine (font_call NULL): 120, metric-only text, and the
   * client retries a face only every 256 glyph requests. */
  server.font_call = 0;
  CHECK(raw(W32R_FONT_CMAP, sans, 0, 0, 0, 0, 0, &r, reply, sizeof(reply)) == 120);
  SetLastError(0);
  CHECK(!w32_gdi_bridge_statistics(&b) && GetLastError() == 120 &&
        b.last_error == 120);
  puts("BRIDGE unavailable typed_failure=120 GetLastError=120");
  CHECK(gdi_host_bridge(bridge_exchange)); /* resets faces and caches */
  UINT transactions = bridge_transactions;
  uint32_t px[320 * 128];
  for (UINT i = 0; i < 40; i++)
    CHECK(gdi_host_render("Arial", -12, 400, 0, "Unavailable engine", px, 320,
                          128) > 0);
  for (UINT k = 0; k < 320 * 128; k++)
    CHECK(!px[k]);
  CHECK(w32_gdi_bridge_statistics(&b) == 0 && b.errors >= 1 &&
        b.last_error == 120);
  /* 40 renders of 18 glyphs (TextOut plus extents): about 1,500 face
   * requests, one open attempt per 256 of them instead of one per glyph. */
  UINT attempts = bridge_transactions - transactions - 1;
  CHECK(attempts >= 2 && attempts <= 16);
  printf("BRIDGE unavailable renders=40 glyphs=720 open_attempts=%u\n", attempts);
  server.font_call = wf_call;
  bridge_detach();
}
static void test_bridge(void) {
  bridge_equality();
  bridge_budget();
  bridge_refusals();
}
static void frames_bridge(void) {
  bridge_attach(0);
  test_frames();
  struct w32_gdi_bridge_stats b;
  CHECK(w32_gdi_bridge_statistics(&b) && b.glyphs > 100 && !b.refusals &&
        !b.errors);
  printf("BRIDGE frames glyphs=%u service_peak=%u\n", b.glyphs,
         b.service_face_peak);
  bridge_detach();
}

/* ---- w32-dialogs fixture dry-run ------------------------------------------ */
static void exercise_pump(HWND dialog, int settle_flag) {
  (void)settle_flag;
  drain(dialog);
}
static void emit_record(const char *probe, const char *event, const char *fmt,
                        ...) {
  char line[240];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  printf("RECORD %s %s %s\n", probe, event, line);
}
static void test_exercise(void) {
  bridge_attach(0);
  struct w32_dialogs_result r = {.magic = W32_DIALOGS_MAGIC,
                                 .version = W32_DIALOGS_VERSION};
  const struct w32_dialogs_env env = {FIXTURE, exercise_pump};
  w32_dialogs_run(&env, &r);
  r.done = 1;
  CHECK(w32_dialogs_valid(&r));
  w32_dialogs_publish(&r, r.errors ? 1u : 0u, emit_record);
  printf("EXERCISE errors=%u first_error=%u steps=%u\n", r.errors,
         r.first_error, r.steps);
  bridge_detach();
}

static void test_exercise_pe(void) {
  const char *path = getenv("CIUKI_PE_RESOURCES");
  CHECK(path);
  FILE *f = fopen(path, "rb");
  CHECK(f);
  uint8_t sizes[8];
  CHECK(fread(sizes, 1, sizeof(sizes), f) == sizeof(sizes));
  for (unsigned i = 0; i < 2; i++) {
    pe_resources[i].id = 600 + i;
    pe_resources[i].size = w32_u32(sizes + 4 * i);
    CHECK(pe_resources[i].size && pe_resources[i].size < 65536);
    pe_resources[i].data = malloc(pe_resources[i].size);
    CHECK(pe_resources[i].data);
    CHECK(fread(pe_resources[i].data, 1, pe_resources[i].size, f) == pe_resources[i].size);
  }
  CHECK(fgetc(f) == EOF && !fclose(f));
  test_exercise();
  for (unsigned i = 0; i < 2; i++) {
    free(pe_resources[i].data);
    pe_resources[i].data = 0;
  }
}
static void test_exercise_missing_resource(void) {
  bridge_attach(0);
  refuse_dialog_resource = 1;
  struct w32_dialogs_result r = {.magic = W32_DIALOGS_MAGIC,
                                 .version = W32_DIALOGS_VERSION};
  const struct w32_dialogs_env env = {FIXTURE, exercise_pump};
  w32_dialogs_run(&env, &r);
  CHECK(r.errors && r.first_error && !r.live_windows && !r.live_menus);
  printf("DIAGNOSTIC first_error=%u\n", r.first_error);
  refuse_dialog_resource = 0;
  bridge_detach();
}

/* Platform adapters for the unchanged guest entry. Full VirtualAlloc and
 * GetModuleHandleA implementations are covered in kernel_loader_main.c. */
extern void W32API dialogs_guest_entry(void);
static jmp_buf entry_return;
static struct w32_dialogs_result *entry_result;
static UINT entry_sleeps, entry_modules;
void *W32API dialogs_entry_alloc(void *at, DWORD bytes, DWORD flags, DWORD prot) {
  CHECK(at == (void *)(uintptr_t)W32_DIALOGS_RESULT_VA && bytes == 4096 &&
        flags == 0x3000 && prot == 4);
  void *p = mmap(at, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  CHECK(p == at);
  entry_result = p;
  return p;
}
void *W32API dialogs_entry_module(const char *name) {
  CHECK(!name && !bridge_unready && entry_sleeps == 3);
  entry_modules++;
  return FIXTURE;
}
void W32API dialogs_entry_sleep(DWORD ms) {
  CHECK(entry_result && entry_result->magic == W32_DIALOGS_MAGIC &&
        entry_result->version == W32_DIALOGS_VERSION);
  entry_sleeps++;
  if (entry_result->done) {
    CHECK(w32_dialogs_valid(entry_result) && !entry_result->errors &&
          !entry_result->teardown_errors && !entry_result->live_windows &&
          !entry_result->live_menus);
    __atomic_store_n(&entry_result->release, 1u, __ATOMIC_RELEASE);
  }
  ui_kernel_pause(ms);
}
W32_NORETURN void W32API dialogs_entry_exit(DWORD code) {
  CHECK(!code && entry_modules == 1 && w32_dialogs_valid(entry_result) &&
        entry_result->release);
  longjmp(entry_return, 1);
}
static void test_exercise_entry(void) {
  bridge_attach(0);
  bridge_unready = 3;
  if (!setjmp(entry_return)) dialogs_guest_entry();
  w32_dialogs_publish(entry_result, 0, emit_record);
  printf("ENTRY readiness_failures=3 modules=%u done=%u release=%u errors=%u\n",
         entry_modules, entry_result->done, entry_result->release, entry_result->errors);
  CHECK(!munmap(entry_result, 4096));
  entry_result = 0;
  bridge_detach();
}
