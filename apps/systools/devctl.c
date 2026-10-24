/* CP-19 Device Manager actions and CP-2 Add New Hardware wizard. SPDX-License-Identifier: MIT */
#include "systools.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dc_init(struct dc_store *s) { memset(s, 0, sizeof(*s)); }
static struct dc_entry *find(struct dc_store *s, uint32_t id)
{
    for (unsigned i = 0; i < s->count; i++) if (s->e[i].id == id) return &s->e[i];
    return NULL;
}
static void drop_if_default(struct dc_store *s, struct dc_entry *e)
{
    if (e->disabled || e->removed || e->driver[0]) return;
    unsigned i = (unsigned)(e - s->e);
    memmove(&s->e[i], &s->e[i + 1], (s->count - i - 1) * sizeof(s->e[0]));
    s->count--;
}
enum dc_status dc_status_of(const struct dc_store *s, uint32_t id)
{
    for (unsigned i = 0; i < s->count; i++)
        if (s->e[i].id == id) return s->e[i].removed ? DC_REMOVED : s->e[i].disabled ? DC_DISABLED : DC_ACTIVE;
    return DC_ACTIVE;
}
const char *dc_driver_override(const struct dc_store *s, uint32_t id)
{
    for (unsigned i = 0; i < s->count; i++) if (s->e[i].id == id && s->e[i].driver[0]) return s->e[i].driver;
    return NULL;
}
int dc_critical(const struct ciuki_device_record *d)
{ return d->class_id == CIUKI_DEVICE_SYSTEM || d->class_id == CIUKI_DEVICE_DISK; }
unsigned dc_resource_count(const struct dc_store *s, const struct ciuki_device_record *d)
{ return dc_status_of(s, d->id) == DC_ACTIVE ? d->resource_count : 0; }

static int valid_driver(const char *n)
{
    size_t len = strlen(n);
    if (!len || len >= sizeof(((struct dc_entry *)0)->driver)) return 0;
    for (size_t i = 0; i < len; i++) if ((unsigned char)n[i] <= ' ' || n[i] == '=' || (unsigned char)n[i] > 126) return 0;
    return 1;
}
static void say(struct dc_result *r, int error, const char *text)
{ r->error = error; snprintf(r->message, sizeof(r->message), "%s", text); }

