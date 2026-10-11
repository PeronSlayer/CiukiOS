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
#include "../../../apps/desktop/desktop.c"
#undef desk_init
#undef main
void desk_init(struct desktop *, const struct desk_ops *, uint32_t, uint32_t);
struct desktop *replay_desktop;
void replay_hook_desk_init(struct desktop *d, const struct desk_ops *ops, uint32_t w, uint32_t h)
{
    replay_desktop = d;
    desk_init(d, ops, w, h);
}
void replay_gate_counts(unsigned out[3])
{
    out[0] = gate_keys; out[1] = gate_motion; out[2] = gate_buttons;
}
