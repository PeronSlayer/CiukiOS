/* SPDX-License-Identifier: MIT
 * The production desktop (apps/desktop/desktop.c, unchanged) as one process
 * of the app-case replay: main becomes desktop_main, and two read-only hooks
 * expose its state to the harness (the struct desktop it initialises and the
 * gate_input counters behind its interaction reports). */
#define main desktop_main
#define desk_init replay_hook_desk_init
/* desktop.c also passes close and munmap as desk_ops function pointers. */
#undef close
#undef munmap
#define close rp_close
#define munmap rp_munmap
#include "replay_display.h"
#include "../../../apps/desktop/desktop.c"
#undef desk_init
#undef main
/* Replay fixtures keep every bundled game available, like the desktop gate. */
int ciuki_dosvm(uint32_t op, struct ciuki_dosvm_command *c)
{
    if (op == CIUKI_DOSVM_GAME_QUERY) { c->state = 1; return 0; }
    errno = ENOSYS;
    return -1;
}
void desk_init(struct desktop *, const struct desk_ops *, uint32_t, uint32_t);
struct desktop *replay_desktop;
void replay_hook_desk_init(struct desktop *d, const struct desk_ops *ops, uint32_t w, uint32_t h)
{
    replay_desktop = d;
    desk_init(d, ops, w, h);
}
void desk_sound_init(struct desktop *d) { (void)d; }
void desk_sound_results(struct desktop *d) { (void)d; }
void replay_gate_counts(unsigned out[3])
{
    out[0] = gate_keys; out[1] = gate_motion; out[2] = gate_buttons;
}

int ap_preference_get(const char *dir,const char *id) { (void)dir; (void)id; return -1; }
int ap_preference_set(const char *dir,const char *id,const char *label,int action)
{ (void)dir; (void)id; (void)label; (void)action; return -1; }

/* This desktop fixture has no installed SETUP.STATE. */
int desk_firstboot(struct desktop *d) { (void)d; return 0; }
/* The app replay does not start the scheduled-tasks service: desktop.c
 * supplies the weak desk_schedsvc_exited default. */
