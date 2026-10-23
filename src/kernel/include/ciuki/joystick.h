/* Native analog transport and logical joystick subscriptions.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_JOYSTICK_H
#define CIUKI_JOYSTICK_H
/* Kernel-private: no user ABI here, so dosvm_internal.h (which embeds
 * struct joystick_virtual) stays usable by host tests that include the C
 * library first. Units that need the ABI records include <ciuki/abi.h>. */
#include <ciuki/registry.h>
struct ciuki_joystick_control;
struct joystick_route {
    uint32_t device, generation, second_device, second_generation, mode;
};
struct joystick_virtual {
    struct joystick_route route;
    uint64_t now, deadline[4];
    uint8_t measured;
};
uint8_t joystick_virtual_io(struct joystick_virtual *, uint64_t clocks, bool write);
int joystick_virtual_bios(struct joystick_virtual *, uint16_t subfunction, uint16_t registers[4]);
struct gameport_io {
    uint8_t (*read)(void *, uint16_t);
    void (*trigger)(void *, uint16_t);
    uint64_t (*clock)(void *); /* PIT input clocks, monotonic */
    void *context;
};
struct gameport_sample {
    int32_t clocks[4];
    uint8_t axes, buttons;
};
/* IF remains enabled across the bounded RC loop. Gap-corrupted reads retry. */
int gameport_measure(const struct gameport_io *, uint16_t, struct gameport_sample *);
int gameport_admit(uint16_t base, const struct pci_func *, const struct gameport_io *);
int gameport_pci_base(const struct pci_func *, uint16_t *);
void gameport_poll(void);
void gameport_init(void);
int gameport_hid_query(uint32_t, void *);
int joystick_hid_query(uint32_t, void *);
int joystick_control(struct ciuki_joystick_control *);
int joystick_route_validate(const struct joystick_route *);
uint32_t joystick_session(void);
int joystick_identity(uint32_t, uint32_t, uint8_t[32]);
int gameport_restore_mode(uint32_t, uint32_t, unsigned);
void joystick_default_route(struct joystick_route *);
/* Stable cache only; no physical I/O. Missing/stale identities give no axes. */
void joystick_snapshot(const struct joystick_route *, int32_t axes[4], uint8_t *mask, uint8_t *buttons);
bool gameport_inventory(unsigned, uint16_t *, const struct pci_func **, bool *, const char **);
#endif
