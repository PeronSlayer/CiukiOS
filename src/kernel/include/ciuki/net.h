/* Provisional F6 device seam; reconcile with network-contract.md at F6-00.
 * Sources: docs/validation/f6-01-nic-drivers.md. No imported driver code.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NET_H
#define CIUKI_NET_H
#include <ciuki/registry.h>
#include <ciuki/abi.h>
#include <ciuki/irq_budget.h>

#define NETIF_MAX 2u
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
    uint32_t (*read_mmio)(void *, volatile void *mapping, unsigned reg);
    void (*write_mmio)(void *, volatile void *mapping, unsigned reg, uint32_t value);
};
struct netif_driver {
    const char *name;
    bool (*match)(const struct pci_func *);
    uint32_t aperture;
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
    /* Thread-only under the device mutex. No reset or TX-buffer reuse.
     * NULL fails closed; EBUSY leaves the previous receive mode intact. */
    int (*promiscuous)(struct netif *, bool);
};
struct net_stats {
    uint64_t rx_packets, rx_bytes, rx_errors, rx_dropped;
    uint64_t tx_submitted, tx_completed, tx_bytes, tx_errors, tx_busy;
    uint64_t interrupts, budget_violations, max_irq_us, quarantines;
    uint64_t smi_attributed;
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
    uint32_t mmio_phys;
    volatile void *mmio;
    uint8_t address[6], phy, eeprom_bits, next_page;
    enum net_link link;
    gen_t generation;
    uint32_t bridge_epoch; /* latched link loss/revocation, including down/up */
    uint32_t config_epoch; /* rejects native frames queued before an apply */
    uint32_t dma_phys;
    uint8_t *dma;
    int claims[3];
    gen_t claim_generation[3];
    struct kmutex mutex;
    struct net_stats stats;
    struct irq_budget irq_budget;
    uint64_t smi_reported;
    struct net_packet queue[NET_QUEUE_MAX], scratch;
    unsigned queue_head, queue_count, rx_offset, rx_head, rx_tail;
    bool tx_used[4];
    uint16_t tx_length[4];
    uint32_t pending, media_control;
    unsigned tx_head;
    unsigned configured_speed, configured_duplex, speed, duplex;
    uint64_t tx_deadline[4], link_deadline;
    bool ready, running, stopping, quarantined, touched, irq_attached, administratively_down;
    bool irq_pending, fault_pending, servicing, promiscuous;
    const char *reason;
};
int net_e100_media(struct netif *, unsigned, unsigned);
int net_e1000_media(struct netif *, unsigned, unsigned);
extern const struct netif_driver net_e100, net_rtl8139, net_ne2000;
extern const struct netif_driver net_e1000, net_pcnet, net_tulip, net_3c90x, net_rhine;
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
int netif_promiscuous(struct netif *, gen_t, bool);
int netif_stop(struct netif *);
void netif_quarantine(struct netif *, const char *reason);
bool netif_irq(struct netif *);
void netif_poll(struct netif *);
bool netif_pop(struct netif *, struct net_packet *);
void netif_snapshot(struct netif *, struct net_stats *);
/* First running adapter, otherwise first detected adapter; no hardware I/O. */
void net_state_snapshot(struct ciuki_net_state *);
struct process;
int net_devctl(struct process *, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes);
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
int net_power_quiesce(void); /* bounded shutdown barrier; errors retain uncertain DMA */
unsigned net_count(void);
struct netif *net_get(unsigned);
void net_worker_step(void);
unsigned net_arp_request(uint8_t frame[60], const uint8_t mac[6]);
bool net_arp_reply(const uint8_t *, unsigned, const uint8_t local[6], uint8_t peer[6]);
#endif
