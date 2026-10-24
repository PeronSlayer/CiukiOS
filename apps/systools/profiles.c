/* CP-19 Hardware Profiles and Performance status. SPDX-License-Identifier: MIT */
#include "systools.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        unsigned x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

void hp_init(struct hp_store *s)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->p[0].name, HP_NAME, "%s", HP_ORIGINAL);
    s->count = 1; s->wait_seconds = 30;
    dc_init(&s->p[0].devices);
}
static int name_ok(const char *n)
{
    size_t len = n ? strlen(n) : 0;
    if (!len || len >= HP_NAME || n[0] == ' ' || n[len - 1] == ' ') return 0;
    for (size_t i = 0; i < len; i++) if ((unsigned char)n[i] < ' ' || n[i] == '=' || (unsigned char)n[i] > 126) return 0;
    return 1;
}
static int taken(const struct hp_store *s, const char *n, int except)
{
    for (unsigned i = 0; i < s->count; i++) if ((int)i != except && same_name(s->p[i].name, n)) return 1;
    return 0;
}
int hp_copy(struct hp_store *s, unsigned from, const char *name)
{
    if (from >= s->count) return -ENOENT;
    if (!name_ok(name)) return -EINVAL;
    if (taken(s, name, -1)) return -EEXIST;
    if (s->count >= HP_MAX) return -ENOSPC;
    s->p[s->count] = s->p[from];
    snprintf(s->p[s->count].name, HP_NAME, "%s", name);
    s->count++;
    return 0;
}
int hp_rename(struct hp_store *s, unsigned index, const char *name)
{
    if (index >= s->count) return -ENOENT;
    if (!name_ok(name)) return -EINVAL;
    if (taken(s, name, (int)index)) return -EEXIST;
    snprintf(s->p[index].name, HP_NAME, "%s", name);
    return 0;
}
int hp_delete(struct hp_store *s, unsigned index)
{
    if (index >= s->count) return -ENOENT;
    if (s->count == 1) return -EPERM;
    if (index == s->active) return -EBUSY;
    memmove(&s->p[index], &s->p[index + 1], (s->count - index - 1) * sizeof(s->p[0]));
    s->count--;
    if (s->active > index) s->active--;
    if (s->default_index == index) s->default_index = 0;
    else if (s->default_index > index) s->default_index--;
    return 0;
}
int hp_move(struct hp_store *s, unsigned index, int delta)
{
    if (index >= s->count) return -ENOENT;
    int to = (int)index + delta;
    if (to < 0 || to >= (int)s->count) return -EINVAL;
    struct hp_profile moved = s->p[index];
    int step = to > (int)index ? 1 : -1;
    unsigned active = s->active, def = s->default_index;
    for (int i = (int)index; i != to; i += step) s->p[i] = s->p[i + step];
    s->p[to] = moved;
    unsigned *follow[2] = { &active, &def };
    for (int k = 0; k < 2; k++) {
        unsigned v = *follow[k];
        if (v == index) *follow[k] = (unsigned)to;
        else if (step > 0 && v > index && v <= (unsigned)to) *follow[k] = v - 1;
        else if (step < 0 && v < index && v >= (unsigned)to) *follow[k] = v + 1;
    }
    s->active = active; s->default_index = def;
    return 0;
}
int hp_set_wait(struct hp_store *s, int seconds)
{
    if (seconds < -1 || seconds > 999) return -EINVAL;
    s->wait_seconds = seconds;
    return 0;
}
int hp_set_default(struct hp_store *s, unsigned index)
{
    if (index >= s->count) return -ENOENT;
    s->default_index = index;
    return 0;
}
int hp_startup(const struct hp_store *s, int chosen, unsigned elapsed)
{
    if (s->count == 1) return 0;
    if (chosen >= 0 && (unsigned)chosen < s->count) return chosen;
    if (s->wait_seconds >= 0 && elapsed >= (unsigned)s->wait_seconds) return (int)s->default_index;
    return -1;
}
int hp_apply(struct hp_store *s, unsigned profile, const struct ciuki_device_record *d, int action, const char *arg, struct dc_result *r)
{
    if (profile >= s->count) { memset(r, 0, sizeof(*r)); r->error = -ENOENT; return -ENOENT; }
    dc_apply(&s->p[profile].devices, d, action, arg, r);
    return r->error;
}
int hp_save(const struct hp_store *s, char *out, size_t cap)
{
    size_t at = 0;
    int w = snprintf(out, cap, "version=1\nwait=%d\ndefault=%u\nactive=%u\nboot=%u\n", s->wait_seconds, s->default_index, s->active, s->boot_serial);
    if (w < 0 || (size_t)w >= cap) return -ENOSPC;
    at = (size_t)w;
    for (unsigned i = 0; i < s->count; i++) {
        w = snprintf(out + at, cap - at, "profile=%s|%d\n", s->p[i].name, s->p[i].portable);
        if (w < 0 || (size_t)w >= cap - at) return -ENOSPC;
        at += (size_t)w;
        char body[2560];   /* DC_MAX entries of up to 75 bytes plus the header */
        int n = dc_save(&s->p[i].devices, body, sizeof(body));
        if (n < 0) return -ENOSPC;
        for (char *line = body; *line;) {
            char *eol = strchr(line, '\n');
            if (!eol) break;
            *eol = 0;
            if (!strncmp(line, "device=", 7)) {
                w = snprintf(out + at, cap - at, "%u:%s\n", i, line);
                if (w < 0 || (size_t)w >= cap - at) return -ENOSPC;
                at += (size_t)w;
            }
            line = eol + 1;
        }
    }
    return (int)at;
}
int hp_load(struct hp_store *s, const char *text)
{
    hp_init(s);
    s->count = 0;
    int wait = 30; unsigned def = 0, active = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[200];
        if (len < sizeof(line)) {
            memcpy(line, p, len); line[len] = 0;
            if (!strncmp(line, "boot=", 5)) s->boot_serial = (uint32_t)strtoul(line + 5, NULL, 10);
            else if (!strncmp(line, "wait=", 5)) wait = atoi(line + 5);
            else if (!strncmp(line, "default=", 8)) def = (unsigned)atoi(line + 8);
            else if (!strncmp(line, "active=", 7)) active = (unsigned)atoi(line + 7);
            else if (!strncmp(line, "profile=", 8) && s->count < HP_MAX) {
                char *bar = strchr(line + 8, '|');
                if (bar) {
                    *bar = 0;
                    if (name_ok(line + 8) && !taken(s, line + 8, -1)) {
                        struct hp_profile *q = &s->p[s->count++];
                        memset(q, 0, sizeof(*q));
                        snprintf(q->name, HP_NAME, "%s", line + 8);
                        q->portable = atoi(bar + 1) != 0;
                    }
                }
            } else if (s->count && line[0] >= '0' && line[0] <= '9') {
                char *colon = strchr(line, ':');
                unsigned idx = colon ? (unsigned)atoi(line) : HP_MAX;
                if (colon && idx < s->count) {
                    char one[160];
                    snprintf(one, sizeof(one), "%s\n", colon + 1);
                    struct dc_store tmp;
                    if (dc_load(&tmp, one) == 1 && s->p[idx].devices.count < DC_MAX) s->p[idx].devices.e[s->p[idx].devices.count++] = tmp.e[0];
                }
            }
        }
        p += len + (eol ? 1 : 0);
        if (!eol) break;
    }
    if (!s->count) { hp_init(s); return 1; }
    s->wait_seconds = wait >= -1 && wait <= 999 ? wait : 30;
    s->default_index = def < s->count ? def : 0;
    s->active = active < s->count ? active : 0;
    return (int)s->count;
}

