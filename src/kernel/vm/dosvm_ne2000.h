/* ISA NE2000 register model; no physical I/O. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_NE2000_H
#define CIUKI_DOSVM_NE2000_H
#include <ciuki/kernel.h>
#define DOSVM_NE2000_BASE 0x300u
#define DOSVM_NE2000_LAST 0x31fu
#define DOSVM_NE2000_IRQ 3u
struct dosvm_ne2000_ops {
    void *arg;
    int (*send)(void *, const uint8_t *, unsigned);
    /* Virtual level only. The adapter latches a rising edge in the VM PIC;
     * guest IF, PIC masking and EOI remain the monitor's responsibility. */
    void (*irq)(void *, bool level);
};
struct dosvm_ne2000 {
    uint8_t ram[16384], prom[32], assigned[6], par[6], mar[8];
    uint8_t command, pstart, pstop, boundary, current, tpsr, tsr, rsr;
    uint8_t isr, imr, rcr, tcr, dcr, counters[3], local_next, remote_next, fifo;
    uint16_t remote, remote_start, count, tx_count, local, address_count;
    uint32_t generation, bad_dma, dropped;
    bool irq_level;
    struct dosvm_ne2000_ops ops;
};
void dosvm_ne2000_init(struct dosvm_ne2000 *, const uint8_t mac[6], const struct dosvm_ne2000_ops *);
void dosvm_ne2000_reset(struct dosvm_ne2000 *);
uint32_t dosvm_ne2000_read(struct dosvm_ne2000 *, uint16_t port, unsigned width);
void dosvm_ne2000_write(struct dosvm_ne2000 *, uint16_t port, unsigned width, uint32_t);
/* 1 stored, 0 filtered/stopped, negative malformed/overflow. */
int dosvm_ne2000_receive(struct dosvm_ne2000 *, const uint8_t *, unsigned);
void dosvm_ne2000_overflow(struct dosvm_ne2000 *);
#endif
