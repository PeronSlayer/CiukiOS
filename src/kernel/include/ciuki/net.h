/* Provisional F6 device seam; reconcile with network-contract.md at F6-00.
 * Sources: docs/validation/f6-01-nic-drivers.md. No imported driver code.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_H
#define CIUKI_NET_H
#include <ciuki/registry.h>

#define NETIF_MAX 4u
#define NET_FRAME_MAX 1514u
#define NET_QUEUE_MAX 16u
#define NET_TX_TIMEOUT_MS 1000u
enum net_link { NET_LINK_UNKNOWN, NET_LINK_DOWN, NET_LINK_UP };
struct net_packet { uint16_t length; uint8_t data[NET_FRAME_MAX]; };
struct netif;
struct net_io {
    void *cookie;
    uint32_t (*read)(void *, uint16_t port, unsigned width);
    void (*write)(void *, uint16_t port, uint32_t value, unsigned width);
    uint64_t (*now)(void *);
    void (*sleep)(void *, uint32_t ms);
    int (*delay)(void *, uint32_t us);
};
struct netif_driver {
    const char *name;
    bool (*match)(const struct pci_func *);
    uint16_t aperture;
    unsigned bar, dma_pages;
    bool (*idle)(struct netif *);
    int (*init)(struct netif *);
    int (*start)(struct netif *);
    int (*stop)(struct netif *);
    int (*transmit)(struct netif *, const uint8_t *, unsigned length);
    /* One frame at most, 0 if empty, negative on fault. Also retires TX. */
    int (*receive_poll)(struct netif *, uint8_t *);
    enum net_link (*link_state)(struct netif *);
    void (*mac)(struct netif *, uint8_t address[6]);
    bool (*interrupt)(struct netif *);
    void (*irq_enable)(struct netif *, bool);
};
struct net_stats {
    uint64_t rx_packets, rx_bytes, rx_errors, rx_dropped;
    uint64_t tx_submitted, tx_completed, tx_bytes, tx_errors, tx_busy;
    uint64_t interrupts, budget_violations, max_irq_us, quarantines;
    unsigned queued, queue_peak;
};
/* Static device contexts and queue storage remain pinned even on quarantine.
 * Public layout permits register models to exercise production drivers. */
struct netif {
    const struct netif_driver *driver;
    struct net_io io;
    struct pci_func pci;
    char owner[20];
    uint16_t base, saved_command;
    uint8_t address[6], phy, eeprom_bits, next_page;
    enum net_link link;
    gen_t generation;
    uint32_t dma_phys;
    uint8_t *dma;
    int claims[3];
    gen_t claim_generation[3];
    struct kmutex mutex;
    struct net_stats stats;
    struct net_packet queue[NET_QUEUE_MAX], scratch;
    unsigned queue_head, queue_count, rx_offset, rx_head, rx_tail;
    bool tx_used[4];
    uint16_t tx_length[4], pending;
    uint64_t tx_deadline[4], link_deadline;
    bool ready, running, stopping, quarantined, touched, irq_attached;
    bool irq_pending, fault_pending, servicing;
    const char *reason;
};
extern const struct netif_driver net_e100, net_rtl8139, net_ne2000;
uint32_t net_read(struct netif *, unsigned reg, unsigned width);
void net_write(struct netif *, unsigned reg, uint32_t value, unsigned width);
void net_dma_barrier(void);
unsigned net_take_pending(struct netif *);
bool net_expired(struct netif *, uint64_t deadline);
void net_tx_complete(struct netif *, unsigned slot, bool ok);

/* Thread context, IF=1. Attach is also the register-model boundary; claims,
 * DMA allocation, PCI admission, init, start and rollback are production. */
int netif_attach(struct netif *, const struct pci_func *, const struct net_io *);
int netif_transmit(struct netif *, gen_t, const void *, unsigned length);
int netif_stop(struct netif *);
void netif_quarantine(struct netif *, const char *reason);
bool netif_irq(struct netif *);
void netif_poll(struct netif *);
bool netif_pop(struct netif *, struct net_packet *);
void netif_snapshot(struct netif *, struct net_stats *);
void net_mac_text(const uint8_t address[6], char text[18]);
const char *net_link_text(enum net_link);

typedef void (*net_input_fn)(struct netif *, const uint8_t *, unsigned, void *);
/* Called only by the net worker with IF=1 and no device mutex. Frame borrowed
 * until return; consumer must copy and return/yield within 1 ms. Registration
 * is thread-only; clearing synchronizes with an in-progress callback.
 * A consumer may transmit a response, but cannot replace its own callback. */
void net_set_input(net_input_fn, void *);
int net_init(void);
void net_shutdown(void);
unsigned net_count(void);
struct netif *net_get(unsigned);
void net_worker_step(void);
unsigned net_arp_request(uint8_t frame[60], const uint8_t mac[6]);
bool net_arp_reply(const uint8_t *, unsigned, const uint8_t local[6], uint8_t peer[6]);
#endif
