/* Host regression harness for the production page adapter. The JS boundary is
   stubbed so this test isolates HTML ordering, bounds and TLV application. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define WPAGE_HOST_TEST 1
#include "../../src/web/worker_page.h"
#include "../../src/web/worker_js.h"

static char order[32];
static unsigned order_count;

void wjs_reset(void) { }
int wjs_set_element(const char *id, uint32_t id_bytes,
                    const char *text, uint32_t text_bytes)
{
    (void)id; (void)id_bytes; (void)text; (void)text_bytes;
    return WJS_OK;
}
void wjs_set_cancel_flag(volatile const uint32_t *flag, uint32_t expected)
{ (void)flag; (void)expected; }

static void put16(uint8_t *p, uint16_t value)
{ p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }

int wjs_eval(const char *source, uint32_t source_bytes,
             uint8_t *output, uint32_t output_capacity,
             uint32_t *output_bytes)
{
    const char *payload = NULL, *target;
    uint16_t op = 0, idn = 0, textn = 0;
    *output_bytes = 0;
    if (source_bytes >= 6 && !memcmp(source, "TEXT=", 5)) {
        op = WJS_TLV_SET_TEXT; target = source + 5;
        payload = memchr(target, ':', source_bytes - 5);
        if (!payload) return WJS_E_SCRIPT;
        idn = (uint16_t)(payload - target); ++payload;
        textn = source_bytes - (uint32_t)(payload - source);
        memcpy(order + order_count++, "T", 1);
    } else if (source_bytes >= 6 && !memcmp(source, "HTML=", 5)) {
        op = WJS_TLV_SET_HTML; target = source + 5;
        payload = memchr(target, ':', source_bytes - 5);
        if (!payload) return WJS_E_SCRIPT;
        idn = (uint16_t)(payload - target); ++payload;
        textn = source_bytes - (uint32_t)(payload - source);
        memcpy(order + order_count++, "H", 1);
    } else if (source_bytes >= 6 && !memcmp(source, "WRITE=", 6)) {
        op = WJS_TLV_WRITE; payload = source + 6; textn = source_bytes - 6;
        memcpy(order + order_count++, "W", 1);
    } else if (source_bytes >= 4 && !memcmp(source, "FAIL", 4)) {
        memcpy(order + order_count++, "F", 1);
        return WJS_E_SCRIPT;
    } else {
        return WJS_OK;
    }
    if (WJS_TLV_HEADER_BYTES + idn + textn > output_capacity) return WJS_E_LIMIT;
    put16(output, op); put16(output + 2, idn); put16(output + 4, textn);
    if (idn) memcpy(output + WJS_TLV_HEADER_BYTES, target, idn);
    if (textn) memcpy(output + WJS_TLV_HEADER_BYTES + idn, payload, textn);
    *output_bytes = WJS_TLV_HEADER_BYTES + idn + textn;
    return WJS_OK;
}

const char *wjs_last_error(void)
{
    return "mock error";
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "worker_page: FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    fclose(f);
    return ok;
}

static int contains(const char *path, const char *needle)
{
    char data[4096]; size_t n; FILE *f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(data, 1, sizeof(data)-1, f); fclose(f); data[n] = 0;
    return strstr(data, needle) != NULL;
}

int main(void)
{
    static const char page[] =
        "<html><body><div id='title'>old</div><div id='other'>before</div>"
        "<script>WRITE=first</script>"
        "<script src='extra.js'></script>"
        "<script>TEXT=title:from-inline</script>"
        "<script>FAIL</script><p>tail</p></body></html>";
    static const char parser_edges[] =
        "<html><body><!-- <script>FAIL</script><div id='ghost'>x</div> -->"
        "<style>p:after{content:\"<script>FAIL</script><div id='fake'>\"}</style>"
        "<script type='application/ld+json'>FAILJSON</script>"
        "<div id='outer'><span><div id='inner'>inner</div></span></div>"
        "<script>TEXT=outer:Updated</script><script>WRITE=post</script>"
        "</body></html>";
    uint8_t listing[1024]; uint32_t listing_bytes; char warning[128];
    const char *page_path = "/tmp/worker-page-fixture.htm";
    const char *script_path = "/tmp/worker-page-extra.js";
    const char *out_path = "/tmp/worker-page-render.htm";
    const char *edge_path = "/tmp/worker-page-edges.htm";
    uint16_t count;
    CHECK(write_file(page_path, page));
    CHECK(write_file(script_path, "HTML=other:<b>external</b>"));
    CHECK(wpage_scan(page_path, listing, sizeof(listing), &listing_bytes) == WPAGE_OK);
    CHECK(listing_bytes > 2);
    count = (uint16_t)(listing[0] | ((uint16_t)listing[1] << 8));
    CHECK(count == 1);
    CHECK(wpage_resource(0, script_path) == WPAGE_OK);
    CHECK(wpage_run(out_path, warning, sizeof(warning)) == WPAGE_OK);
    CHECK(warning[0] != 0); /* Failed script is a warning, page still renders. */
    CHECK(order_count == 4 && !memcmp(order, "WHTF", 4));
    CHECK(contains(out_path, "</script>first"));
    CHECK(contains(out_path, "<div id='other'><b>external</b></div>"));
    CHECK(contains(out_path, "<div id='title'>from-inline</div>"));
    CHECK(contains(out_path, "<p>tail</p>"));

    order_count = 0;
    CHECK(write_file(page_path, parser_edges));
    CHECK(wpage_scan(page_path, listing, sizeof(listing), &listing_bytes) == WPAGE_OK);
    CHECK((listing[0] | ((uint16_t)listing[1] << 8)) == 0);
    CHECK(wpage_run(edge_path, warning, sizeof(warning)) == WPAGE_OK);
    CHECK(warning[0] == 0);
    CHECK(order_count == 2 && !memcmp(order, "TW", 2));
    CHECK(contains(edge_path, "<div id='outer'>Updated</div>"));
    CHECK(contains(edge_path, "</script>post"));
    CHECK(contains(edge_path, "id='ghost'")); /* Still inert comment text. */
    CHECK(contains(edge_path, "id='fake'"));  /* Still inert style text. */
    puts("[WORKER_PAGE] PASS");
    remove(page_path); remove(script_path); remove(out_path); remove(edge_path);
    return 0;
}
