/* Private shell/Control Panel model. SPDX-License-Identifier: MIT */
#ifndef DESK_STARTUP_H
#define DESK_STARTUP_H
#include "shell_link.h"
#define DESK_STARTUP_MAX 128u
#define DESK_STARTUP_FOLDER "/WINDOWS/Start Menu/Programs/StartUp"
/* First six sources are registry keys, source 6 is the Startup folder. */
struct desk_startup_entry {
    struct desk_launch_record launch;
    char name[1021], command[2048];
    unsigned source;
    int enabled;
    int error;
};
/* The input path owns the latch, so late input and queue drains count too. */
struct desk_startup_gate { uint64_t deadline; int done, skip; };
static inline void desk_startup_key(struct desk_startup_gate *s,const struct ciuki_input_event *e)
{
    if (!s->done && e->type==CIUKI_INPUT_KEY && e->value &&
        (e->code==CIUKI_KEY_LEFTSHIFT || e->code==CIUKI_KEY_RIGHTSHIFT)) s->skip=1;
}
static inline int desk_startup_due(struct desk_startup_gate *s,int presented,int empty,uint64_t now)
{
    if (s->done || !presented) return 0;
    if (!s->deadline) s->deadline=now+1000000000ull;
    if (!empty || now<s->deadline) return 0;
    s->done=1; return 1;
}
const char *desk_startup_source(unsigned);
int desk_startup_command(const char *, struct desk_launch_record *);
int ciuki_win32_startup_list(struct desk_startup_entry *, unsigned);
int ciuki_win32_startup_consume(const struct desk_startup_entry *);
int desk_startup_list_at(const char *folder, const char *registry, struct desk_startup_entry *, unsigned);
int desk_startup_enable_at(const char *registry, const struct desk_startup_entry *, int);
int desk_startup_execute(struct desk_startup_entry *, unsigned, int skip,
                         int (*consume)(const struct desk_startup_entry *),
                         int (*launch)(const struct desk_launch_record *));
int desk_startup_begin(int skip);
#endif
