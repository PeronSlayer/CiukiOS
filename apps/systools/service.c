/* SY-7 scheduler service step. SPDX-License-Identifier: MIT */
#include "systools.h"
#include <errno.h>
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
    memcpy(s->saved, s->text, length + 1); s->saved_length = length; s->loaded = 1;
}
int ss_poll(struct ss_service *s, const struct ss_host *h)
{
    reload(s, h);
    int64_t now = h->now_minutes(h->ctx);
    unsigned due[ST_MAX];
    unsigned n = st_due(&s->store, now, !s->started, due, ST_MAX);
    s->started = 1;
    int launched = 0;
    unsigned failed_before = s->failed;
    for (unsigned i = 0; i < n; i++) {
        char program[48], argument[161];
        int pid = ss_split(s->store.t[due[i]].command, program, sizeof(program), argument, sizeof(argument));
        if (!pid) pid = h->launch(h->ctx, program, argument);
        if (pid > 0) { launched++; s->launched++; st_mark_run(&s->store, due[i], now, 0); }
        else if (pid == -EAGAIN) { s->deferred++; continue; }          /* the launcher is busy: still due, retried at the next step */
        else { s->failed++; st_mark_run(&s->store, due[i], now, pid ? pid : -EINVAL); }   /* the failure is recorded, not retried */
    }
    if (launched || s->failed != failed_before) {
        int w = st_save(&s->store, s->text, SS_TEXT);
        if (w > 0 && h->save(h->ctx, s->text, (size_t)w) == 0) { memcpy(s->saved, s->text, (size_t)w + 1); s->saved_length = (size_t)w; }
        else if (w < 0) return w;
    }
    return launched;
}