void dc_apply(struct dc_store *s, const struct ciuki_device_record *d, int action, const char *arg, struct dc_result *r)
{
    memset(r, 0, sizeof(*r));
    if (action < DC_DISABLE || action > DC_UPDATE_DRIVER) { say(r, -EINVAL, "Unknown action."); return; }
    if (d->state == CIUKI_DEVICE_ABSENT && action != DC_ENABLE) { say(r, -ENODEV, "The device is not present."); return; }
    struct dc_entry *e = find(s, d->id);
    if (action == DC_DISABLE || action == DC_REMOVE) {
        if (dc_critical(d)) {
            say(r, -EPERM, action == DC_DISABLE ? "This device is required by the system and cannot be disabled."
                                                : "This device is required by the system and cannot be removed.");
            return;
        }
    }
    if (!e && (action != DC_ENABLE)) {
        if (s->count >= DC_MAX) { say(r, -ENOSPC, "Too many device settings."); return; }
        e = &s->e[s->count++];
        memset(e, 0, sizeof(*e)); e->id = d->id;
    }
    int in_use = d->state == CIUKI_DEVICE_READY;
    switch (action) {
    case DC_DISABLE:
        if (e->disabled && !e->removed) { say(r, 0, "The device is already disabled."); return; }
        e->disabled = 1; e->removed = 0;
        r->needs_restart = in_use; r->warn = in_use;
        say(r, 0, in_use ? "The device is disabled. Restart to release its resources." : "The device is disabled.");
        break;
    case DC_ENABLE:
        if (!e || (!e->disabled && !e->removed)) { say(r, 0, "The device is already enabled."); return; }
        e->disabled = 0; e->removed = 0;
        r->needs_restart = 1;
        say(r, 0, "The device is enabled. Restart to start it.");
        break;
    case DC_REMOVE:
        e->removed = 1; e->disabled = 0;
        r->needs_restart = in_use; r->warn = 1;
        say(r, 0, "The device is removed. It is detected again at the next restart.");
        break;
    case DC_UPDATE_DRIVER:
        if (!arg || !valid_driver(arg)) { drop_if_default(s, e); say(r, -EINVAL, "The driver name is not valid."); return; }
        snprintf(e->driver, sizeof(e->driver), "%s", arg);
        r->needs_restart = 1;
        say(r, 0, "The driver is updated. Restart to load it.");
        break;
    }
    if (e) drop_if_default(s, e);
    s->generation++;
    if (r->needs_restart) s->restart_needed = 1;
}
void dc_restart(struct dc_store *s)
{
    for (unsigned i = 0; i < s->count;) {
        if (s->e[i].removed) s->e[i].removed = 0;
        if (!s->e[i].disabled && !s->e[i].driver[0]) { memmove(&s->e[i], &s->e[i + 1], (s->count - i - 1) * sizeof(s->e[0])); s->count--; }
        else i++;
    }
    s->restart_needed = 0; s->generation++;
}
int dc_save(const struct dc_store *s, char *out, size_t cap)
{
    size_t at = 0;
    int w = snprintf(out, cap, "version=1\n");
    if (w < 0 || (size_t)w >= cap) return -ENOSPC;
    at += (size_t)w;
    for (unsigned i = 0; i < s->count; i++) {
        w = snprintf(out + at, cap - at, "device=%x disabled=%u removed=%u driver=%s\n", s->e[i].id, s->e[i].disabled, s->e[i].removed,
                     s->e[i].driver[0] ? s->e[i].driver : "-");
        if (w < 0 || (size_t)w >= cap - at) return -ENOSPC;
        at += (size_t)w;
    }
    return (int)at;
}
int dc_load(struct dc_store *s, const char *text)
{
    dc_init(s);
    while (*text) {
        const char *eol = strchr(text, '\n');
        size_t len = eol ? (size_t)(eol - text) : strlen(text);
        char line[160];
        if (len < sizeof(line)) {
            memcpy(line, text, len); line[len] = 0;
            unsigned id, dis, rem; char drv[64];
            if (!strncmp(line, "device=", 7) && sscanf(line, "device=%x disabled=%u removed=%u driver=%63s", &id, &dis, &rem, drv) == 4 &&
                dis <= 1 && rem <= 1 && s->count < DC_MAX && !find(s, id) && (!strcmp(drv, "-") || valid_driver(drv))) {
                struct dc_entry *e = &s->e[s->count++];
                memset(e, 0, sizeof(*e));
                e->id = id; e->disabled = (uint8_t)dis; e->removed = (uint8_t)rem;
                if (strcmp(drv, "-")) snprintf(e->driver, sizeof(e->driver), "%s", drv);
            }
        }
        text += len + (eol ? 1 : 0);
        if (!eol) break;
    }
    return (int)s->count;
}

