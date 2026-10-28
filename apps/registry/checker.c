/* sec-sysvc2: Registry Checker in the SYSTEM registry broker (checker.h;
 * research docs/validation/sec-sysvc2.md). The broker owns the machine
 * registry since sec-k06, so its backups and restores run here, as SYSTEM,
 * after the caller's check: listing, backups, restores and repairs are for
 * administrators (the Windows 98 Registry Checker restore and Windows XP
 * System Restore are administrator tools); the daily backup is a SYSTEM job
 * any session's desktop may ask for, made at most once a local day by the
 * broker's own clock (XP's System Restore makes its daily checkpoint as
 * LocalSystem whoever is logged on). No path or time comes from a request.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "checker.h"
#include "broker.h"
#include "../sysrecovery/recovery.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int administrator(const struct rb_identity *c)
{
    if (!c->uid) return 1;
    for (unsigned i = 0; i < c->group_count && i < 8; i++) if (c->groups[i] == 544) return 1;
    return 0;
}
static void say(struct rb_checker *k, const char *line) { if (k->log) k->log(k->context, line); }
void rb_checker_boot(struct rb_checker *k)
{
    k->restored = 0; k->from[0] = k->applied[0] = 0;
    int e = rc_apply_pending(k->root, k->applied);
    int f = rc_autorestore_start(k->root, &k->restored, k->from);
    k->boot_error = e ? e : f;
    char line[160];
    snprintf(line, sizeof(line), "[registry] checker start restore=%s autorestore=%u from=%s result=%d",
             k->applied[0] ? k->applied : "-", k->restored, k->from[0] ? k->from : "-", k->boot_error);
    say(k, line);
}
static int reply_record(uint8_t *out, size_t cap, size_t *used, const struct rc_backup *b)
{
    if (cap < 32 + RC_WIRE) return -EINVAL;
    rb_put32(out + 4, 1); rc_wire_put(b, out + 32); *used = 32 + RC_WIRE;
    return 0;
}
int rb_checker_exchange(struct rb_checker *k, const struct rb_identity *c, unsigned op,
                        const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *used)
{
    if (!k || !c || !in || !out || !used || n < 32 || cap < 32 || rb_get32(in) != 1) return -EINVAL;
    for (unsigned i = 4; i < 32; i++) if (in[i]) return -EINVAL;
    size_t tail = op == RB_CHECKER_BACKUP ? RC_COMMENT : op == RB_CHECKER_RESTORE ? 16 : 0;
    if (n != 32 + tail) return -EINVAL;
    int admin = administrator(c);
    if (op != RB_CHECKER_DAILY && op != RB_CHECKER_STATUS && !admin) return -EPERM;
    memset(out, 0, 32); rb_put32(out, 1); *used = 32;
    char line[160]; int e = 0;
    switch (op) {
    case RB_CHECKER_LIST: {
        struct rc_backup *list = calloc(100, sizeof(*list));
        if (!list) return -ENOMEM;
        int count = rc_list(k->root, list, 100);
        if (count >= 0 && cap < 32 + (size_t)count * RC_WIRE) count = (int)((cap - 32) / RC_WIRE);
        if (count < 0) e = count;
        else {
            rb_put32(out + 4, (uint32_t)count);
            for (int i = 0; i < count; i++) rc_wire_put(&list[i], out + 32 + (size_t)i * RC_WIRE);
            *used = 32 + (size_t)count * RC_WIRE;
        }
        free(list);
        return e;
    }
    case RB_CHECKER_BACKUP: {
        char comment[RC_COMMENT];
        memcpy(comment, in + 32, RC_COMMENT);
        if (comment[RC_COMMENT - 1]) return -EINVAL;
        for (size_t i = 0; comment[i]; i++) if ((unsigned char)comment[i] < 32 || (unsigned char)comment[i] > 126) return -EINVAL;
        int offset = 0; int64_t now = k->now ? k->now(k->context, &offset) : 0;
        struct rc_backup made;
        e = rc_backup(k->root, comment[0] ? comment : NULL, now, k->started && k->started(k->context) == 1, &made);
        snprintf(line, sizeof(line), "[registry] checker backup uid=%u name=%s result=%d", c->uid, e ? "-" : made.name, e);
        say(k, line);
        return e ? e : reply_record(out, cap, used, &made);
    }
    case RB_CHECKER_RESTORE: {
        char name[16];
        memcpy(name, in + 32, 16);
        if (name[15]) return -EINVAL;
        e = rc_stage_restore(k->root, name);
        snprintf(line, sizeof(line), "[registry] checker restore requested uid=%u name=%s result=%d", c->uid, name, e);
        say(k, line);
        return e;
    }
    case RB_CHECKER_FIX: {
        if (cap < 48) return -EINVAL;
        unsigned restored = 0; char from[16];
        e = rc_autorestore(k->root, &restored, from);
        snprintf(line, sizeof(line), "[registry] checker fix uid=%u restored=%u result=%d", c->uid, restored, e);
        say(k, line);
        if (e) return e;
        rb_put32(out + 4, restored); memcpy(out + 32, from, 16); *used = 48;
        return 0;
    }
    case RB_CHECKER_DAILY: {
        if (!k->now) return -EOPNOTSUPP;
        int offset = 0, made = 0; int64_t now = k->now(k->context, &offset);
        e = rc_daily(k->root, now, offset, &made);
        if (e || !made) return e;
        /* The newest backup is the one just made. */
        struct rc_backup list[RC_MAX_BACKUPS + 4];
        if (rc_list(k->root, list, RC_MAX_BACKUPS + 4) < 1) return -EIO;
        snprintf(line, sizeof(line), "[registry] checker daily backup uid=%u session=%u name=%s", c->uid, c->session, list[0].name);
        say(k, line);
        return reply_record(out, cap, used, &list[0]);
    }
    case RB_CHECKER_STATUS: {
        if (cap < 80) return -EINVAL;
        unsigned damaged = 0; char pending[16];
        e = rc_scan(k->root, &damaged);
        if (e) return e;
        if (rc_pending(k->root, pending) <= 0) pending[0] = 0;
        rb_put32(out + 4, damaged); rb_put32(out + 8, k->restored);
        memcpy(out + 32, k->from, 16); memcpy(out + 48, k->applied, 16); memcpy(out + 64, pending, 16);
        *used = 80;
        return 0;
    }
    default: return -EOPNOTSUPP;
    }
}
