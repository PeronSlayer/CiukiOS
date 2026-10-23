/* Local-only package importer. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DRIVER_PACK_H
#define CIUKI_DRIVER_PACK_H
#include "../../src/kernel/ndis/package.h"
#define DP_INF_LIMIT (128u*1024)
#define DP_PATH 256u
struct dp_plan {
    struct ndp_manifest manifest;
    char source[NDP_FILES][192];
};
/* Fetch relative INF bytes from the selected directory. Caller frees output. */
typedef int (*dp_fetch)(void *,const char *,uint8_t **,size_t *);
int dp_parse(struct dp_plan *,const char *inf,const struct ndp_device *,unsigned nt_minor,dp_fetch,void *,char reason[160]);
/* Native adapter. The selected directory can be on local FAT or a mounted CD.
 * Scan returns individual candidates; choosing one never chooses a duplicate. */
int dp_scan(const char *root,char names[][192],unsigned capacity);
int dp_preview(struct dp_plan *,const char *root,const char *inf,const struct ndp_device *,char reason[160]);
int dp_install(const struct dp_plan *,const char *root,const char *store,char reason[160]);
int dp_uninstall(const struct ndp_device *,const char *store,char reason[160]);
typedef int (*dp_busy_check)(void *,const struct ndp_device *);
int dp_uninstall_checked(const struct ndp_device *,const char *store,dp_busy_check,void *,char reason[160]);
int dp_installed(const struct ndp_device *,const char *store);
/* Test-only failure injection is supplied through this callback, NULL in production. */
typedef int (*dp_fault)(const char *operation,void *);
void dp_test_fault(dp_fault,void *);
#endif
