/* Core-owned Ethernet/DHCP/DNS integration; no device polling on tcpip_thread.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/stats.h"
#include <ciuki/net_stack.h>
#include <ciuki/kernel.h>
#ifndef LWIP_HOST
#include <ciuki/ledger.h>
#include <ciuki/cpu.h>
#include <ciuki/clock.h>
#endif
struct ethernet_port {
    struct netif interface;
    uint8_t mac[6];
    unsigned index;
    bool attached, desired_link, applied_link;
    bool dhcp_started, primary;
    int dhcp_error;
    uint64_t dhcp_retry;
    uint32_t address;
    struct tcpip_callback_msg *link_message;
};
static struct ethernet_port ports[2];
static bool started, attempted;
static sys_sem_t initialized;
int cu_sockets_init(void);
void ciuki_sockets_address_lost(uint32_t);
static void link_dispatch(void *);

static err_t output(struct netif *n, struct pbuf *p)
{
    if (p->tot_len < 14 || p->tot_len > 1514) return ERR_IF;
    uint8_t frame[1514];
    if (pbuf_copy_partial(p, frame, p->tot_len, 0) != p->tot_len) return ERR_IF;
    struct ethernet_port *port = n->state;
    int r = net_stack_transmit(port->index, frame, p->tot_len);
    return r == -EAGAIN ? ERR_MEM : r ? ERR_IF : ERR_OK;
}
static void status(struct netif *n)
{
    struct ethernet_port *p = n->state;
    sys_prot_t f = sys_arch_protect();
    uint32_t old = p->address;
    p->address = netif_is_link_up(n) ? ip4_addr_get_u32(netif_ip4_addr(n)) : 0;
    uint32_t current = p->address;
    sys_arch_unprotect(f);
    if (old && old != current) {
        ciuki_sockets_address_lost(old);
        if (netif_default == n && !current)
            for (unsigned i = 0; i < DNS_MAX_SERVERS; i++) dns_setserver(i, IP_ADDR_ANY);
    }
    cu_changed();
}
static err_t interface_init(struct netif *n)
{
    struct ethernet_port *p = n->state;
    n->name[0] = 'e'; n->name[1] = 'n'; n->mtu = 1500;
    n->hwaddr_len = 6; memcpy(n->hwaddr, p->mac, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output; n->linkoutput = output;
    return ERR_OK;
}
static void link_update(void *arg)
{
    struct ethernet_port *p = arg;
    sys_prot_t f = sys_arch_protect(); bool up = p->desired_link; sys_arch_unprotect(f);
    if (p->applied_link != up) {
        f = sys_arch_protect(); p->applied_link = up; sys_arch_unprotect(f);
        if (up) netif_set_link_up(&p->interface);
        else {
            dhcp_stop(&p->interface);
            netif_set_link_down(&p->interface);
            netif_set_addr(&p->interface, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
            /* DNS TTL/source/question validation remains upstream; no fallback. */
            if (p->primary)
                for (unsigned i = 0; i < DNS_MAX_SERVERS; i++) dns_setserver(i, IP_ADDR_ANY);
            f = sys_arch_protect(); p->dhcp_started = false; p->dhcp_error = 0; p->dhcp_retry = 0; sys_arch_unprotect(f);
        }
    }
    if (up && netif_default == &p->interface && !p->dhcp_started && cu_now() >= p->dhcp_retry) {
        err_t e = dhcp_start(&p->interface);
        f = sys_arch_protect(); p->dhcp_started = e == ERR_OK;
        p->dhcp_error = e == ERR_OK ? 0 : e == ERR_MEM ? -ENOMEM : -ENETDOWN;
        p->dhcp_retry = cu_now() + 1000;
        sys_arch_unprotect(f); cu_changed();
    }
    status(&p->interface);
}
static void attach_core(void *arg)
{
    struct ethernet_port *p = arg;
    if (!netif_add(&p->interface, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4,
                   p, interface_init, tcpip_input)) return;
    netif_set_status_callback(&p->interface, status);
    netif_set_link_callback(&p->interface, status);
    if (!netif_default || netif_default->name[0] == 'l') netif_set_default(&p->interface);
    p->primary = netif_default == &p->interface;
    netif_set_up(&p->interface); p->attached = true;
    link_update(p);
}
int net_stack_attach(unsigned index, const uint8_t mac[6], bool link)
{
    if (!started || index >= 2 || ports[index].attached) return -EINVAL;
    struct ethernet_port *p = &ports[index]; p->index = index;
    memcpy(p->mac, mac, 6); p->desired_link = link;
    p->link_message = tcpip_callbackmsg_new(link_dispatch, p);
    if (!p->link_message) return -ENOMEM;
    err_t r = tcpip_callback_wait(attach_core, p);
    if (r || !p->attached) { tcpip_callbackmsg_delete(p->link_message); p->link_message = NULL; return -ENOMEM; }
    return 0;
}
void net_stack_link(unsigned index, bool up)
{
    if (index >= 2 || !ports[index].attached) return;
    struct ethernet_port *p = &ports[index];
    sys_prot_t f = sys_arch_protect(); p->desired_link = up; sys_arch_unprotect(f);
    /* Worker retries on each pass. One preallocated static message can only
     * be outstanding once; the completion clears the pending flag below. */
    extern void cu_stack_schedule_link(void *);
    cu_stack_schedule_link(p);
}
static bool link_pending[2];
static void link_dispatch(void *arg)
{
    struct ethernet_port *p = arg; link_update(p);
    sys_prot_t f = sys_arch_protect(); link_pending[p->index] = false; sys_arch_unprotect(f);
}
void cu_stack_schedule_link(void *arg)
{
    struct ethernet_port *p = arg; sys_prot_t f = sys_arch_protect();
    bool retry = p->primary && p->desired_link && !p->dhcp_started && cu_now() >= p->dhcp_retry;
    bool post = !link_pending[p->index] && (p->desired_link != p->applied_link || retry);
    if (post) link_pending[p->index] = true;
    sys_arch_unprotect(f);
    if (post && tcpip_callbackmsg_trycallback(p->link_message) != ERR_OK) {
        f = sys_arch_protect(); link_pending[p->index] = false; sys_arch_unprotect(f);
    }
}
void net_stack_input(unsigned index, const void *data, unsigned bytes)
{
    if (index >= 2 || !ports[index].attached || bytes < 14 || bytes > 1514) return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)bytes, PBUF_POOL);
    if (!p) { LINK_STATS_INC(link.drop); return; }
    if (pbuf_take(p, data, (u16_t)bytes) != ERR_OK || tcpip_input(p, &ports[index].interface) != ERR_OK) {
        pbuf_free(p); LINK_STATS_INC(link.drop);
    }
}
void net_stack_address(unsigned index, uint32_t *address)
{
    sys_prot_t f = sys_arch_protect(); *address = index < 2 ? ports[index].address : 0; sys_arch_unprotect(f);
}
int net_stack_dhcp_error(unsigned index)
{
    sys_prot_t f = sys_arch_protect(); int e = index < 2 ? ports[index].dhcp_error : -EINVAL;
    sys_arch_unprotect(f); return e;
}
static void ready(void *unused) { (void)unused; sys_sem_signal(&initialized); }
void net_stack_entropy(uint64_t sample) { ciuki_lwip_seed(sample); }
int net_stack_init(void)
{
    if (started) return 0;
    if (attempted) return -ENOMEM;
    attempted = true;
#ifndef LWIP_HOST
    extern char __net_start[], __net_end[];
    unsigned pages = (unsigned)(__net_end - __net_start) / PAGE_SIZE;
    /* Static pages are already resident in the image. Keep emergency pages,
     * and reserve the remaining full 4 MiB before starting any network task. */
    struct mm_ledger memory; ledger_snapshot(&memory);
    if (pages > 1024 || memory.zone[ZONE_NORMAL].free < memory.high + 1024 - pages)
        return -ENOMEM;
    ledger_move(V2P(__net_start), pages, MM_KERNEL_IMAGE, MM_NET);
    /* commit_capacity accounts resident owner pages against the promise.
     * Promise the TOTAL ceiling, otherwise BSS would cancel the reservation
     * for the remaining stacks and DMA allocations a second time. */
    if (!ledger_commit(MM_NET, 1024)) {
        ledger_move(V2P(__net_start), pages, MM_NET, MM_KERNEL_IMAGE);
        return -ENOMEM;
    }
    struct clock_seed rtc; file_clock_snapshot(&rtc);
    ciuki_lwip_seed((uint64_t)rtc.utc ^ g_ticks);
    if (g_cpu_tsc) ciuki_lwip_seed(read_tsc_serialized());
    uint32_t flags = irq_save(); outb(0x43, 0); unsigned pit = inb(0x40); pit |= (unsigned)inb(0x40) << 8;
    irq_restore(flags); ciuki_lwip_seed(pit);
#endif
    /* Driver discovery publishes the primary MAC before transport init uses
     * LWIP_RAND for the initial TCP/UDP ephemeral-port starts. */
    extern void net_stack_driver_entropy(void) __attribute__((weak));
    if (net_stack_driver_entropy) net_stack_driver_entropy();
    if (sys_sem_new(&initialized, 0) != ERR_OK) return -ENOMEM;
    tcpip_init(ready, NULL);
    if (sys_arch_sem_wait(&initialized, 2000) == SYS_ARCH_TIMEOUT) return -ENOMEM;
    sys_sem_free(&initialized);
    if (cu_sockets_init()) return -ENOMEM;
    started = true; return 0;
}
