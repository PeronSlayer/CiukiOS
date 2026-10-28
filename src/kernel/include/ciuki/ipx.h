/* Bounded IPX/SPX endpoint hub. All calls require its owner's gate.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_IPX_H
#define CIUKI_IPX_H
#include <ciuki/ipx_wire.h>
#include <ciuki/net_packet.h>
#define IPX_PORT_BUSY (-4096) /* private: retry owner contention, never a user errno */
#define IPX_ENDPOINTS 48
#define IPX_QUEUE 8
#define SPX_DATA_MAX 534
enum spx_state { SPX_IDLE, SPX_LISTEN, SPX_CONNECTING, SPX_ESTABLISHED, SPX_END, SPX_DEAD };
struct ipx_endpoint {
    struct ciuki_sockaddr_ipx local, peer;
    struct net_frame *rx[IPX_QUEUE], *tx;
    uint64_t due, last_rx, watchdog;
    unsigned type, state, head, count, offset, retries, probes, backlog;
    unsigned packet_type, filter, datastream, parent;
    uint16_t id, remote_id, sequence, expected, allocation;
    int error, connect_error;
    bool used, bound, connected, broadcast, receive_broadcast, filtering;
    bool eof, shut_read, shut_write, detached, abortive;
};
struct ipx_stack {
    struct ipx_endpoint endpoint[IPX_ENDPOINTS];
    uint8_t node[6], network[4], networks[IPX_FRAMINGS][4];
    uint16_t next_socket, next_id;
    unsigned framing, scores[IPX_FRAMINGS], routers, linkspeed;
    unsigned frame_type;
    struct ipx_frame_cache frames;
    uint8_t router_node[16][6], router_framing[16];
    uint64_t now, discovery_end;
    bool link, probed;
    void *arg;
    /* Encapsulated frame; copy before return. Must not reenter this hub. */
    int (*send)(void *, const uint8_t *, unsigned);
};
void ipx_init(struct ipx_stack *, const uint8_t node[6], void *, int (*)(void *, const uint8_t *, unsigned));
int ipx_open(struct ipx_stack *, unsigned type, unsigned protocol);
int ipx_bind(struct ipx_stack *, unsigned, const struct ciuki_sockaddr_ipx *);
int ipx_connect(struct ipx_stack *, unsigned, const struct ciuki_sockaddr_ipx *);
int ipx_listen(struct ipx_stack *, unsigned, unsigned);
int ipx_accept(struct ipx_stack *, unsigned);
int ipx_send(struct ipx_stack *, unsigned, const void *, unsigned, const struct ciuki_sockaddr_ipx *);
int ipx_recv(struct ipx_stack *, unsigned, void *, unsigned, unsigned, struct ciuki_sockaddr_ipx *, unsigned *);
int ipx_option(struct ipx_stack *, unsigned, bool, int, unsigned, void *, unsigned *);
void ipx_close(struct ipx_stack *, unsigned);
int ipx_shutdown(struct ipx_stack *, unsigned, unsigned);
void ipx_input(struct ipx_stack *, const uint8_t *, unsigned);
void ipx_tick(struct ipx_stack *, uint64_t);
int ipx_set_framing(struct ipx_stack *, unsigned);
void ipx_link(struct ipx_stack *, bool);
int ipx_events(struct ipx_stack *, unsigned);
/* Production bridge owner. Token = slot+1; zero means no endpoint. */
int net_ipx_open(unsigned, unsigned);
int net_ipx_call(unsigned, unsigned, void *, unsigned, unsigned, struct ciuki_sockaddr_ipx *, unsigned *);
int net_ipx_option(unsigned, bool, int, unsigned, void *, unsigned *);
int net_ipx_names(unsigned, struct ciuki_sockaddr_ipx *, struct ciuki_sockaddr_ipx *);
int net_ipx_events(unsigned);
int net_ipx_close(unsigned);
struct net_bridge_port;
void net_ipx_port_start(struct net_bridge_port *); /* port gate held */
enum { IPX_CALL_BIND, IPX_CALL_CONNECT, IPX_CALL_LISTEN, IPX_CALL_ACCEPT,
       IPX_CALL_SEND, IPX_CALL_RECV, IPX_CALL_SHUTDOWN, IPX_CALL_STATUS };
#endif
