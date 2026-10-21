/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_CONTROL_PROGRAMS_H
#define CIUKI_CONTROL_PROGRAMS_H
#include "../desktop/shell_link.h"
struct cp_program_store {
    const char *root; /* empty on CiukiOS, fixture root on the host */
    void *ctx;
    int (*registry_list)(void *,struct desk_uninstall_entry *,unsigned);
    int (*registry_run)(void *,const char *);
    int (*game)(void *,unsigned,int,uint32_t *); /* query returns 1 present, 0 absent, or -errno */
    void (*log)(void *,const char *);
    void (*forget_icon)(void *,const char *);
};
int cp_program_list(struct cp_program_store *,struct desk_uninstall_entry *,unsigned);
int cp_program_remove(struct cp_program_store *,const struct desk_uninstall_entry *);
void cp_program_record(struct cp_program_store *,const struct desk_uninstall_entry *,int);
int cp_program_installer(const char *);
int cp_program_contains(const char *,const char *);
#endif
