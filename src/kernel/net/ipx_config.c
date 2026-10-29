/* One atomic machine setting; no network gates are acquired by SYSTEM.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/ipx_config.h>
static unsigned frame_type=CIUKI_IPX_FRAME_AUTO;
static bool writing;
bool net_ipx_config_lock(void) { return !__atomic_exchange_n(&writing,true,__ATOMIC_ACQUIRE); }
void net_ipx_config_unlock(void) { __atomic_store_n(&writing,false,__ATOMIC_RELEASE); }
unsigned net_ipx_frame_type(void) { return __atomic_load_n(&frame_type,__ATOMIC_ACQUIRE); }
void net_ipx_frame_publish(unsigned value)
{ if (value<=CIUKI_IPX_FRAME_AUTO) __atomic_store_n(&frame_type,value,__ATOMIC_RELEASE); }
