/* Same fixture logic in the guest and the production shell host harness. */
#ifndef CIUKI_W32_RESTART_EXERCISE_H
#define CIUKI_W32_RESTART_EXERCISE_H
#include "controller.h"
struct w32_restart_env {
    int (*read)(const char *,void *,unsigned); /* byte count, -1 on error */
    int (*write)(const char *,const void *,unsigned);
    int (*remove)(const char *);
    int (*exists)(const char *); /* 1 present, 0 ENOENT, -1 other error */
    int (*dde)(unsigned raw,const char *command,char *groups,unsigned cap);
    /* Set a string when command != NULL; otherwise return RegQueryValue error. */
    unsigned (*registry)(unsigned queue,const char *name,const char *command);
    int (*session)(unsigned flags,unsigned veto,struct w32_restart_result *);
    int (*park)(struct w32_restart_result *);
};
int w32_restart_exercise(struct w32_restart_result *,const struct w32_restart_env *);
int w32_restart_helper(struct w32_restart_result *,const struct w32_restart_env *,unsigned);
#endif
