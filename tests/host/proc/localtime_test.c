/* G1: production time-zone service (proc/timezone.c), clock seeding from
 * an RTC that keeps LOCAL time, daylight-saving changes while running and
 * while switched off, setting the clock and the zone with and without the
 * time authority, the syscall record, TZ for spawned programs, and FAT
 * timestamps (local on disk, UTC through stat) on the real FAT/VFS/posix
 * path of files_test.c. Europe/Rome 2026: CEST from 29 March 01:00 UTC to
 * 25 October 01:00 UTC.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#include "files_test.c"
#include "../../../src/kernel/lib/tzrule.c"
#include "../../../src/kernel/proc/timezone.c"

static char klog_text[4096];
void klog(const char *fmt, ...)
{
    size_t n = strlen(klog_text);
    va_list a; va_start(a, fmt);
    if (n + 200 < sizeof(klog_text)) { vsnprintf(klog_text + n, sizeof(klog_text) - n - 1, fmt, a); strcat(klog_text, "\n"); }
    va_end(a);
}

static char store[512];
static size_t store_n;
static unsigned store_writes;
static int mem_read(char *t, size_t cap, size_t *n)
{
    if (!store_n) return -ENOENT;
    CHECK(store_n <= cap); memcpy(t, store, store_n); *n = store_n; return 0;
}
static int mem_write(const char *t, size_t n)
{
    CHECK(n < sizeof(store)); memcpy(store, t, n); store_n = n; store[n] = 0; store_writes++; return 0;
}
static int64_t civil(int y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{ return tzr_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s; }
static void rtc_set(int64_t local)
{
    int64_t y; unsigned m, d; int64_t days = tzr_floor_div(local, 86400), rem = local - days * 86400;
    tzr_civil_from_days(days, &y, &m, &d);
    setup_rtc_date = (uint16_t)((y - 1980) << 9 | m << 5 | d);
    setup_rtc_time = (uint16_t)((rem / 3600) << 11 | ((rem / 60) % 60) << 5 | (rem % 60) / 2);
}
static int64_t rtc_get(void) { int64_t s; CHECK(clock_fat_civil(setup_rtc_date, setup_rtc_time, &s)); return s; }
static bool fake_rtc(uint16_t *date, uint16_t *time, uint8_t *tenths)
{ *date = setup_rtc_date; *time = setup_rtc_time; *tenths = 0; return true; }
static void configure(const char *zone, const char *tz, int rtc_local, int bias)
{
    store_n = (size_t)snprintf(store, sizeof(store), "version=2\nrtc_local=%d\noffset_minutes=60\nzone=%s\ntz=%s\nauto_dst=1\nrtc_bias_minutes=%d\n",
                               rtc_local, zone, tz, bias);
}
#define ROME "CET-1CEST,M3.5.0,M10.5.0/3"
static void boot(int bias, int64_t rtc_local, int rtc_mode)
{
    configure("Europe/Rome", ROME, rtc_mode, bias);
    timezone_reset(); timezone_set_store(mem_read, mem_write);
    rtc_set(rtc_local); fs_set_calendar_clock(fake_rtc);
    g_ticks = 5000; file_clock_start(42, true);
}
static int64_t now(void) { struct ciuki_timespec t; CHECK(!file_clock_now(CLOCK_REALTIME, client, &t)); return t.tv_sec; }
static int op(uint32_t operation, struct ciuki_time_zone *q)
{
    q->version = CIUKI_TIME_VERSION; q->size = sizeof(*q); q->operation = operation;
    return timezone_call(client, q);
}
static unsigned rtc_writes(void) { return setup_rtc_writes; }

static void test_boot_and_dst(void)
{
    struct ciuki_time_zone q = {0};
    /* Winter boot: the RTC says 12:00 CET. */
    boot(60, civil(2026, 1, 15, 12, 0, 0), 1);
    CHECK(now() == civil(2026, 1, 15, 11, 0, 0));
    unsigned w = rtc_writes();
    CHECK(!op(CIUKI_TIME_GET, &q) && q.offset_minutes == 60 && q.local_seconds == civil(2026, 1, 15, 12, 0, 0));
    CHECK(q.flags == (CIUKI_TIME_AUTO_DST | CIUKI_TIME_RTC_LOCAL) && rtc_writes() == w);
    CHECK(q.standard_minutes == 60 && q.daylight_minutes == 120 && q.rtc_bias_minutes == 60);
    CHECK(!strcmp(q.zone, "Europe/Rome") && !strcmp(q.rule, ROME) && !strcmp(q.standard_name, "CET") && !strcmp(q.daylight_name, "CEST"));
    CHECK(q.next_change == civil(2026, 3, 29, 1, 0, 0));
    /* Summer boot: the RTC says 12:00 CEST. */
    boot(120, civil(2026, 7, 1, 12, 0, 0), 1);
    CHECK(now() == civil(2026, 7, 1, 10, 0, 0));
    CHECK(!op(CIUKI_TIME_GET, &q) && q.offset_minutes == 120 && (q.flags & CIUKI_TIME_DAYLIGHT) && rtc_writes() == w);
    /* Switched off across the spring change: the RTC still holds CET. */
    boot(60, civil(2026, 4, 1, 12, 0, 0), 1);
    CHECK(now() == civil(2026, 4, 1, 11, 0, 0));
    store_writes = 0;
    CHECK(!op(CIUKI_TIME_GET, &q) && (q.flags & CIUKI_TIME_ADJUSTED) && q.rtc_bias_minutes == 120);
    CHECK(rtc_writes() == w + 1 && rtc_get() == civil(2026, 4, 1, 13, 0, 0) && store_writes == 1);
    CHECK(strstr(store, "rtc_bias_minutes=120\n"));
    CHECK(!op(CIUKI_TIME_GET, &q) && rtc_writes() == w + 1 && (q.flags & CIUKI_TIME_ADJUSTED));
    client->time_capability = false;
    CHECK(op(CIUKI_TIME_ACKNOWLEDGE, &q) == -EPERM);
    client->time_capability = true;
    CHECK(!op(CIUKI_TIME_ACKNOWLEDGE, &q) && !(q.flags & CIUKI_TIME_ADJUSTED) && (q.flags & CIUKI_TIME_AUTHORITY));
    w = rtc_writes();
    /* Running across the spring change: 01:59 CET -> 03:00 CEST. */
    boot(60, civil(2026, 3, 29, 1, 59, 0), 1);
    CHECK(now() == civil(2026, 3, 29, 0, 59, 0));
    CHECK(!op(CIUKI_TIME_GET, &q) && !(q.flags & CIUKI_TIME_ADJUSTED) && rtc_writes() == w);
    g_ticks += 60000;
    CHECK(!op(CIUKI_TIME_GET, &q) && (q.flags & CIUKI_TIME_ADJUSTED) && (q.flags & CIUKI_TIME_DAYLIGHT));
    CHECK(q.local_seconds == civil(2026, 3, 29, 3, 0, 0) && rtc_get() == civil(2026, 3, 29, 3, 0, 0) && q.rtc_bias_minutes == 120);
    CHECK(q.next_change == civil(2026, 10, 25, 1, 0, 0));
    /* Running across the autumn change: 02:59 CEST -> 02:00 CET. */
    boot(120, civil(2026, 10, 25, 2, 59, 0), 1);
    CHECK(now() == civil(2026, 10, 25, 0, 59, 0));
    w = rtc_writes();
    CHECK(!op(CIUKI_TIME_GET, &q) && rtc_writes() == w);
    g_ticks += 60000;
    CHECK(!op(CIUKI_TIME_GET, &q) && (q.flags & CIUKI_TIME_ADJUSTED) && !(q.flags & CIUKI_TIME_DAYLIGHT));
    CHECK(q.local_seconds == civil(2026, 10, 25, 2, 0, 0) && rtc_get() == civil(2026, 10, 25, 2, 0, 0) && q.rtc_bias_minutes == 60);
    /* RTC kept in UTC: no correction at either change. */
    boot(0, civil(2026, 3, 29, 0, 59, 0), 0);
    CHECK(now() == civil(2026, 3, 29, 0, 59, 0));
    w = rtc_writes(); g_ticks += 60000;
    CHECK(!op(CIUKI_TIME_GET, &q) && rtc_writes() == w && q.offset_minutes == 120 && !(q.flags & CIUKI_TIME_RTC_LOCAL));
    /* No clock.cfg: the historical UTC convention. */
    store_n = 0; timezone_reset(); rtc_set(civil(2026, 7, 1, 12, 0, 0)); file_clock_start(42, true);
    CHECK(now() == civil(2026, 7, 1, 12, 0, 0) && !op(CIUKI_TIME_GET, &q) && !q.offset_minutes && !strcmp(q.rule, "UTC0"));
    char env[96];
    CHECK(timezone_env(env, sizeof(env)) && !strcmp(env, "TZ=UTC0"));
    /* Version 1 file (f9-04b Setup): fixed offset, RTC local. */
    store_n = (size_t)snprintf(store, sizeof(store), "version=1\nrtc_local=1\noffset_minutes=60\n");
    timezone_reset(); rtc_set(civil(2026, 7, 1, 12, 0, 0)); file_clock_start(42, true);
    CHECK(now() == civil(2026, 7, 1, 11, 0, 0) && !op(CIUKI_TIME_GET, &q) && q.offset_minutes == 60 && !(q.flags & CIUKI_TIME_ADJUSTED));
    puts("localtime boot/DST: RTC local in CET and CEST, change while off (adjusted once, acknowledged), both 2026 change days while running, RTC in UTC, no file, version 1 PASS");
}

