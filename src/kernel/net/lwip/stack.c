/* Core-owned Ethernet/DHCP/DNS integration; no device polling on tcpip_thread.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/stats.h"
#include "netif/ethernet.h"
#include <ciuki/net_stack.h>
#include <ciuki/net_control.h>
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
    struct ciuki_net_config config;
    uint32_t acquired_dns[3];
    bool released;
    int dhcp_error;
    uint64_t dhcp_retry;
    uint32_t address;
    struct tcpip_callback_msg *link_message;
    /* f6-22 records. Core thread only, except the frame counters below. */
    uint8_t dhcp_state;             /* last DHCP state observed, 0xff none */
    bool dhcp_unrecorded;           /* dhcp_state not recorded yet */
    int start_recorded;             /* last dhcp_start result recorded */
    unsigned record_tokens, suppressed;
    uint64_t acquiring_since, timeout_recorded, tokens_at;
    /* Copied frames accepted by/handed out of the stack (sys_arch_protect). */
    uint64_t frames_in, frames_out;
};
static struct ethernet_port ports[2];
static bool started, attempted;
/* f6-22: state records. Input and the netif status callback observe DHCP
 * transitions at once; a watch pass every 250 ms (posted by the NIC worker
 * through net_stack_watch with one preallocated core message, so no timeout
 * allocation can fail) observes those made on lwIP timers (ACD,
 * retransmission, T1/T2), reports an acquisition that does not complete and
 * prints the stack line: every 10 s for the first two minutes (DHCP and the
 * first lookups), then every 60 s; a line with nothing changed waits 5 min. */
#define NET_WATCH_MS 250u
#define NET_STACK_EARLY_MS 120000u
#define NET_STACK_EARLY_LINE_MS 10000u
#define NET_STACK_LINE_MS 60000u
#define NET_STACK_QUIET_MS 300000u
#define NET_DHCP_TIMEOUT_MS 30000u
#define NET_DHCP_TIMEOUT_REPEAT_MS 60000u
/* lwIP restarts DISCOVER at once after a NAK: a server that NAKs every
 * REQUEST drives offer/discover in a tight loop. Records other than "bound"
 * spend a token (burst 16, one per second); a suppressed transition is
 * counted and the then-current state is recorded by a later watch pass. */
#define NET_RECORD_BURST 16u
#define NET_RECORD_REFILL_MS 1000u
extern void net_stack_driver_counters(unsigned, struct net_stack_counters *) __attribute__((weak));
static struct tcpip_callback_msg *watch_message;
static bool watch_pending;
static uint64_t watch_due;
static uint64_t stack_line_at, stack_line_quiet_at, stack_started;
static struct net_stack_counters stack_line_last;
static uint32_t stack_line_address;
static sys_sem_t initialized;
extern int entropy_get(void *, size_t) __attribute__((weak));
static bool crypto_seeded;
int cu_sockets_init(void);
void ciuki_resolver_counts(unsigned *, unsigned *);
void ciuki_sockets_address_lost(uint32_t);
void ciuki_tcp_records(void);
static void link_dispatch(void *);
void ciuki_stack_dhcp_dns(struct netif *n, unsigned index, uint32_t address)
{
    struct ethernet_port *p = n->state;
    if (!p || index >= 3) return;
    p->acquired_dns[index] = address;
    if (p->primary && !p->config.dns[0]) {
        ip_addr_t a; ip_addr_set_ip4_u32_val(a, address); dns_setserver((u8_t)index, &a);
    }
}
static void configured_dns(struct ethernet_port *p)
{
    if (p->primary && p->config.enabled && p->address && !p->released && p->config.dns[0] && netif_is_link_up(&p->interface))
        for (unsigned i = 0; i < DNS_MAX_SERVERS; i++) {
            ip_addr_t a; ip_addr_set_ip4_u32_val(a, p->config.dns[i]); dns_setserver((u8_t)i, &a);
        }
}

