/* Machine IPX framing intent, published by the SYSTEM service.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_IPX_CONFIG_H
#define CIUKI_IPX_CONFIG_H
#include <ciuki/abi.h>
#include <stddef.h>
#include <stdbool.h>
#define IPX_CONFIG_PATH "C:\\SYSTEM\\SETTINGS\\ipx.cfg"
#define IPX_CONFIG_BYTES 13u
static inline bool ipx_config_parse(const char *text, size_t size, unsigned *value)
{
    static const char prefix[] = "frame_type=";
    if (!text || size != IPX_CONFIG_BYTES) return false;
    for (unsigned i=0; i<sizeof(prefix)-1; i++) if (text[i]!=prefix[i]) return false;
    if (text[11]<'0' || text[11]>'4' || text[12]!='\n') return false;
    *value=(unsigned)(text[11]-'0'); return true;
}
unsigned net_ipx_frame_type(void);
void net_ipx_frame_publish(unsigned);
bool net_ipx_config_lock(void);
void net_ipx_config_unlock(void);
#endif
