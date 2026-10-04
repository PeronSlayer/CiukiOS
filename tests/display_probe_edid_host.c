/* Focused host regression checks for the target EDID parser. */
#include <stdio.h>
#include <string.h>
#include "../src/apps/display_probe.h"

void mem_set(void *dst, int value, int bytes)
{
    memset(dst, value, (size_t)bytes);
}

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); ++failures; \
} } while (0)

static void checksum(u8 *e)
{
    unsigned i, sum = 0;
    e[127] = 0;
    for (i = 0; i < 127; ++i) sum = (sum + e[i]) & 255;
    e[127] = (u8)(0 - sum);
}

static void make_edid(u8 *e, unsigned hz)
{
    static const u8 header[8] = {0, 255, 255, 255, 255, 255, 255, 0};
    static const char name[] = "Ciuki Display";
    unsigned clock = (2475U * hz) / 10; /* 2200 x 1125 pixels, in 10 kHz units */
    memset(e, 0, 128);
    memcpy(e, header, sizeof header);
    e[8] = 0x4D; e[9] = 0xD9;        /* PNP manufacturer: SNY */
    e[10] = 0x34; e[11] = 0x12;
    e[18] = 1; e[19] = 4; e[20] = 0x80;
    e[21] = 60; e[22] = 34;
    e[24] = 2;                       /* first detailed timing is preferred */
    e[54] = (u8)clock; e[55] = (u8)(clock >> 8);
    e[56] = 0x80; e[57] = 0x18; e[58] = 0x71; /* 1920 + 280 */
    e[59] = 0x38; e[60] = 0x2D; e[61] = 0x40; /* 1080 + 45 */
    e[71] = 0x1E;                    /* progressive, digital separate sync */
    e[72] = 0; e[73] = 0; e[74] = 0; e[75] = 0xFC; e[76] = 0;
    memcpy(e + 77, name, 13); /* exact 13-byte descriptor payload */
    checksum(e);
}

int main(void)
{
    u8 edid[128];
    struct disp_monitor m;

    make_edid(edid, 120);
    CHECK(disp_probe_parse_edid(edid, 128, &m) == 1);
    CHECK(m.valid == 1 && m.input_digital == 1);
    CHECK(strcmp(m.manufacturer, "SNY") == 0 && m.product == 0x1234);
    CHECK(m.preferred_width == 1920 && m.preferred_height == 1080);
    CHECK(m.preferred_millihz == 120000UL);
    CHECK(strcmp(m.name, "Ciuki Display") == 0);
    CHECK(m.name[DISP_NAME_BYTES - 1] == 0);

    make_edid(edid, 144);
    CHECK(disp_probe_parse_edid(edid, 128, &m) == 1);
    CHECK(m.preferred_millihz == 144000UL); /* catches 16-bit truncation */

    make_edid(edid, 120);
    edid[71] |= 0x80; checksum(edid);
    CHECK(disp_probe_parse_edid(edid, 128, &m) == 1);
    CHECK(m.valid == 1 && m.preferred_width == 0 && m.preferred_millihz == 0);

    make_edid(edid, 120);
    edid[0] = 1; checksum(edid);
    CHECK(disp_probe_parse_edid(edid, 128, &m) == 0);
    CHECK(m.valid == 0);
    make_edid(edid, 120);
    edid[30] ^= 1;
    CHECK(disp_probe_parse_edid(edid, 128, &m) == 0); /* bad checksum */
    CHECK(m.valid == 0);
    make_edid(edid, 120);
    CHECK(disp_probe_parse_edid(edid, 127, &m) == 0); /* truncated block */
    CHECK(m.valid == 0);

    if (failures) return 1;
    puts("display EDID parser: all checks passed");
    return 0;
}
