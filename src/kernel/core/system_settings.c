/* G30B: consume settings before optional drivers and the pager start.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/system_settings.h>
static struct system_perf performance = {0, 3, 1, 0, 16, 1024};
static struct { uint32_t id; bool disabled; char driver[32]; } choices[32];
static unsigned choice_count, boot_profile;
static bool boot_choices;
void system_profile_select(unsigned index) { if (index < 8) boot_profile = index; }
unsigned system_profile_index(void) { return boot_profile; }
static char text[24576];
static struct vfs_table table;
const struct system_perf *system_performance(void) { return &performance; }
void system_hw_parse(const char *input, size_t size, unsigned selected)
{
    choice_count = 0;
    for (size_t at = 0; at < size;) {
        size_t first = at; while (at < size && input[at] != '\n') at++;
        size_t n = at - first; if (at < size) at++;
        const char *p = input + first;
        if (n < 12 || p[0] != (char)('0' + selected) || p[1] != ':' || memcmp(p + 2, "device=", 7)) continue;
        p += 9; n -= 9;
        size_t idlen = 0; while (idlen < n && p[idlen] != ' ') idlen++;
        uint32_t id;
        if (!system_uint(p, idlen, 16, &id) || idlen == n) continue;
        p += idlen; n -= idlen;
        if (n < 30 || memcmp(p, " disabled=", 10) || p[10] < '0' || p[10] > '1' ||
            memcmp(p + 11, " removed=", 9) || p[20] < '0' || p[20] > '1' || memcmp(p + 21, " driver=", 8)) continue;
        bool disabled = p[10] == '1'; p += 29; n -= 29;
        if (n && p[n - 1] == '\r') n--;
        if (!n || n >= 32 || choice_count == 32) continue;
        bool valid = true;
        for (size_t i = 0; i < n; i++) if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
            (p[i] >= '0' && p[i] <= '9') || p[i] == '_' || p[i] == '-' || p[i] == '.')) valid = false;
        if (!valid) continue;
        bool duplicate = false; for (unsigned i = 0; i < choice_count; i++) if (choices[i].id == id) duplicate = true;
        if (duplicate) continue;
        choices[choice_count].id = id; choices[choice_count].disabled = disabled;
        if (!(n == 1 && p[0] == '-')) { memcpy(choices[choice_count].driver, p, n); choices[choice_count].driver[n] = 0; }
        else choices[choice_count].driver[0] = 0;
        choice_count++;
    }
}
void system_boot_hw_copy(const char *input, size_t size, unsigned selected)
{
    if (size >= sizeof(text) || selected >= 8) return;
    system_profile_select(selected); system_hw_parse(input, size, selected); boot_choices = true;
}
static bool same_driver(const char *a, const char *b)
{
    for (unsigned i=0;i<32;i++) {
        unsigned x=(unsigned char)a[i], y=(unsigned char)b[i];
        if (x>='A' && x<='Z') x+=32;
        if (y>='A' && y<='Z') y+=32;
        if (x!=y) return false;
        if (!x) return true;
    }
    return false;
}
bool system_device_allowed(uint32_t id, const char *driver)
{
    for (unsigned i = 0; i < choice_count; i++) if (choices[i].id == id) {
        if (choices[i].disabled) return false;
        if (driver && choices[i].driver[0] && !same_driver(driver, choices[i].driver)) return false;
    }
    return true;
}
static int read_setting(const char *path)
{
    int fd = vfs_open(&table, path, VFS_READ, VFS_DENY_NONE, 0);
    if (fd < 0) return fd;
    size_t n = 0;
    int e = vfs_read(&table, fd, text, sizeof(text), &n);
    vfs_close(&table, fd);
    if (e || n >= sizeof(text)) return -FS_EIO;
    return (int)n;
}
void system_settings_load(struct vfs *v)
{
    performance = system_perf_default(); if (!boot_choices) choice_count = 0;
    if (!v || !v->volumes[2] || vfs_table_init(v, &table, 2)) return;
    int n = read_setting("C:\\SYSTEM\\SETTINGS\\performance.cfg");
    if (n > 0 && !system_perf_parse(&performance, text, (size_t)n)) klog("[settings] performance invalid; defaults");
    n = read_setting("C:\\SYSTEM\\HWPROF.CFG");
    if (n == -FS_ENOENT) n = read_setting("C:\\SYSTEM\\SETTINGS\\hwprofiles.cfg");
    unsigned selected = boot_profile;
    if (n > 0) system_hw_parse(text, (size_t)n, selected);
    vfs_table_destroy(&table);
    klog("[settings] profile=%u choices=%u role=%u read_ahead=%u graphics=%u vm=%u min_mb=%u max_mb=%u",
        selected, choice_count, performance.role, performance.read_ahead, performance.graphics, performance.vm, performance.min_mb, performance.max_mb);
}
