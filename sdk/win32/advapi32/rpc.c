/* Shared per-process channel transport lives in ntdll.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/ntdll.h>
#include <ciuki/abi.h>
extern uint32_t w32r_gate(uint32_t,uint32_t,uint32_t,uint32_t);
void w32r_pause(void){w32r_gate(CIUKI_SYS_SLEEP_MS,1,0,0);}
void w32r_last_error(uint32_t e){NtCurrentTeb()->LastErrorValue=e;}
uint32_t w32r_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used) {
    return w32_server_exchange(in,n,out,cap,used);
}
