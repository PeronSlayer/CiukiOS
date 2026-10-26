/* G14 boot-status sector and Automatic Skip Driver agent (core/bootstat.c)
 * on an in-memory device: adoption, refusal of foreign data, the Windows 98
 * two-failure rule, completion after the first frame, persistence order.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/bootstat.h>
#include <ciuki/boot_info.h>
#include <ciuki/abi.h>
#include <ciuki/blackbox.h>
#include "../../src/kernel/fs/blkdev.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

struct ciuki_boot_info g_boot;
unsigned fs_host_page_fail, fs_host_contig_fail, fs_host_contig_live;
static unsigned failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define START 2048u
static uint8_t disk[(START + 200) * 512];
static unsigned writes, flushes, fail_writes, fail_flushes, checking_bind;
static char logs[8192];
void klog(const char *f, ...)
{
    va_list a; va_start(a, f);
    size_t n = strlen(logs);
    vsnprintf(logs + n, sizeof(logs) - n, f, a);
    va_end(a);
    strncat(logs, "\n", sizeof(logs) - strlen(logs) - 1);
}
static int rd(struct blkdev *d, uint64_t lba, uint32_t n, void *b)
{
    if (blkdev_range(d, lba, n)) return -FS_EINVAL;
    memcpy(b, disk + lba * 512, n * 512); return 0;
}
static int wr(struct blkdev *d, uint64_t lba, uint32_t n, const void *b)
{
    if (checking_bind) CHECK(!bootstat_bound());
    if (blkdev_range(d, lba, n)) return -FS_EINVAL;
    if (fail_writes) { fail_writes--; return -FS_EIO; }
    memcpy(disk + lba * 512, b, n * 512); writes++; return 0;
}
static int fl(struct blkdev *d) { (void)d; if (checking_bind) CHECK(!bootstat_bound()); flushes++; if (fail_flushes) { fail_flushes--; return -FS_EIO; } return 0; }
static struct blkdev dev = { .read = rd, .write = wr, .flush = fl, .capacity = START + 200,
                             .sector_size = 512, .write_cache_state = BLKDEV_CACHE_ENABLED };

static uint8_t *sector9(void) { return disk + (START + BOOTSTAT_SECTOR) * 512; }
static void volume(bool descriptor)
{
    memset(disk, 0, sizeof(disk));
    if (descriptor) {
        uint8_t *d = disk + (START + BLACKBOX_DESCRIPTOR) * 512;
        memcpy(d, "CBB1", 4); bootstat_wr32(d + 4, 1); bootstat_wr32(d + 8, BLACKBOX_FIRST);
        bootstat_wr32(d + 12, BLACKBOX_SECTORS);
    }
}
static struct bootstat_record disk_record(void)
{
    struct bootstat_record r; memset(&r, 0, sizeof(r));
    CHECK(bootstat_decode(sector9(), &r));
    return r;
}
/* One start: loader (state 1, CBI_F_BOOT_FAILED if it was 1) then bind. */
static int boot(uint32_t extra_flags)
{
    struct bootstat_record r;
    uint32_t flags = extra_flags;
    if (bootstat_decode(sector9(), &r) && r.state) flags |= CBI_F_BOOT_FAILED;
    if (bootstat_decode(sector9(), &r)) flags |= (uint32_t)((r.disabled | r.skipped) & 0x7f) << CBI_F_SKIP_SHIFT;
    if (!(flags & CBI_F_TEST_REQUEST) && bootstat_decode(sector9(), &r)) sector9()[BOOTSTAT_OFF_STATE] = 1;
    g_boot.flags = flags;
    logs[0] = 0;
    checking_bind=1;
    int e=bootstat_bind(&dev, START, 200);
    checking_bind=0;
    return e;
}