/* Network byte order to dotted decimal, without libc formatting. */
static const char *dotted(uint32_t address, char out[16])
{
    const uint8_t *b = (const uint8_t *)&address; char *p = out;
    for (unsigned i = 0; i < 4; i++) {
        unsigned v = b[i];
        if (v >= 100) *p++ = (char)('0' + v / 100);
        if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
        *p++ = (char)('0' + v % 10);
        *p++ = i < 3 ? '.' : 0;
    }
    return out;
}
static uint32_t dns_server(unsigned i)
{
    const ip_addr_t *a = dns_getserver((u8_t)i);
    return a ? ip4_addr_get_u32(ip_2_ip4(a)) : 0;
}
static const char *dhcp_state_name(unsigned state)
{
    switch (state) {
    case DHCP_STATE_OFF: return "off";
    case DHCP_STATE_REQUESTING: return "offer";      /* OFFER selected, REQUEST sent */
    case DHCP_STATE_INIT: return "init";
    case DHCP_STATE_REBOOTING: return "rebooting";
    case DHCP_STATE_REBINDING: return "rebinding";
    case DHCP_STATE_RENEWING: return "renewing";
    case DHCP_STATE_SELECTING: return "discover";    /* DISCOVER sent, no OFFER yet */
    case DHCP_STATE_INFORMING: return "informing";
    case DHCP_STATE_CHECKING: return "checking";     /* ACK received, ACD probe */
    case DHCP_STATE_BOUND: return "bound";
    case DHCP_STATE_BACKING_OFF: return "backing_off";
    default: return "unknown";
    }
}
static bool dhcp_acquiring(unsigned state)
{
    return state != DHCP_STATE_OFF && state != DHCP_STATE_BOUND &&
           state != DHCP_STATE_RENEWING && state != DHCP_STATE_REBINDING;
}
static void driver_counters(unsigned index, struct net_stack_counters *out)
{
    *out = (struct net_stack_counters){0};
    if (net_stack_driver_counters) net_stack_driver_counters(index, out);
}
static bool record_token(struct ethernet_port *p, uint64_t now)
{
    uint64_t refill = (now - p->tokens_at) / NET_RECORD_REFILL_MS;
    if (refill) {
        p->tokens_at += refill * NET_RECORD_REFILL_MS;
        p->record_tokens = refill >= NET_RECORD_BURST - p->record_tokens ? NET_RECORD_BURST : p->record_tokens + (unsigned)refill;
    }
    if (!p->record_tokens) return false;
    p->record_tokens--; return true;
}
/* Core thread: one protected record per DHCP state change, never under
 * sys_arch_protect. Repeated DISCOVERs stay in "discover"; the timeout
 * record reports an acquisition that does not complete. */
