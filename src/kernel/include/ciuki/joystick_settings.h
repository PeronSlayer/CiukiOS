/* Private worker-owned persistent settings; no public ABI records.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_JOYSTICK_SETTINGS_H
#define CIUKI_JOYSTICK_SETTINGS_H
#include <ciuki/abi.h>
#include <ciuki/joystick.h>
#define JOY_SETTINGS_CONTROLLERS CIUKI_JOYSTICK_LAST
#define JOY_SETTINGS_GAMES CIUKI_DOSVM_GAME_COUNT
struct joystick_settings_io {
    /* Missing returns -ENOENT; truncated/oversized files must return -EIO. */
    int (*read)(void *, const char *, void *, size_t, size_t *);
    /* Commit the complete replacement; called only on the inactive slot. */
    int (*write)(void *, const char *, const void *, size_t);
    void *context;
};
void joystick_settings_start(const struct joystick_settings_io *);
void joystick_settings_service(void); /* thread context, IRQs enabled */
int joystick_settings_calibrate(const struct ciuki_joystick_control *);
int joystick_settings_route(unsigned game, const struct ciuki_joystick_control *);
int joystick_settings_mode(uint32_t device, uint32_t generation, unsigned mode);
int joystick_settings_get_game(struct ciuki_joystick_control *);
void joystick_settings_status(struct ciuki_joystick_control *);
/* Caller holds the input snapshot boundary. Returns true for fallback. */
bool joystick_settings_game(unsigned game, struct joystick_route *);
struct vfs;
void joystick_storage_init(struct vfs *);
void joystick_storage_start(void);
#endif