static void test_adoption_and_refusals(void)
{
    volume(false);
    CHECK(boot(0) == -FS_ENOENT && !bootstat_bound());          /* zero sector, no black box */
    volume(true);
    sector9()[100] = 1;
    CHECK(boot(0) == -FS_ENOENT && !bootstat_bound());          /* foreign data is never overwritten */
    CHECK(sector9()[100] == 1 && writes == 0);
    volume(true);
    CHECK(boot(CBI_F_TEST_REQUEST) == -FS_EOPNOTSUPP && !bootstat_bound());
    CHECK(boot(0) == 0 && bootstat_bound());
    struct bootstat_record r = disk_record();
    CHECK(r.state == 1 && r.boots == 1 && r.failures == 0 && strstr(logs, "adopt=1"));
    CHECK(writes == 1 && flushes == 1);
    /* A bad CRC is foreign. */
    sector9()[20] ^= 1;
    CHECK(boot(0) == -FS_ENOENT);
    sector9()[20] ^= 1;
    struct blkdev ro = dev; ro.flush = 0;
    g_boot.flags = 0;
    CHECK(bootstat_bind(&ro, START, 200) == -FS_EROFS);
}

static void test_two_failures_skip_a_class(void)
{
    volume(true);
    CHECK(boot(0) == 0);
    bootstat_begin(BOOTSTAT_SOUND);                   /* the start dies inside audio_init */
    CHECK(disk_record().active == 1u << BOOTSTAT_SOUND);
    CHECK(boot(0) == 0);                              /* failure 1 */
    struct bootstat_record r = disk_record();
    CHECK(r.failures == 1 && r.fail_count[BOOTSTAT_SOUND] == 1 && !r.skipped && r.blamed == 1u << BOOTSTAT_SOUND);
    CHECK(!bootstat_skip(BOOTSTAT_SOUND));
    bootstat_begin(BOOTSTAT_SOUND);
    CHECK(boot(0) == 0);                              /* failure 2: ASD skips sound */
    r = disk_record();
    CHECK(r.failures == 2 && r.fail_count[BOOTSTAT_SOUND] == 2 && r.skipped == 1u << BOOTSTAT_SOUND);
    CHECK(r.skipped_at[BOOTSTAT_SOUND] == 3 && bootstat_skip(BOOTSTAT_SOUND));
    CHECK(strstr(logs, "[asd] skip class=sound failures=2 boot=3"));
    struct ciuki_recovery_status s; bootstat_status(&s);
    CHECK(s.newly_skipped == 1u << BOOTSTAT_SOUND && s.skipped == s.newly_skipped && s.failures == 2);
    CHECK((s.boot_flags & CBI_F_BOOT_FAILED) && (s.boot_flags >> CBI_F_SKIP_SHIFT) == 1u << BOOTSTAT_SOUND);
    /* The next start skips it from the loader's flags, without new blame. */
    bootstat_begin(BOOTSTAT_NETWORK); bootstat_end(BOOTSTAT_NETWORK);
    bootstat_presented();
    CHECK(bootstat_service() == 0);
    r = disk_record();
    CHECK(r.state == 0 && r.failures == 0 && r.completed == 1 && r.skipped == 1u << BOOTSTAT_SOUND);
    CHECK(boot(0) == 0 && bootstat_skip(BOOTSTAT_SOUND));
    bootstat_status(&s);
    CHECK(!s.newly_skipped && !(s.boot_flags & CBI_F_BOOT_FAILED));
    /* Enable: the class loads again and its failures are forgotten. */
    CHECK(bootstat_enable(1u << BOOTSTAT_SOUND) == 0);
    r = disk_record();
    CHECK(!r.skipped && !r.fail_count[BOOTSTAT_SOUND] && !r.skipped_at[BOOTSTAT_SOUND]);
    CHECK(bootstat_enable(0x80) == -FS_EINVAL);
}

