/* Explicit publication after qualified initialization. GPL-2.0-only */
#ifndef CIUKI_NDIS_NETIF_H
#define CIUKI_NDIS_NETIF_H
#include "native.h"
#include <ciuki/net.h>
extern const struct netif_driver net_ndis;
int ndis_netif_publish(struct ndis_native *);
void ndis_netif_tx_done(struct ndis_native *,uint32_t token,int status);
#endif
