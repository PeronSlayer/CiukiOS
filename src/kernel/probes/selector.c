/* Production selector and phase dispatch, independently host-testable.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/process.h>
#include "selector.h"

static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
#define SELECT_SERVER_STANDIN (1u << 30)
#define SELECT_SERVER_DESKTOP (1u << 31)


/* F2 crash-isolation additionally accepts [server=desktop|standin] last. */
bool probes_parse_selector(const char *s, unsigned len, uint32_t boot_flags, struct probe_selection *selection)
{
    if (len < 3 || len > 64 || s[0] != 'f' || (s[1] != '0' && s[1] != '1' && s[1] != '2') || s[2] != ':')
        return false;
    for (unsigned j = 0; j < len; j++)
        if ((uint8_t)s[j] < 32 || (uint8_t)s[j] > 126)
            return false;
    struct probe_selection parsed = { .phase = (unsigned)(s[1] - '0') };
    unsigned i = 3, k = 0;
    while (i < len && s[i] != ' ' && k + 1 < sizeof(parsed.probe))
        parsed.probe[k++] = s[i++];
    parsed.probe[k] = 0;
    if (!k || i >= len || s[i] != ' ')
        return false;
    static const char *const f0_names[] = {
        "boot", "bootinfo", "allocator", "protection", "isolation", "preempt",
        "localfault", "syslife", "panic", "fpu", "runner", "all", "core"
    };
    static const char *const f1_names[] = {
        "registry", "input", "input-fault", "framebuffer", "ata", "ata-fault",
        "partition", "fat-read", "fat-write", "cache", "mount-crash", "safe", "bootlog", "all", "core"
    };
    static const char *const f2_names[] = {
        "elf-load", "spawn-wait", "fd-table", "mmap", "signals-fault",
        "threads-wait", "crash-isolation", "libc-smoke", "app-gate"
    };
    const char *const *names = parsed.phase == 2 ? f2_names : parsed.phase == 1 ? f1_names : f0_names;
    unsigned count = parsed.phase == 2 ? ARRAY_SIZE(f2_names) : parsed.phase == 1 ? ARRAY_SIZE(f1_names) : ARRAY_SIZE(f0_names);
    bool known = false;
    for (unsigned n = 0; n < count; n++)
        if (!strncmp(parsed.probe, names[n], sizeof(parsed.probe)))
            known = true;
    if (!known)
        return false;
    i++;
    if (len - i < 12 || strncmp(s + i, "run=", 4) != 0)
        return false;
    for (unsigned j = 0; j < 8; j++) {
        if (!is_hex(s[i + 4 + j]))
            return false;
        parsed.run[j] = s[i + 4 + j];
    }
    parsed.run[8] = 0;
    i += 12;
    /* optional suffixes, in order, each at most once */
    if (len - i >= 14 && strncmp(s + i, " platform=e500", 14) == 0) {
        if (!(boot_flags & CBI_F_SMBIOS_QEMU) || !(boot_flags & CBI_F_INPUT_FORCED))
            return false;
        parsed.flags |= CBI_F_INPUT_FORCED;
        i += 14;
    }
    if (len - i >= 7 && strncmp(s + i, " safe=1", 7) == 0) {
        if (!(boot_flags & CBI_F_SMBIOS_QEMU) || !(boot_flags & CBI_F_SAFE_MODE))
            return false;
        parsed.flags |= CBI_F_SAFE_MODE;
        i += 7;
    }
    if (len - i == 15 && parsed.phase == 2 && !strncmp(parsed.probe, "crash-isolation", sizeof(parsed.probe))) {
        if (!strncmp(s + i, " server=desktop", 15)) parsed.flags |= SELECT_SERVER_DESKTOP;
        else if (!strncmp(s + i, " server=standin", 15)) parsed.flags |= SELECT_SERVER_STANDIN;
        else return false;
        i += 15;
    }
    if (i != len)
        return false;
    *selection = parsed;
    return true;
}

/* Probe-only policy; no new boot-info bits or public ABI. Safe/text always
 * retain the fallback. An explicit desktop with a missing payload fails at
 * launch, whereas automatic selection uses payload availability. */
int probes_crash_server(const char *s, unsigned len, uint32_t flags,
                        bool lfb, bool payload)
{
    struct probe_selection selection;
    if (!probes_parse_selector(s, len, flags, &selection)) return -EINVAL;
    if ((flags & CBI_F_SAFE_MODE) || !lfb || (selection.flags & SELECT_SERVER_STANDIN)) return 0;
    return !!((selection.flags & SELECT_SERVER_DESKTOP) || payload);
}

void probes_dispatch(const struct probe_selection *selection,
                     const struct probe_tables *tables, const struct probe_hooks *hooks)
{
    const char *probe = selection->probe;
    bool all = !strncmp(probe, "all", 4);
    bool core = !strncmp(probe, "core", 5);
    if (selection->phase == 2) {
        for (unsigned i = 0; i < tables->f2_count; i++) {
            const struct ciuki_f2_probe *p = &tables->f2[i];
            if (!strncmp(probe, p->name, sizeof(selection->probe))) {
                hooks->app_begin(selection);
                p->run();
                hooks->app_end();
                return;
            }
        }
        rec_emit(probe, "BEGIN", 0);
        rec_emit(probe, "READY", "table=f2 installed=%u", tables->f2_count);
        rec_emit(probe, "ERROR", "status=not_run reason=missing_probe");
        return;
    }
    const struct probe_def *table = selection->phase == 1 ? tables->f1 : tables->f0;
    unsigned count = selection->phase == 1 ? tables->f1_count : tables->f0_count;
    unsigned ran = 0;
    for (unsigned i = 0; i < count; i++) {
        if (all || core || !strncmp(probe, table[i].name, sizeof(selection->probe))) {
            int failed = table[i].fn();
            ran++;
            if (failed && (all || core)) {
                for (unsigned j = i + 1; j < count; j++)
                    rec_emit(table[j].name, "NOT_RUN", "reason=prerequisite_failed after=%s", table[i].name);
                if (!selection->phase && all)
                    rec_emit("panic", "NOT_RUN", "reason=prerequisite_failed after=%s", table[i].name);
                return;
            }
        }
    }
    if (!selection->phase && (all || !strncmp(probe, "panic", 6))) {
        hooks->panic();
        ran++;
    }
    if (!ran && selection->phase == 1) {
        rec_emit(probe, "BEGIN", 0);
        rec_emit(probe, "READY", "table=f1 installed=%u", count);
        rec_emit(probe, "ERROR", "status=not_run reason=missing_probe");
    }
    if (!ran && !selection->phase)
        klog("[selector] unknown probe '%s'; no probe runs", probe);
}