static void dhcp_observe(struct ethernet_port *p)
{
    struct netif *n = &p->interface;
    struct dhcp *dhcp = netif_dhcp_data(n);
    unsigned state = dhcp ? dhcp->state : DHCP_STATE_OFF;
    uint64_t now = cu_now();
    if (state != p->dhcp_state) {
        if (p->dhcp_unrecorded) p->suppressed++;
        p->dhcp_state = (uint8_t)state; p->dhcp_unrecorded = true;
        if (dhcp_acquiring(state)) { if (!p->acquiring_since) p->acquiring_since = now; }
        else p->acquiring_since = p->timeout_recorded = 0;
    }
    if (!p->dhcp_unrecorded || (state != DHCP_STATE_BOUND && !record_token(p, now))) return;
    p->dhcp_unrecorded = false;
    unsigned suppressed = p->suppressed; p->suppressed = 0;
    char ip[16], gw[16], dns0[16], dns1[16], extra[16];
    if (state == DHCP_STATE_REQUESTING || state == DHCP_STATE_CHECKING) {
        /* The offered lease; router and DNS options arrive with the ACK. */
        klog("[net] dhcp state=%s iface=en%u ip=%s gw=%s dns=%s server=%s tries=%u suppressed=%u",
             dhcp_state_name(state), p->index, dotted(ip4_addr_get_u32(&dhcp->offered_ip_addr), ip),
             dotted(ip4_addr_get_u32(&dhcp->offered_gw_addr), gw), dotted(dns_server(0), dns0),
             dotted(ip4_addr_get_u32(ip_2_ip4(&dhcp->server_ip_addr)), extra), (unsigned)dhcp->tries, suppressed);
        return;
    }
    klog("[net] dhcp state=%s iface=en%u ip=%s gw=%s dns=%s,%s mask=%s lease=%u tries=%u suppressed=%u",
         dhcp_state_name(state), p->index, dotted(ip4_addr_get_u32(netif_ip4_addr(n)), ip),
         dotted(ip4_addr_get_u32(netif_ip4_gw(n)), gw), dotted(dns_server(0), dns0), dotted(dns_server(1), dns1),
         dotted(ip4_addr_get_u32(netif_ip4_netmask(n)), extra),
         dhcp && state == DHCP_STATE_BOUND ? (unsigned)dhcp->offered_t0_lease : 0u, dhcp ? (unsigned)dhcp->tries : 0u,
         suppressed);
}
static void dhcp_timeout_check(struct ethernet_port *p, uint64_t now)
{
    struct dhcp *dhcp = netif_dhcp_data(&p->interface);
    unsigned state = dhcp ? dhcp->state : DHCP_STATE_OFF;
    if (!p->dhcp_started || !dhcp_acquiring(state) || !p->acquiring_since ||
        now - p->acquiring_since < NET_DHCP_TIMEOUT_MS ||
        (p->timeout_recorded && now - p->timeout_recorded < NET_DHCP_TIMEOUT_REPEAT_MS)) return;
    p->timeout_recorded = now;
    struct net_stack_counters c; driver_counters(p->index, &c);
    sys_prot_t f = sys_arch_protect(); uint64_t in = p->frames_in, out = p->frames_out; sys_arch_unprotect(f);
    char ip[16], gw[16], dns[16];
    klog("[net] dhcp state=timeout phase=%s iface=en%u ip=%s gw=%s dns=%s tries=%u elapsed_ms=%llu rx=%llu rx_errors=%llu tx=%llu in=%llu out=%llu",
         dhcp_state_name(state), p->index, dotted(ip4_addr_get_u32(netif_ip4_addr(&p->interface)), ip),
         dotted(ip4_addr_get_u32(netif_ip4_gw(&p->interface)), gw), dotted(dns_server(0), dns),
         (unsigned)dhcp->tries, (unsigned long long)(now - p->acquiring_since), (unsigned long long)c.rx,
         (unsigned long long)c.rx_errors, (unsigned long long)c.tx, (unsigned long long)in, (unsigned long long)out);
}
/* `[net] stack ...`: the primary (else first) attached interface. */
static void stack_line(uint64_t now)
{
    struct ethernet_port *p = NULL; unsigned interfaces = 0;
    for (unsigned i = 0; i < 2; i++) if (ports[i].attached) { interfaces++; if (!p || ports[i].primary) p = &ports[i]; }
    if (!p || now < stack_line_at) return;
    stack_line_at = now + (now - stack_started < NET_STACK_EARLY_MS ? NET_STACK_EARLY_LINE_MS : NET_STACK_LINE_MS);
    struct net_stack_counters c; driver_counters(p->index, &c);
    sys_prot_t f = sys_arch_protect();
    c.frames_in = p->frames_in; c.frames_out = p->frames_out; uint32_t address = p->address;
    sys_arch_unprotect(f);
    bool changed = memcmp(&c, &stack_line_last, sizeof(c)) || address != stack_line_address;
    if (!changed && now < stack_line_quiet_at) return;
    stack_line_last = c; stack_line_address = address; stack_line_quiet_at = now + NET_STACK_QUIET_MS;
    struct dhcp *dhcp = netif_dhcp_data(&p->interface);
    unsigned resolved = 0, failed = 0; ciuki_resolver_counts(&resolved, &failed);
    char ip[16], gw[16], dns[16];
    klog_periodic("[net] stack ifaces=%u en%u link=%s ip=%s gw=%s dns=%s dhcp=%s rx=%llu tx=%llu errors=%llu dropped=%llu in=%llu out=%llu dns_ok=%u dns_fail=%u",
         interfaces, p->index, netif_is_link_up(&p->interface) ? "up" : "down", dotted(address, ip),
         dotted(ip4_addr_get_u32(netif_ip4_gw(&p->interface)), gw), dotted(dns_server(0), dns),
         dhcp_state_name(dhcp ? dhcp->state : DHCP_STATE_OFF), (unsigned long long)c.rx, (unsigned long long)c.tx,
         (unsigned long long)(c.rx_errors + c.tx_errors), (unsigned long long)c.dropped,
         (unsigned long long)c.frames_in, (unsigned long long)c.frames_out, resolved, failed);
}
static void watch(void *unused)
{
    (void)unused; uint64_t now = cu_now();
    for (unsigned i = 0; i < 2; i++) if (ports[i].attached) { dhcp_observe(&ports[i]); dhcp_timeout_check(&ports[i], now); }
    stack_line(now);
    ciuki_tcp_records();
    sys_prot_t f = sys_arch_protect(); watch_pending = false; sys_arch_unprotect(f);
}
void net_stack_watch(void)
{
    if (!started || !watch_message) return;
    uint64_t now = cu_now();
    sys_prot_t f = sys_arch_protect();
    bool post = !watch_pending && now >= watch_due;
    if (post) { watch_pending = true; watch_due = now + NET_WATCH_MS; }
    sys_arch_unprotect(f);
    if (post && tcpip_callbackmsg_trycallback(watch_message) != ERR_OK) {
        f = sys_arch_protect(); watch_pending = false; sys_arch_unprotect(f);
    }
}
/* Core-side ingress: Ethernet input, then any DHCP transition it caused
 * (OFFER -> offer, ACK -> checking or bound, NAK -> discover again). */
