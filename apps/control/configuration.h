/* Destination-aware configuration shared by Control Panel and Setup.
 * Private application/library boundary, not an installed SDK header.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_CONTROL_CONFIGURATION_H
#define CIUKI_CONTROL_CONFIGURATION_H
#include "../../sdk/libsetup/include/setup_inf.h"
#include <ciuki/net_config.h>
struct cp_configuration_store {
    void *ctx;
    int (*read)(void *,const char *,void *,unsigned,unsigned *);
    int (*write)(void *,const char *,const void *,unsigned);
    int (*mkdir)(void *,const char *);
    int (*network)(void *,struct net_config_identity [2]);
    int (*clock)(void *,int64_t,int16_t,bool);
    int64_t (*now)(void *);
};
int cp_configuration_write(const struct cp_configuration_store *,const struct setup_answers *);
#endif
