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
/* --- sec-user-05: per-session preferences (joystick_settings.c) ---
 * Selects the store for the following operations: session 0 the machine
 * store, otherwise that session's profile store, prepared by bind() while
 * the worker's storage pass is excluded, loaded and saved as `subject`
 * (retained while the scope is active). -EAGAIN if storage stays busy. */
struct security_subject;
struct process;
int joystick_settings_scope(uint32_t session, struct security_subject *subject,
                            int (*bind)(void *, struct joystick_settings_io *), void *ctx);
uint32_t joystick_settings_scope_session(void);
void joystick_settings_session_end(uint32_t session);
/* The settings worker's sleep, installed when it starts (gameport.c). */
extern void (*joystick_settings_pause)(uint32_t ms);
/* joystick_storage.c: the caller's scope (SYSTEM: machine; an interactive
 * session with a committed profile: its own store) before a joystick
 * operation. -EACCES for anything else. */
int joystick_settings_select(struct process *caller);
/* --- end sec-user-05 --- */
#endif
