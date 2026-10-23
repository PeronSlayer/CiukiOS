/* Trusted COM lease adapter. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_UART_H
#define CIUKI_DOSVM_UART_H
#include <ciuki/uart16550.h>
struct dosvm_uart {
    struct uart16550 model;
    uint32_t generation;
    int native;
    bool bound, failed;
};
int dosvm_uart_init(struct dosvm *);
void dosvm_uart_close(struct dosvm *);
void dosvm_uart_service(struct dosvm *, uint64_t ms);
int dosvm_uart_bios(struct dosvm *, struct v86_frame *);
int dosvm_uart_port(struct dosvm *, uint16_t, unsigned, bool, uint32_t *, void *);
bool dosvm_uart_irq_claimed(const struct dosvm *, unsigned);
#endif
