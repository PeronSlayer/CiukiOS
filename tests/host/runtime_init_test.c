/* Fake boot calls production activation/safe evidence; no hardware access.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/init.h>
#include <ciuki/fbdev.h>
#include <ciuki/i8042.h>
#include <ciuki/biosvm.h>
#include <ciuki/fwinput.h>
#include <ciuki/registry.h>
#include <ciuki/ata.h>
#include <ciuki/probe.h>

static unsigned failures, calls, records, consoles, optional_records;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static char order[16], provenance[256];
static uint32_t expected_flags;
static int fb_result, input_result, bios_result, adapter_result, ata_result;
static bool disk, quarantined, fail_expected;
static struct fb_device display;
struct ciuki_boot_info g_boot;

static void called(char id)
{
    CHECK(g_boot.flags == expected_flags && calls + 1 < sizeof(order));
    order[calls++] = id;
    order[calls] = 0;
}
int fbdev_init(void) { called('F'); display.present = !fb_result && !!g_boot.fb_phys; return fb_result; }
const struct fb_device *fbdev_get(void) { return &display; }
int i8042_init(void) { called('N'); return input_result; }
int biosvm_init(void) { called('B'); return bios_result; }
int fwinput_adapter_init(void) { called('W'); return adapter_result; }
int ata_init(void) { called('A'); return ata_result; }
struct ata_device *ata_device_get(unsigned c, unsigned u)
{
    static struct ata_device d;
    return disk && !c && !u ? &d : 0;
}
unsigned registry_count(void) { return quarantined ? 1 : 0; }
const struct resource *registry_get(unsigned i)
{
    static struct resource r = { .owner = "ata0", .state = RS_QUARANTINED };
    CHECK(!i);
    return &r;
}
void i8042_snapshot(struct i8042_stats *out)
{
    *out = (struct i8042_stats){ .active = !input_result && !bios_result && !adapter_result };
}
void fwinput_backend_state(struct fwinput_backend_state *out)
{
    *out = (struct fwinput_backend_state){ .keyboard = true, .mouse = true, .key_releases = true };
}
void console_write(const char *s, size_t n) { CHECK(s && n); consoles++; }
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[512] = { 0 };
    va_list ap;
    va_start(ap, fmt);
    if (fmt) vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    size_t length = 55 + strlen(probe) + strlen(event) + strlen(extra);
    CHECK(length <= 240);
    if (length > 240) printf("oversized %s %s %s\n", probe, event, extra);
    if (strstr(extra, "case=option")) snprintf(provenance, sizeof(provenance), "%s", extra);
    if (!strcmp(probe, "boot") && strstr(extra, "group=activation device=")) optional_records++;
    if (!strcmp(event, "END")) CHECK(!!strstr(extra, "status=FAIL") == fail_expected);
    records++;
}

#include "../../src/kernel/core/init.c"
#include "../../src/kernel/probes/safe_probe.c"

static void reset(uint32_t flags, bool firmware, bool lfb)
{
    state = (struct drivers_state){ 0 };
    g_boot = (struct ciuki_boot_info){ .flags = flags, .input_policy = firmware ? CBI_INPUT_FIRMWARE : CBI_INPUT_NATIVE,
                                     .fb_phys = lfb ? 0xE0000000u : 0 };
    expected_flags = flags;
    fb_result = input_result = bios_result = adapter_result = ata_result = 0;
    disk = quarantined = fail_expected = false;
    calls = optional_records = consoles = 0;
    order[0] = provenance[0] = 0;
    display = (struct fb_device){ 0 };
}
static void verify(const char *expected, unsigned optional)
{
    drivers_init();
    CHECK(!strcmp(order, expected));
    struct drivers_state s;
    drivers_snapshot(&s);
    CHECK(s.initialized && s.boot_flags == expected_flags && s.optional_activations == optional);
    CHECK(s.flag_sequence < s.fb_sequence && s.fb_sequence < s.input_sequence && s.input_sequence < s.ata_sequence);
    CHECK(optional_records == 3);
    unsigned n = calls, r = records;
    drivers_init();
    CHECK(calls == n && records == r); /* failures also get only one attempt */
}
int main(void)
{
    reset(0, false, true); disk = true; verify("FNA", 2);
    reset(CBI_F_TEXT_MODE, false, false); verify("FNA", 2);
    reset(0, false, true); fb_result = -EINVAL; input_result = -5; ata_result = -5; verify("FNA", 2);
    CHECK(state.fb_error == -EINVAL && state.input_error == -5 && state.ata_error == -5);
    reset(0, true, true); verify("FBWA", 2);
    reset(0, true, true); bios_result = -5; verify("FBA", 2);
    reset(CBI_F_SAFE_MODE, false, true); verify("FN", 0); CHECK(!probe_safe() && consoles == 1);
    CHECK(strstr(provenance, "menu=1 menu_inferred=1"));
    reset(CBI_F_SAFE_MODE | CBI_F_TEXT_MODE, false, false);
    memcpy(g_boot.options, "video=640x480\nsafe=1\n", sizeof("video=640x480\nsafe=1\n"));
    verify("N", 0); CHECK(!probe_safe() && consoles == 1 && strstr(provenance, "boot_cfg=1 menu=0"));
    reset(CBI_F_SAFE_MODE | CBI_F_TEXT_MODE | CBI_F_SMBIOS_QEMU | CBI_F_TEST_REQUEST | CBI_F_INPUT_FORCED,
          true, false);
    const char *selector = "f1:safe run=12ab34cd platform=e500 safe=1";
    g_boot.test_request_len = (uint16_t)strlen(selector);
    memcpy(g_boot.test_request, selector, g_boot.test_request_len);
    verify("BW", 0); CHECK(!probe_safe() && strstr(provenance, "fw_cfg=1"));
    reset(CBI_F_SAFE_MODE, false, false); verify("N", 0); CHECK(!probe_safe()); /* no LFB/no storage */
    reset(CBI_F_SAFE_MODE, false, false); input_result = -5; fail_expected = true;
    verify("N", 0); CHECK(probe_safe() == 1);
    CHECK(!safe_option("nosafe=1", 8) && !safe_option("safe=10", 7) && safe_option("safe=1", 6));
    printf("runtime init/safe: %s (%u failures, %u records; ordering, flags, gating, fallbacks, failures)\n",
           failures ? "FAIL" : "PASS", failures, records);
    return failures ? 1 : 0;
}
