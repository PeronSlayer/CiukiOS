/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_COM_H
#define CIUKI_COM_H
#include <ciuki/abi.h>
#include <ciuki/registry.h>
#include <ciuki/arch.h>
struct file_description;
struct process;
struct com_io {
    uint8_t (*read)(void *, uint16_t);
    void (*write)(void *, uint16_t, uint8_t);
    void (*pause)(void *); /* thread context, one millisecond in the real adapter */
    void *context;
};
/* Assigned resources only. ISA routes are edge IRQ3/4; PCI requires route proof.
 * No arbitrary clock/stride probing: callers supply facts from an exact layout. */
struct com_resource {
    uint32_t number, base, irq, origin, clock, stride;
    bool routed;
};
void com_init(void);
int com_admit(const struct com_resource *, const struct com_io *);
/* f1-109 producer submits assigned PNP0500/PNP0501 resources; no ISA isolation. */
int com_pnp_assigned(const char *id, uint32_t base, uint32_t length, unsigned irq);
void com_bda_discover(const uint16_t bases[4]);
void com_pci_discover(const struct pci_func *);
bool com_snapshot(unsigned index, struct ciuki_com_status *, const char **reason);
bool com_legacy_attach(unsigned irq, irq_handler_t logger) __attribute__((weak));
int com_open(unsigned index, uint32_t *generation);
void com_close(unsigned index, uint32_t generation);
int com_transfer(unsigned index, uint32_t generation, void *, unsigned bytes, bool write);
int com_wait(unsigned index, uint32_t generation, bool write, unsigned timeout_ms,
             bool (*cancelled)(void));
int com_control(unsigned index, uint32_t generation, uint32_t op, void *record, unsigned size, bool writable);
int com_device_open(struct file_description *, uint32_t flags);
void com_device_close(struct file_description *);
int com_rw_user(struct file_description *, uint32_t address, uint32_t bytes, bool write);
int com_devctl(struct process *, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes);
#endif