static void test_failures_outside_classes_and_recovery(void)
{
    volume(true);
    CHECK(boot(0) == 0);
    CHECK(boot(0) == 0);                              /* died before any class */
    struct bootstat_record r = disk_record();
    CHECK(r.failures == 1 && !r.blamed && !r.skipped);
    bootstat_begin(BOOTSTAT_USB); bootstat_end(BOOTSTAT_USB);
    CHECK(disk_record().active == 1u << BOOTSTAT_USB);   /* end: no I/O in the caller */
    CHECK(bootstat_service() == 0);                   /* the writer task's next turn */
    CHECK(!disk_record().active);
    CHECK(boot(0) == 0);                              /* finished USB, died later */
    r = disk_record();
    CHECK(r.failures == 2 && !r.blamed && !r.fail_count[BOOTSTAT_USB]);
    /* A single failure is forgotten by a completed start that ran it. */
    bootstat_begin(BOOTSTAT_PORTS);
    CHECK(boot(0) == 0);
    CHECK(disk_record().fail_count[BOOTSTAT_PORTS] == 1);
    bootstat_begin(BOOTSTAT_PORTS); bootstat_end(BOOTSTAT_PORTS);
    bootstat_presented(); CHECK(bootstat_service() == 0);
    r = disk_record();
    CHECK(!r.fail_count[BOOTSTAT_PORTS] && !r.skipped && r.state == 0);
}

static void test_completion_waits_for_active_classes(void)
{
    volume(true);
    CHECK(boot(0) == 0);
    unsigned before = writes;
    bootstat_begin(BOOTSTAT_POWER);                   /* the ACPI thread */
    CHECK(writes == before + 1);
    bootstat_presented();
    CHECK(bootstat_service() == 0 && disk_record().state == 1);   /* not complete yet */
    bootstat_end(BOOTSTAT_POWER);
    CHECK(writes == before + 1);                      /* end does no I/O */
    CHECK(bootstat_service() == 0);
    struct bootstat_record r = disk_record();
    CHECK(r.state == 0 && r.completed == 1 && !r.active && strstr(logs, "start complete"));
    /* A late activation after completion: in progress again, then clean. */
    bootstat_begin(BOOTSTAT_DISPLAY);
    CHECK(disk_record().state == 1 && disk_record().active == 1);
    bootstat_end(BOOTSTAT_DISPLAY);
    CHECK(bootstat_service() == 0);
    r = disk_record();
    CHECK(r.state == 0 && !r.active && r.completed == 1);
    unsigned quiet = writes;
    CHECK(bootstat_service() == 0 && writes == quiet);
    bootstat_presented();                             /* every later frame: nothing */
    CHECK(writes == quiet);
    /* A failed write is retried by the writer task. */
    bootstat_begin(BOOTSTAT_DISPLAY); bootstat_end(BOOTSTAT_DISPLAY);
    fail_writes = 1;
    CHECK(bootstat_service() == -FS_EIO && strstr(logs, "write error"));
    CHECK(bootstat_service() == 0 && disk_record().state == 0);
}

static void test_configure_requests_and_menu_once(void)
{
    volume(true);
    CHECK(boot(0) == 0);
    CHECK(bootstat_configure(0x06, BOOTSTAT_REQ_MENU) == 0);
    struct bootstat_record r = disk_record();
    CHECK(r.disabled == 0x06 && r.request == BOOTSTAT_REQ_MENU);
    CHECK(bootstat_configure(0x80, 0) == -FS_EINVAL && bootstat_configure(0, 8) == -FS_EINVAL);
    CHECK(bootstat_schedule_scan(true) == 0);
    r = disk_record();
    CHECK(r.request == (BOOTSTAT_REQ_MENU | BOOTSTAT_REQ_SCANDISK | BOOTSTAT_REQ_THOROUGH));
    bootstat_presented(); CHECK(bootstat_service() == 0);
    /* Next start: disabled classes are this start's skips; requests are consumed. */
    CHECK(boot(0) == 0);
    CHECK(bootstat_skip(BOOTSTAT_SOUND) && bootstat_skip(BOOTSTAT_NETWORK) && !bootstat_skip(BOOTSTAT_USB));
    r = disk_record();
    CHECK(r.disabled == 0x06 && r.request == 0);
    struct ciuki_recovery_status s; bootstat_status(&s);
    CHECK(s.version == CIUKI_RECOVERY_VERSION && s.size == sizeof(s) && (s.flags & CIUKI_RECOVERY_VALID) &&
          !(s.flags & CIUKI_RECOVERY_COMPLETE) && s.disabled == 0x06 && s.boots == 2);
    bootstat_reset();
    bootstat_status(&s);
    CHECK(!s.flags && bootstat_configure(0, 0) == -FS_ENOENT && bootstat_enable(1) == -FS_ENOENT);
    bootstat_begin(BOOTSTAT_SOUND);                   /* unbound: no-ops */
    bootstat_presented();
    CHECK(bootstat_service() == 0);
}