static void test_set(void)
{
    struct ciuki_time_zone q = {0};
    boot(120, civil(2026, 10, 24, 20, 0, 0), 1);
    client->time_capability = false;
    unsigned w = rtc_writes();
    q.local_seconds = civil(2026, 12, 24, 18, 0, 0);
    CHECK(op(CIUKI_TIME_SET_LOCAL, &q) == -EPERM && rtc_writes() == w);
    CHECK(op(CIUKI_TIME_SET_UTC, &q) == -EPERM && op(CIUKI_TIME_SET_ZONE, &q) == -EPERM);
    CHECK(!op(CIUKI_TIME_GET, &q) && !(q.flags & CIUKI_TIME_AUTHORITY));
    client->time_capability = true;
    /* Repeated hour: first occurrence (CEST). */
    q.local_seconds = civil(2026, 10, 25, 2, 30, 0);
    CHECK(!op(CIUKI_TIME_SET_LOCAL, &q) && now() == civil(2026, 10, 25, 0, 30, 0));
    CHECK(rtc_get() == civil(2026, 10, 25, 2, 30, 0) && q.rtc_bias_minutes == 120 && q.offset_minutes == 120);
    /* Into winter: the RTC bias follows the new date at once. */
    q.local_seconds = civil(2026, 12, 24, 18, 0, 0);
    uint32_t gen = q.generation;
    CHECK(!op(CIUKI_TIME_SET_LOCAL, &q) && now() == civil(2026, 12, 24, 17, 0, 0) && q.generation == gen + 1);
    CHECK(rtc_get() == civil(2026, 12, 24, 18, 0, 0) && q.rtc_bias_minutes == 60 && strstr(store, "rtc_bias_minutes=60\n"));
    CHECK(!(q.flags & CIUKI_TIME_ADJUSTED));
    /* Skipped hour reads as standard time. */
    q.local_seconds = civil(2026, 3, 29, 2, 30, 0);
    CHECK(!op(CIUKI_TIME_SET_LOCAL, &q) && now() == civil(2026, 3, 29, 1, 30, 0) && rtc_get() == civil(2026, 3, 29, 3, 30, 0));
    q.utc_seconds = civil(2026, 8, 1, 6, 0, 0);
    CHECK(!op(CIUKI_TIME_SET_UTC, &q) && now() == civil(2026, 8, 1, 6, 0, 0) && rtc_get() == civil(2026, 8, 1, 8, 0, 0));
    q.local_seconds = civil(1979, 12, 31, 23, 0, 0);
    CHECK(op(CIUKI_TIME_SET_LOCAL, &q) == -EINVAL && now() == civil(2026, 8, 1, 6, 0, 0));
    q.local_seconds = civil(2080, 1, 1, 0, 0, 0);
    CHECK(op(CIUKI_TIME_SET_LOCAL, &q) == -EINVAL);
    /* Zone change: New York; the RTC moves to New York time. */
    memset(&q, 0, sizeof(q));
    strcpy(q.zone, "America/New_York"); strcpy(q.rule, "EST5EDT,M3.2.0,M11.1.0"); q.flags = CIUKI_TIME_AUTO_DST;
    CHECK(!op(CIUKI_TIME_SET_ZONE, &q) && q.offset_minutes == -240 && now() == civil(2026, 8, 1, 6, 0, 0));
    CHECK(rtc_get() == civil(2026, 8, 1, 2, 0, 0) && strstr(store, "zone=America/New_York\n") && strstr(store, "offset_minutes=-300\n"));
    char env[96];
    CHECK(timezone_env(env, sizeof(env)) && !strcmp(env, "TZ=EST5EDT,M3.2.0,M11.1.0"));
    /* Rome without automatic adjustment: standard time all year. */
    strcpy(q.zone, "Europe/Rome"); strcpy(q.rule, ROME); q.flags = 0;
    CHECK(!op(CIUKI_TIME_SET_ZONE, &q) && q.offset_minutes == 60 && !(q.flags & CIUKI_TIME_AUTO_DST) && !q.next_change);
    CHECK(rtc_get() == civil(2026, 8, 1, 7, 0, 0) && timezone_env(env, sizeof(env)) && !strcmp(env, "TZ=CET-1"));
    CHECK(strstr(store, "auto_dst=0\n") && !strcmp(q.rule, ROME));
    /* Refusals leave everything unchanged. */
    w = rtc_writes(); unsigned sw = store_writes;
    static const char *const bad_rules[] = {"CET", "CET-1CEST,M3.5.0", "x"};
    for (unsigned i = 0; i < 3; i++) {
        strcpy(q.rule, bad_rules[i]); q.flags = CIUKI_TIME_AUTO_DST;
        CHECK(op(CIUKI_TIME_SET_ZONE, &q) == -EINVAL);
    }
    strcpy(q.rule, ROME); strcpy(q.zone, "Europe/Rome;"); CHECK(op(CIUKI_TIME_SET_ZONE, &q) == -EINVAL);
    strcpy(q.zone, "Europe/Rome"); q.flags = 64; CHECK(op(CIUKI_TIME_SET_ZONE, &q) == -EINVAL);
    memset(q.rule, 'A', sizeof(q.rule)); q.flags = 1; CHECK(op(CIUKI_TIME_SET_ZONE, &q) == -EINVAL);
    q.operation = 9; q.version = 1; q.size = sizeof(q); CHECK(timezone_call(client, &q) == -EINVAL);
    q.version = 2; q.operation = 0; CHECK(timezone_call(client, &q) == -EINVAL);
    CHECK(rtc_writes() == w && store_writes == sw);
    /* Syscall record through user memory. */
    struct ciuki_time_zone u = {.version = 1, .size = sizeof(u), .operation = CIUKI_TIME_GET};
    CHECK(!ua_write(client->memory, USER, &u, sizeof(u)));
    struct trap_frame tf = {.eax = CIUKI_SYS_TIME, .ebx = USER, .ecx = sizeof(u)};
    CHECK(time_syscall(&tf) == 0 && !ua_read(client->memory, &u, USER, sizeof(u)));
    CHECK(u.offset_minutes == 60 && u.utc_seconds == civil(2026, 8, 1, 6, 0, 0) && u.local_seconds == civil(2026, 8, 1, 7, 0, 0));
    tf.ecx = sizeof(u) - 1; CHECK(time_syscall(&tf) == -EINVAL);
    tf.ecx = sizeof(u); tf.ebx = 16; CHECK(time_syscall(&tf) == -EFAULT);
    puts("localtime set: authority refusals, SET_LOCAL on both change days and into winter, SET_UTC, range, zone change moves the RTC, auto-DST off, TZ env, malformed records, syscall copy PASS");
}