/* ---- Add New Hardware ---- */
static int needs_driver(const struct ciuki_device_record *d, const struct dc_store *s)
{
    if (dc_status_of(s, d->id) != DC_ACTIVE) return 0;
    return d->state == CIUKI_DEVICE_QUARANTINED || d->state == CIUKI_DEVICE_FIRMWARE_FIRST;
}
void aw_init(struct aw_wizard *w, const struct aw_model *catalog, unsigned count, struct dc_store *store, const struct ciuki_devices_info *devices)
{
    memset(w, 0, sizeof(*w));
    w->catalog = catalog; w->catalog_count = count > AW_CATALOG_MAX ? AW_CATALOG_MAX : count;
    w->store = store; w->devices = devices; w->automatic = 1; w->model = -1; w->class_id = CIUKI_DEVICE_CLASSES;
    w->page = AW_WELCOME;
}
void aw_choose_search(struct aw_wizard *w, int automatic) { w->automatic = automatic != 0; }
int aw_detect(struct aw_wizard *w)
{
    w->found_count = 0;
    unsigned n = w->devices ? (w->devices->count > CIUKI_DEVICES_MAX ? CIUKI_DEVICES_MAX : w->devices->count) : 0;
    for (unsigned i = 0; i < n && w->found_count < AW_FOUND_MAX; i++)
        if (needs_driver(&w->devices->devices[i], w->store)) w->found[w->found_count++] = w->devices->devices[i].id;
    return (int)w->found_count;
}
int aw_select_class(struct aw_wizard *w, unsigned class_id)
{
    if (class_id >= CIUKI_DEVICE_CLASSES) return -EINVAL;
    w->class_id = class_id; w->model = -1;
    return 0;
}
unsigned aw_models(const struct aw_wizard *w, unsigned *out, unsigned cap)
{
    unsigned n = 0;
    for (unsigned i = 0; i < w->catalog_count && n < cap; i++) if (w->catalog[i].class_id == w->class_id) out[n++] = i;
    return n;
}
int aw_select_model(struct aw_wizard *w, int index)
{
    if (index < 0 || (unsigned)index >= w->catalog_count || w->catalog[index].class_id != w->class_id) return -EINVAL;
    w->model = index;
    return 0;
}
const char *aw_title(enum aw_page p)
{
    static const char *const t[] = { "Add New Hardware Wizard", "Do you want Windows to search for your new hardware?",
        "Searching for new hardware", "New hardware found", "No new hardware found", "Hardware types", "Select the manufacturer and model",
        "Finish", "Done" };
    return p <= AW_DONE ? t[p] : "";
}
static void push(struct aw_wizard *w, enum aw_page to)
{ if (w->depth < sizeof(w->history) / sizeof(w->history[0])) w->history[w->depth++] = w->page; w->page = to; }
/* Applies the chosen model to the first target device (or records an override for a manual choice). */
static int finish(struct aw_wizard *w)
{
    if (w->model < 0) return -EINVAL;
    const struct aw_model *m = &w->catalog[w->model];
    struct dc_result r;
    const struct ciuki_device_record *target = NULL;
    for (unsigned k = 0; k < w->found_count && !target; k++)
        for (unsigned i = 0; w->devices && i < w->devices->count && i < CIUKI_DEVICES_MAX; i++)
            if (w->devices->devices[i].id == w->found[k] && w->devices->devices[i].class_id == m->class_id) { target = &w->devices->devices[i]; break; }
    if (!target && w->devices)
        for (unsigned i = 0; i < w->devices->count && i < CIUKI_DEVICES_MAX; i++)
            if (w->devices->devices[i].class_id == m->class_id && needs_driver(&w->devices->devices[i], w->store)) { target = &w->devices->devices[i]; break; }
    if (!target) { snprintf(w->summary, sizeof(w->summary), "No %s device is waiting for a driver.", m->name); return -ENODEV; }
    dc_apply(w->store, target, DC_UPDATE_DRIVER, m->driver, &r);
    if (r.error) { snprintf(w->summary, sizeof(w->summary), "%s", r.message); return r.error; }
    w->restart = r.needs_restart;
    snprintf(w->summary, sizeof(w->summary), "%s will use the driver %s after restart.", target->name, m->driver);
    return 0;
}
int aw_next(struct aw_wizard *w)
{
    switch (w->page) {
    case AW_WELCOME: push(w, AW_SEARCH); return 0;
    case AW_SEARCH:
        if (!w->automatic) { push(w, AW_CLASS); return 0; }
        push(w, aw_detect(w) > 0 ? AW_FOUND : AW_NONE_FOUND);   /* AW_DETECT is the UI's transient progress page */
        return 0;
    case AW_DETECT: return 0;
    case AW_FOUND:
        if (!w->found_count) return -EINVAL;
        { /* the first found device's class selects the driver list */
            for (unsigned i = 0; w->devices && i < w->devices->count && i < CIUKI_DEVICES_MAX; i++)
                if (w->devices->devices[i].id == w->found[0]) { w->class_id = w->devices->devices[i].class_id; break; }
        }
        push(w, AW_MODEL); return 0;
    case AW_NONE_FOUND: push(w, AW_CLASS); return 0;
    case AW_CLASS: if (w->class_id >= CIUKI_DEVICE_CLASSES) return -EINVAL; push(w, AW_MODEL); return 0;
    case AW_MODEL: if (w->model < 0) return -EINVAL; push(w, AW_FINISH); return 0;
    case AW_FINISH: { int e = finish(w); if (e) return e; w->page = AW_DONE; return 0; }
    case AW_DONE: return 0;
    }
    return -EINVAL;
}
int aw_back(struct aw_wizard *w)
{
    if (w->page == AW_DONE || !w->depth) return -EINVAL;
    w->page = w->history[--w->depth];
    return 0;
}