static void test_record_codec(void)
{
    struct bootstat_record r = { .state = 1, .version = 1, .disabled = 0x41, .skipped = 2, .active = 4, .request = 5,
                                 .failures = 3, .blamed = 4, .boots = 0x01020304, .completed = 7 };
    r.fail_count[2] = 9; r.skipped_at[1] = 77;
    uint8_t b[512]; bootstat_encode(b, &r);
    struct bootstat_record back; CHECK(bootstat_decode(b, &back) && !memcmp(&r, &back, sizeof(r)));
    b[BOOTSTAT_OFF_STATE] = 0;                         /* the loader's byte is outside the CRC */
    CHECK(bootstat_decode(b, &back) && back.state == 0);
    b[BOOTSTAT_OFF_STATE] = 2; CHECK(!bootstat_decode(b, &back));
    b[BOOTSTAT_OFF_STATE] = 1; b[BOOTSTAT_OFF_SKIPPED] = 0x80; CHECK(!bootstat_decode(b, &back));
    bootstat_encode(b, &r); b[300] = 1; CHECK(!bootstat_decode(b, &back));
    CHECK(!strcmp(bootstat_class_name(BOOTSTAT_POWER), "power") && !strcmp(bootstat_class_name(9), "unknown"));
}

static void test_boot_identity_requires_durable_increment(void)
{
    struct ciuki_recovery_status s;
    volume(true); fail_writes=1;
    CHECK(boot(0)==-FS_EIO && !bootstat_bound());
    bootstat_status(&s); CHECK(!(s.flags&CIUKI_RECOVERY_VALID));
    CHECK(!boot(0)); bootstat_status(&s); CHECK((s.flags&CIUKI_RECOVERY_VALID) && s.boots==1);
    fail_flushes=1;
    CHECK(boot(0)==-FS_EIO && !bootstat_bound());
    bootstat_status(&s); CHECK(!(s.flags&CIUKI_RECOVERY_VALID));
    CHECK(!boot(0)); bootstat_status(&s); CHECK((s.flags&CIUKI_RECOVERY_VALID) && s.boots==3);
    struct bootstat_record r=disk_record(); r.boots=UINT32_MAX;
    bootstat_encode(sector9(),&r);
    CHECK(boot(0)==-FS_EIO && !bootstat_bound());
    bootstat_status(&s); CHECK(!(s.flags&CIUKI_RECOVERY_VALID));
    CHECK(disk_record().boots==UINT32_MAX);
    r.boots=UINT32_MAX-1; bootstat_encode(sector9(),&r);
    CHECK(boot(0)==-FS_EIO && !bootstat_bound());
    puts("PASS g32b boot identity: failed write/flush never VALID; next durable increment differs; saturation refused");
}

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    test_record_codec();
    test_adoption_and_refusals();
    test_two_failures_skip_a_class();
    test_failures_outside_classes_and_recovery();
    test_completion_waits_for_active_classes();
    test_configure_requests_and_menu_once();
    test_boot_identity_requires_durable_increment();
    printf("bootstat: %u failures\n", failures);
    return failures != 0;
}