static void test_fat(void)
{
    struct ciuki_time_zone q = {0};
    boot(120, civil(2026, 7, 1, 12, 0, 0), 1);
    int fd = file_open(client, "/summer.txt", O_CREAT | O_EXCL | O_RDWR, 0666); CHECK(fd >= 0);
    CHECK(io(fd, "s", 1, true, true, 0) == 1);
    struct px_node *summer = file_fd(client, fd)->node;
    CHECK((summer->entry.times.write_time >> 11) == 12 && (summer->entry.times.write_date & 31) == 1);
    CHECK((summer->entry.times.create_time >> 11) == 12);
    struct ciuki_stat st; CHECK(!locked_stat(summer, &st) && st.st_mtim.tv_sec == civil(2026, 7, 1, 10, 0, 0));
    CHECK(!file_close(client, fd));
    client->time_capability = true;
    q.local_seconds = civil(2026, 12, 1, 9, 0, 0);
    CHECK(!op(CIUKI_TIME_SET_LOCAL, &q));
    fd = file_open(client, "/winter.txt", O_CREAT | O_EXCL | O_RDWR, 0666); CHECK(fd >= 0);
    CHECK(io(fd, "w", 1, true, true, 0) == 1);
    struct px_node *winter = file_fd(client, fd)->node;
    CHECK((winter->entry.times.write_time >> 11) == 9 && ((winter->entry.times.write_date >> 5) & 15) == 12);
    CHECK(!locked_stat(winter, &st) && st.st_mtim.tv_sec == civil(2026, 12, 1, 8, 0, 0));
    /* The summer stamp keeps its wall-clock value in winter (FAT local). */
    fd = file_open(client, "/summer.txt", O_RDONLY, 0); CHECK(fd >= 0);
    CHECK(!locked_stat(file_fd(client, fd)->node, &st) && st.st_mtim.tv_sec == civil(2026, 7, 1, 10, 0, 0));
    all_closed();
    CHECK(!remove_path_native("/summer.txt", false) && !remove_path_native("/winter.txt", false));
    fs_set_timestamp_clock(0); fs_set_calendar_clock(0);
    puts("localtime FAT: stamps in local time (CEST and CET), stat in UTC through the stamp's own rule PASS");
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller; proc_init();
    CHECK(!fake_open(&disk[0], argv[1]) && !fake_open(&disk[1], argv[2]));
    CHECK(!cache_init(&cache, 0)); vfs_init(&vfs);
    for (unsigned i = 0; i < 2; i++) { CHECK(!fat_mount(&volumes[i], &cache, &disk[i].dev, 0, disk[i].dev.capacity, true)); CHECK(!vfs_attach(&vfs, i + 2, &volumes[i])); }
    CHECK(!files_attach(&vfs, &space));
    client_thread = make_process(proc_supervisor(), &client); g_current = client_thread->task; g_current->state = T_RUNNING;
    client->cwd = space->root; px_retain(client->cwd); client->cwd_retain = px_retain; client->cwd_release = px_release;
    test_boot_and_dst(); test_set(); test_fat();
    all_closed(); stop_collect(client);
    for (unsigned i = 0; i < 2; i++) CHECK(!vfs_detach(&vfs, i + 2));
    vfs_destroy(&vfs); cache_destroy(&cache);
    for (unsigned i = 0; i < 2; i++) { fake_reset(&disk[i]); fake_close(&disk[i]); }
    printf("PASS localtime: checks=%u\n", checks);
    free(ram); return 0;
}