static err_t observed_input(struct pbuf *p, struct netif *n)
{
    struct ethernet_port *port = n->state;
    if (!port->config.enabled || port->released) { pbuf_free(p); return ERR_OK; }
    err_t e = ethernet_input(p, n);
    dhcp_observe(n->state);
    return e;
}

static err_t output(struct netif *n, struct pbuf *p)
{
    struct ethernet_port *port = n->state;
    if (!port->config.enabled || port->released) return ERR_IF;
    if (p->tot_len < 14 || p->tot_len > 1514) return ERR_IF;
    uint8_t frame[1514];
    if (pbuf_copy_partial(p, frame, p->tot_len, 0) != p->tot_len) return ERR_IF;
    int r = net_stack_transmit(port->index, frame, p->tot_len);
    if (!r) { sys_prot_t f = sys_arch_protect(); port->frames_out++; sys_arch_unprotect(f); }
    return r == -EAGAIN ? ERR_MEM : r ? ERR_IF : ERR_OK;
}
static void status(struct netif *n)
{
    struct ethernet_port *p = n->state;
    sys_prot_t f = sys_arch_protect();
    uint32_t old = p->address;
    p->address = netif_is_up(n) && netif_is_link_up(n) ? ip4_addr_get_u32(netif_ip4_addr(n)) : 0;
    uint32_t current = p->address;
    sys_arch_unprotect(f);
    if (old && old != current) {
        ciuki_sockets_address_lost(old);
        if (netif_default == n && !current)
            for (unsigned i = 0; i < DNS_MAX_SERVERS; i++) dns_setserver(i, IP_ADDR_ANY);
    }
    configured_dns(p);
    if (p->attached) dhcp_observe(p);
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
    if (up && p->config.enabled && p->config.dhcp && !p->released && !p->dhcp_started && cu_now() >= p->dhcp_retry) {
        err_t e = dhcp_start(&p->interface);
        f = sys_arch_protect(); p->dhcp_started = e == ERR_OK;
        p->dhcp_error = e == ERR_OK ? 0 : e == ERR_MEM ? -ENOMEM : -ENETDOWN;
        p->dhcp_retry = cu_now() + 1000;
        int error = p->dhcp_error;
        sys_arch_unprotect(f); cu_changed();
        /* Every successful start; a failure once per distinct error (the
         * worker retries each second). */
        if ((!error || error != p->start_recorded) && record_token(p, cu_now())) {
            char mac[18]; static const char digits[] = "0123456789abcdef";
            for (unsigned i = 0; i < 6; i++) {
                mac[i * 3] = digits[p->mac[i] >> 4]; mac[i * 3 + 1] = digits[p->mac[i] & 15];
                mac[i * 3 + 2] = i < 5 ? ':' : 0;
            }
            klog("[net] dhcp state=%s iface=en%u mac=%s error=%d", error ? "start_failed" : "start", p->index, mac, error);
        }
        p->start_recorded = error;
        dhcp_observe(p);
    }
    if (up && p->config.enabled && !p->config.dhcp) {
        ip4_addr_t a = {p->config.address}, m = {p->config.mask}, g = {p->config.gateway};
        netif_set_addr(&p->interface, &a, &m, &g);
        if (p->primary) for (unsigned i = 0; i < DNS_MAX_SERVERS; i++) {
            ip_addr_t dns; ip_addr_set_ip4_u32_val(dns, p->config.dns[i]); dns_setserver((u8_t)i, &dns);
        }
    }
    status(&p->interface);
}
static void attach_core(void *arg)
{
    struct ethernet_port *p = arg;
    p->dhcp_state = 0xff; p->start_recorded = 1;
    p->record_tokens = NET_RECORD_BURST; p->tokens_at = cu_now();
    if (!netif_add(&p->interface, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4,
                   p, interface_init, tcpip_input)) return;
    netif_set_status_callback(&p->interface, status);
    netif_set_link_callback(&p->interface, status);
    if (!netif_default || netif_default->name[0] == 'l') netif_set_default(&p->interface);
    p->primary = netif_default == &p->interface;
    netif_set_hostname(&p->interface, p->config.hostname);
    if (p->config.enabled) netif_set_up(&p->interface);
    sys_prot_t f = sys_arch_protect(); p->attached = true; sys_arch_unprotect(f);
    klog("[net] config iface=en%u mode=%s enabled=%u source=boot", p->index, p->config.dhcp ? "dhcp" : "static", p->config.enabled);
    link_update(p);
}
int net_stack_attach(unsigned index, const uint8_t mac[6], bool link)
{
    if (!started || index >= 2 || ports[index].attached) return -EINVAL;
    struct ethernet_port *p = &ports[index]; p->index = index;
    memcpy(p->mac, mac, 6); p->desired_link = link;
    extern void net_settings_get(unsigned, struct ciuki_net_config *) __attribute__((weak));
    net_config_default(&p->config);
    if (net_settings_get) net_settings_get(index, &p->config);
    extern int net_control_driver(unsigned, struct ciuki_net_control *, bool) __attribute__((weak));
    if (net_control_driver) {
        struct ciuki_net_control cfg = {.index = index, .config = p->config};
        if (net_control_driver(index, &cfg, true)) {
            net_config_default(&p->config);
            klog("[net] config iface=en%u mode=dhcp fallback=adapter_options", index);
        }
    }
    p->link_message = tcpip_callbackmsg_new(link_dispatch, p);
    if (!p->link_message) return -ENOMEM;
    err_t r = tcpip_callback_wait(attach_core, p);
    if (r || !p->attached) { tcpip_callbackmsg_delete(p->link_message); p->link_message = NULL; return -ENOMEM; }
    return 0;
}
struct config_request { unsigned index, operation; struct ciuki_net_control *record; int error; };
static void config_core(void *arg)
{
    struct config_request *r = arg;
    struct ethernet_port *p = &ports[r->index]; struct netif *n = &p->interface;
    struct ciuki_net_control *o = r->record;
    if (!p->attached) { r->error = -ENODEV; return; }
    if (r->operation == CIUKI_NETCTL_SET) {
        dhcp_release_and_stop(n); p->dhcp_started = false; p->dhcp_retry = 0; p->released = false;
        netif_set_down(n);
        netif_set_addr(n, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        sys_prot_t f = sys_arch_protect(); p->config = o->config; sys_arch_unprotect(f);
        netif_set_hostname(n, p->config.hostname);
        if (p->config.enabled) netif_set_up(n);
        link_update(p);
    } else if (r->operation == CIUKI_NETCTL_RELEASE) {
        if (!p->config.dhcp) { r->error = -EINVAL; return; }
        dhcp_release_and_stop(n); p->dhcp_started = false; p->released = true;
        netif_set_addr(n, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    } else if (r->operation == CIUKI_NETCTL_RENEW) {
        if (!p->config.enabled || !p->config.dhcp || !netif_is_link_up(n)) { r->error = -ENETDOWN; return; }
        struct dhcp *d = netif_dhcp_data(n);
        bool bound = d && dhcp_supplied_address(n);
        p->released = false;
        err_t e = bound ? dhcp_renew(n) : dhcp_start(n);
        p->dhcp_started = e == ERR_OK;
        r->error = e == ERR_OK ? 0 : e == ERR_MEM ? -ENOMEM : -ENETDOWN;
    }
    status(n);
    o->config = p->config;
    o->address = p->address; o->mask = ip4_addr_get_u32(netif_ip4_netmask(n)); o->gateway = ip4_addr_get_u32(netif_ip4_gw(n));
    for (unsigned i = 0; i < DNS_MAX_SERVERS; i++) o->dns[i] = !p->address ? 0 : p->primary ? dns_server(i) :
        p->config.dns[0] || !p->config.dhcp ? p->config.dns[i] : p->acquired_dns[i];
    struct dhcp *d = netif_dhcp_data(n);
    if (d && dhcp_supplied_address(n)) {
        o->lease_seconds = d->offered_t0_lease;
        uint64_t used = (uint64_t)d->lease_used * 60;
        o->lease_remaining = used < o->lease_seconds ? o->lease_seconds - (uint32_t)used : 0;
    }
}
int net_stack_config(unsigned index, unsigned operation, struct ciuki_net_control *record)
{
    if (!started || index >= 2 || !ports[index].attached) return -ENODEV;
    struct config_request r = {index, operation, record, 0};
    err_t e = tcpip_callback_wait(config_core, &r);
    return e == ERR_OK ? r.error : -ENOMEM;
}
void net_stack_search_domain(char out[128])
{
    out[0] = 0; sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < 2; i++) if (ports[i].attached && ports[i].primary) {
        memcpy(out, ports[i].config.domain, 128); break;
    }
    sys_arch_unprotect(f);
}
int net_stack_primary(void)
{
    sys_prot_t f = sys_arch_protect(); int index = -ENODEV;
    for (unsigned i = 0; i < 2; i++)
        if (ports[i].attached && ports[i].primary) { index = (int)i; break; }
    sys_arch_unprotect(f); return index;
}
int net_stack_random_id(uint8_t id[16])
{
    if (!id) return -EINVAL;
    if (!started) { memset(id, 0, 16); return -ENETDOWN; }
    /* Exactly the approved per-boot SplitMix64 stream, seeded with RTC,
     * PIT/TSC samples and the primary MAC before transport initialization. */
    for (unsigned i = 0; i < 4; i++) {
        uint32_t word = ciuki_lwip_rand();
        for (unsigned j = 0; j < 4; j++) id[i * 4 + j] = (uint8_t)(word >> (j * 8));
    }
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
    bool retry = p->config.enabled && p->config.dhcp && !p->released && p->desired_link && !p->dhcp_started && cu_now() >= p->dhcp_retry;
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
    if (pbuf_take(p, data, (u16_t)bytes) != ERR_OK || tcpip_inpkt(p, &ports[index].interface, observed_input) != ERR_OK) {
        pbuf_free(p); LINK_STATS_INC(link.drop); return;
    }
    sys_prot_t f = sys_arch_protect(); ports[index].frames_in++; sys_arch_unprotect(f);
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
void net_stack_entropy(uint64_t sample)
{
    if (crypto_seeded) return;
    uint64_t seed;
    if (entropy_get && !entropy_get(&seed, sizeof(seed))) {
        ciuki_lwip_seed(seed);
        volatile uint8_t *bytes = (volatile uint8_t *)&seed;
        for (unsigned i = 0; i < sizeof(seed); i++) bytes[i] = 0;
        crypto_seeded = true;
    } else ciuki_lwip_seed(sample);
}
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
    net_stack_entropy((uint64_t)rtc.utc ^ g_ticks);
    if (g_cpu_tsc) net_stack_entropy(read_tsc_serialized());
    uint32_t flags = irq_save(); outb(0x43, 0); unsigned pit = inb(0x40); pit |= (unsigned)inb(0x40) << 8;
    irq_restore(flags); net_stack_entropy(pit);
#endif
    /* Prefer DRBG material before transport uses LWIP_RAND. While unavailable,
     * driver discovery supplies the primary MAC to the legacy boot mix. */
    extern void net_stack_driver_entropy(void) __attribute__((weak));
    if (net_stack_driver_entropy) net_stack_driver_entropy();
    if (sys_sem_new(&initialized, 0) != ERR_OK) return -ENOMEM;
    tcpip_init(ready, NULL);
    if (sys_arch_sem_wait(&initialized, 2000) == SYS_ARCH_TIMEOUT) return -ENOMEM;
    sys_sem_free(&initialized);
    if (cu_sockets_init()) return -ENOMEM;
    extern int net_control_init(void);
    if (net_control_init()) return -ENOMEM;
    /* Records only: without the message the stack runs, unobserved. */
    watch_message = tcpip_callbackmsg_new(watch, NULL); stack_started = cu_now();
    started = true; return 0;
}
