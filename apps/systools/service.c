/* SY-7 scheduler service step. SPDX-License-Identifier: MIT */
#include "systools.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int ss_split(const char *command, char *program, size_t pcap, char *argument, size_t acap)
{
    static const char prefix[] = "/Programs/";
    while (*command == ' ') command++;
    if (!strncmp(command, prefix, sizeof(prefix) - 1)) command += sizeof(prefix) - 1;
    size_t n = 0;
    while (command[n] && command[n] != ' ') n++;
    if (!n || n >= pcap) return -EINVAL;
    memcpy(program, command, n); program[n] = 0;
    const char *rest = command + n;
    while (*rest == ' ') rest++;
    size_t m = strlen(rest);
    while (m && rest[m - 1] == ' ') m--;
    if (m >= acap) return -EINVAL;
    memcpy(argument, rest, m); argument[m] = 0;
    return 0;
}
static void reload(struct ss_service *s, const struct ss_host *h)
{
    size_t length = 0;
    int e = h->load(h->ctx, s->text, SS_TEXT - 1, &length);
    if (e) {
        if (!s->loaded) { st_init(&s->store); s->loaded = 1; s->saved_length = 0; s->saved[0] = 0; }
        return;                              /* no file or unreadable: keep what is in memory */
    }
    s->text[length] = 0;
    if (s->loaded && length == s->saved_length && !memcmp(s->text, s->saved, length)) return;
    /* A changed file keeps the runs this service has recorded only if it carries them: the file is the truth. */
    st_load(&s->store, s->text);
    if (!s->loaded) for (unsigned i = 0; i < s->store.count; i++)
        if (s->store.t[i].last_result == ST_RUNNING) s->store.t[i].last_result = -EINTR;
    memcpy(s->saved, s->text, length + 1); s->saved_length = length; s->loaded = 1;
}
static int persist(struct ss_service *s, const struct ss_host *h)
{
    int n = st_save(&s->store, s->text, SS_TEXT);
    if (n < 0) return n;
    int e = h->save(h->ctx, s->text, (size_t)n);
    if (e) return e;
    memcpy(s->saved, s->text, (size_t)n + 1); s->saved_length = (size_t)n;
    return 0;
}
int ss_poll(struct ss_service *s, const struct ss_host *h)
{
    reload(s, h);
    int64_t now = h->now_minutes(h->ctx);
    struct st_conditions c = {0, 2};
    if (h->conditions) h->conditions(h->ctx, &c);
    int changed = 0, launched = 0;
    for (unsigned k = 0; k < ST_MAX; k++) {
        if (!s->running[k].token) continue;
        int result = 0;
        if (h->result && h->result(h->ctx, s->running[k].token, &result)) {
            for (unsigned i = 0; i < s->store.count; i++) {
                struct st_task *t = &s->store.t[i];
                if (!strcmp(t->name, s->running[k].name) && !strcmp(t->command, s->running[k].command) && t->last_run == s->running[k].when) {
                    t->last_result = result; changed = 1;
                }
            }
            s->running[k].token = 0;
        } else {
            int idle = c.idle_minutes >= (s->running[k].idle_minutes ? s->running[k].idle_minutes : 1);
            int stop_idle = (s->running[k].conditions & ST_STOP_IDLE) && s->running[k].was_idle && !idle;
            int stop_battery = (s->running[k].conditions & ST_STOP_BATTERY) && s->running[k].was_ac == 1 && c.ac == 0;
            if (!s->running[k].stopping && h->stop && (stop_idle || stop_battery)) {
                h->stop(h->ctx, s->running[k].token); s->running[k].stopping = 1;
            }
            s->running[k].was_idle = idle;
            if (c.ac != 2) s->running[k].was_ac = c.ac;
        }
    }
    /* At-start tasks remain eligible while blocked, but only once per service lifetime. */
    unsigned due[ST_MAX];
    unsigned n = st_due(&s->store, now, 1, due, ST_MAX);
    char manual[32] = {0};
    if (h->manual && h->manual(h->ctx, manual)) {
        manual[31] = 0;
        for (unsigned i = 0; i < s->store.count; i++) if (!strcmp(s->store.t[i].name, manual)) {
            int found = 0; for (unsigned j = 0; j < n; j++) if (due[j] == i) found = 1;
            if (!found && n < ST_MAX) due[n++] = i;
        }
    }
    for (unsigned i = 0; i < n; i++) {
        struct st_task *t = &s->store.t[due[i]];
        int busy = 0; unsigned slot = ST_MAX;
        for (unsigned k = 0; k < ST_MAX; k++) {
            if (s->running[k].token && !strcmp(s->running[k].name, t->name)) busy = 1;
            if (!s->running[k].token && slot == ST_MAX) slot = k;
        }
        int force = manual[0] && !strcmp(manual, t->name);
        if (!force && t->kind == ST_AT_START) for (unsigned k = 0; k < s->startup_count; k++) if (!strcmp(s->startup_done[k], t->name)) busy = 1;
        if (busy || slot == ST_MAX) { s->deferred++; continue; }
        unsigned wait_slot = ST_MAX;
        for (unsigned k = 0; k < ST_MAX; k++) if (!strcmp(s->waiting[k].name, t->name)) { wait_slot = k; break; }
        if (!force && !st_allowed(t, &c)) {
            if ((t->conditions & ST_IDLE) && t->idle_deadline && c.idle_minutes < (t->idle_minutes ? t->idle_minutes : 1)) {
                if (wait_slot == ST_MAX) for (unsigned k = 0; k < ST_MAX; k++) if (!s->waiting[k].name[0]) { wait_slot = k; break; }
                if (wait_slot < ST_MAX) {
                    if (!s->waiting[wait_slot].name[0] || s->waiting[wait_slot].last_run != t->last_run || now < s->waiting[wait_slot].since) {
                        snprintf(s->waiting[wait_slot].name, 32, "%s", t->name);
                        s->waiting[wait_slot].since = now; s->waiting[wait_slot].last_run = t->last_run;
                    }
                    if (now - s->waiting[wait_slot].since >= (int64_t)t->idle_deadline) {
                        st_mark_run(&s->store, due[i], now, -ETIMEDOUT); changed = 1;
                        s->waiting[wait_slot].name[0] = 0;
                        if (t->kind == ST_AT_START && s->startup_count < ST_MAX) snprintf(s->startup_done[s->startup_count++], 32, "%s", t->name);
                        continue;
                    }
                }
            }
            s->deferred++; continue;
        }
        if (wait_slot < ST_MAX) s->waiting[wait_slot].name[0] = 0;
        char program[48], argument[161];
        int pid = ss_split(t->command, program, sizeof(program), argument, sizeof(argument));
        if (!pid) pid = h->launch(h->ctx, program, argument);
        if (pid == -EAGAIN) { s->deferred++; continue; }
        if (t->kind == ST_AT_START && s->startup_count < ST_MAX) snprintf(s->startup_done[s->startup_count++], 32, "%s", t->name);
        if (pid > 0) {
            launched++; s->launched++;
            st_mark_run(&s->store, due[i], now, h->result ? ST_RUNNING : 0);
            if (h->result) {
                snprintf(s->running[slot].name, 32, "%s", t->name);
                snprintf(s->running[slot].command, ST_COMMAND, "%s", t->command);
                s->running[slot].token = pid; s->running[slot].when = now;
                s->running[slot].conditions = t->conditions; s->running[slot].idle_minutes = t->idle_minutes; s->running[slot].stopping = 0;
                s->running[slot].was_idle = c.idle_minutes >= (t->idle_minutes ? t->idle_minutes : 1);
                s->running[slot].was_ac = c.ac;
            }
        } else { s->failed++; st_mark_run(&s->store, due[i], now, pid ? pid : -EINVAL); }
        changed = 1;
    }
    s->started = 1;
    if (changed || s->store.count) {
        /* Retry a failed save on the next poll, including a finished last result. */
        int nbytes = st_save(&s->store, s->text, SS_TEXT);
        if (nbytes < 0) return nbytes;
        if ((size_t)nbytes != s->saved_length || memcmp(s->text, s->saved, (size_t)nbytes)) {
            int e = persist(s, h); if (e) return e;
        }
    }
    return launched;
}
