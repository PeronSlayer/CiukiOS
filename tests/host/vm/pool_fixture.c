/* Real upstream allocator for isolated device/VM fixtures. No transport or
 * TCP core runs here; reject the optional out-of-order reclaim notification.
 * The full native-stack/shared-pool test runs in tests/host/lwip instead.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"
#include <stdlib.h>
struct tcp_pcb *tcp_active_pcbs;
void tcp_free_ooseq(struct tcp_pcb *pcb) { (void)pcb; abort(); }
err_t tcpip_try_callback(tcpip_callback_fn fn, void *arg)
{ (void)fn; (void)arg; return ERR_MEM; }
__attribute__((constructor)) void pool_fixture_start(void)
{ mem_init(); memp_init(); }