static void kib(char *out, size_t cap, uint64_t k)
{
    if (k >= 1024 * 10) snprintf(out, cap, "%llu.%llu MB", (unsigned long long)(k / 1024), (unsigned long long)(k % 1024 * 10 / 1024));
    else snprintf(out, cap, "%llu KB", (unsigned long long)k);
}
void pf_summarize(struct pf_status *o, const struct pf_input *in)
{
    char size[32], used[32];
    memset(o, 0, sizeof(*o));
    kib(size, sizeof(size), in->memory_kib);
    snprintf(o->memory, sizeof(o->memory), "%s of RAM", size);
    snprintf(o->resources, sizeof(o->resources), "%u%% free", in->system_free_pct);
    snprintf(o->file_system, sizeof(o->file_system), "32-bit");
    if (in->pagefile_present) {
        kib(size, sizeof(size), in->pagefile_kib); kib(used, sizeof(used), in->pagefile_used_kib);
        snprintf(o->virtual_memory, sizeof(o->virtual_memory), "32-bit, swap file %s (%s in use)", size, used);
    } else snprintf(o->virtual_memory, sizeof(o->virtual_memory), "Swap file not in use");
    if (in->accel_active) snprintf(o->graphics, sizeof(o->graphics), "Hardware acceleration: %s", in->accel_name[0] ? in->accel_name : "active");
    else snprintf(o->graphics, sizeof(o->graphics), "Hardware acceleration: software drawing");
    if (in->compressed_drives) snprintf(o->disk_compression, sizeof(o->disk_compression), "%u compressed drive(s)", in->compressed_drives);
    else snprintf(o->disk_compression, sizeof(o->disk_compression), "Not installed");
    if (in->pccard_sockets) snprintf(o->pc_cards, sizeof(o->pc_cards), "%u PC Card socket(s)", in->pccard_sockets);
    else snprintf(o->pc_cards, sizeof(o->pc_cards), "No PC Card sockets are installed.");
    if (in->system_free_pct < RM_CRITICAL_PCT) { o->degraded = 1; snprintf(o->verdict, sizeof(o->verdict), "System resources are low, which reduces performance."); }
    else if (!in->pagefile_present && in->memory_kib < 128 * 1024) { o->degraded = 1; snprintf(o->verdict, sizeof(o->verdict), "Virtual memory is turned off, which reduces performance."); }
    else snprintf(o->verdict, sizeof(o->verdict), "Your system is configured for optimal performance.");
}
