/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_CONTROL_PROGRAMS_H
#define CIUKI_CONTROL_PROGRAMS_H
#include "../desktop/shell_link.h"
struct cp_program_store {
    const char *root; /* empty on CiukiOS, fixture root on the host */
    void *ctx;
    unsigned overflow; /* bounded inventory omitted entries; refresh after removal */
    int (*registry_list)(void *,struct desk_uninstall_entry *,unsigned);
    int (*registry_check)(void *,const char *); /* exact key presence; avoids bounded-list ambiguity */
    int (*registry_run)(void *,const char *);
    int (*game)(void *,unsigned,int,uint32_t *); /* query: 0 absent, 1 present, 2 pending cleanup, or -errno */
    void (*log)(void *,const char *);
    int (*persist)(void *); /* durable icon/settings cleanup before deleting records */
    void (*forget_icon)(void *,const char *);
    /* sec-user-04: the session's committed profile root (USERPROFILE, e.g.
     * /Windows/Profiles/u000003e8), NULL without a profile session. Its
     * Start Menu/Programs and Documents/Desktop (contract 8.2) are searched
     * and cleaned beside the machine folders. A location, never authority. */
    const char *profile;
};
int cp_program_list(struct cp_program_store *,struct desk_uninstall_entry *,unsigned);
int cp_program_remove(struct cp_program_store *,const struct desk_uninstall_entry *);
void cp_program_record(struct cp_program_store *,const struct desk_uninstall_entry *,int);
int cp_program_installer(const char *);
int cp_program_contains(const char *,const char *);
#endif
