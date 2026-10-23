/* Per-VM 16550A state. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_UART16550_H
#define CIUKI_UART16550_H
#include <stdint.h>
#include <stdbool.h>
struct uart16550 {
    uint8_t rx[16], errors[16], tx[16];
    uint8_t rx_head, rx_count, tx_head, tx_count;
    uint8_t ier, lcr, mcr, fcr, scr, dll, dlm, msr, external, overrun;
    bool thre, timeout, shifting;
    uint8_t shift;
    uint64_t rx_time, tx_due;
};
void uart16550_init(struct uart16550 *);
uint8_t uart16550_read(struct uart16550 *, unsigned reg, uint64_t us);
void uart16550_write(struct uart16550 *, unsigned reg, uint8_t, uint64_t us);
void uart16550_receive(struct uart16550 *, uint8_t, uint8_t errors, uint64_t us);
void uart16550_modem(struct uart16550 *, uint8_t inputs);
bool uart16550_irq(const struct uart16550 *);
uint32_t uart16550_character_us(const struct uart16550 *);
/* Sender: 0 accepted, -EAGAIN retries, other errors become local line errors.
 * ready is true only when the physical backend has proved transmitter idle. */
void uart16550_advance(struct uart16550 *, uint64_t us,
                      int (*send)(void *, uint8_t), void *, bool ready);
#endif
