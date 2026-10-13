/* Public read-only network observations; no register or configuration access.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/net.h>

int net_devctl(struct process *p, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes)
{
    if (!file_fd_object(p, fd)) return -EBADF;
    struct file_description *d = file_fd(p, fd);
    if (!d || d->node->kind != PX_NET) return -ENOTTY;
    if (op != NET_GET_STATE) return -ENOTTY;
    if (bytes != sizeof(struct ciuki_net_state)) return -EINVAL;
    struct ciuki_net_state state;
    int err = copy_from_user(&state, address, sizeof(state));
    if (err) return err;
    if (state.version != CIUKI_NET_VERSION) return -EOPNOTSUPP;
    if (state.size != sizeof(state) || state.reserved0[0] || state.reserved0[1] || state.reserved1) return -EINVAL;
    net_state_snapshot(&state);
    return copy_to_user(address, &state, sizeof(state));
}
