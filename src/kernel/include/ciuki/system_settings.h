/* G30B boot settings. Kernel-internal and shared with the native Control Panel;
 * no syscall/wire ABI. SPDX-License-Identifier: MIT */
#ifndef CIUKI_SYSTEM_SETTINGS_H
#define CIUKI_SYSTEM_SETTINGS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define SYSTEM_HW_PATH "/system/HWPROF.CFG"
#define SYSTEM_PERF_PATH "/system/settings/performance.cfg"
struct system_perf {
    unsigned role;          /* 0 desktop, 1 mobile, 2 server */
    unsigned read_ahead;    /* 0 none, 1 16 KiB, 2 32 KiB, 3 64 KiB */
    unsigned graphics;      /* 0 software, 1 full */
    unsigned vm;            /* 0 automatic, 1 manual, 2 disabled */
    unsigned min_mb, max_mb;
};
static inline struct system_perf system_perf_default(void)
{ return (struct system_perf){0, 3, 1, 0, 16, 1024}; }
static inline bool system_perf_valid(const struct system_perf *p)
{
    return p->role <= 2 && p->read_ahead <= 3 && p->graphics <= 1 && p->vm <= 2 &&
        p->min_mb >= 16 && p->max_mb <= 1024 && p->min_mb <= p->max_mb &&
        !(p->min_mb % 16) && !(p->max_mb % 16);
}
static inline bool system_key(const char *s, size_t n, const char *key)
{
    size_t i = 0; while (key[i] && i < n && s[i] == key[i]) i++;
    return i == n && !key[i];
}
static inline bool system_uint(const char *s, size_t n, unsigned base, uint32_t *out)
{
    if (!n) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i], d = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 255;
        if (d >= base || v > (UINT32_MAX - d) / base) return false;
        v = v * base + d;
    }
    *out = v; return true;
}
static inline bool system_perf_parse(struct system_perf *out, const char *text, size_t bytes)
{
    struct system_perf p = system_perf_default(); unsigned seen = 0;
    for (size_t at = 0; at < bytes;) {
        size_t first = at; while (at < bytes && text[at] != '\n') at++;
        size_t end = at; if (at < bytes) at++;
        if (end > first && text[end - 1] == '\r') end--;
        if (end == first || text[first] == '#') continue;
        size_t eq = first; while (eq < end && text[eq] != '=') eq++;
        if (eq == end) return false;
        static const char *const keys[] = {"version", "role", "read_ahead", "graphics", "vm", "min_mb", "max_mb"};
        unsigned k = 0; while (k < 7 && !system_key(text + first, eq - first, keys[k])) k++;
        if (k == 7) continue;
        uint32_t value;
        if ((seen & (1u << k)) || !system_uint(text + eq + 1, end - eq - 1, 10, &value)) return false;
        seen |= 1u << k;
        switch (k) {
        case 0: if (value != 1) return false; break;
        case 1: p.role = value; break;
        case 2: p.read_ahead = value; break;
        case 3: p.graphics = value; break;
        case 4: p.vm = value; break;
        case 5: p.min_mb = value; break;
        case 6: p.max_mb = value; break;
        }
    }
    if (!(seen & 1) || !system_perf_valid(&p)) return false;
    *out = p; return true;
}
struct vfs;
void system_settings_load(struct vfs *);
void system_profile_select(unsigned);
void system_boot_hw_copy(const char *, size_t, unsigned);
unsigned system_profile_index(void);
void system_hw_parse(const char *, size_t, unsigned selected);
bool system_device_allowed(uint32_t, const char *driver);
const struct system_perf *system_performance(void);
/* Standalone driver/cache harnesses omit the boot settings module. */
#pragma weak system_boot_hw_copy
#pragma weak system_profile_select
#pragma weak system_profile_index
#pragma weak system_settings_load
#pragma weak system_device_allowed
#pragma weak system_performance
static inline bool system_admit(uint32_t id, const char *driver)
{ return !system_device_allowed || system_device_allowed(id, driver); }
static inline uint32_t system_pci_id(unsigned bus, unsigned dev, unsigned fn)
{ return 0x10000u | (bus << 8) | (dev << 3) | fn; }
#endif
