/* Adapter to the existing V86 memory, ports, PIC and far-call boundary.
 * Conventional guest frames are retained until VM worker teardown, like the
 * DOS file adapter. This first API accepts only those stable conventional
 * mappings: no VGA, EMS window, pageable XMS or host/user pointer is retained.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dosvm_internal.h"
#include "dosvm_network.h"
#include "dosvm_ne2000.h"
#include "dosvm_ipx.h"
#include "dos_file_hook.h"

#define NETWORK_PINS (DOSVM_IPX_ECBS * (DOSVM_IPX_FRAGMENTS + 2u) + 1u)
struct network_pin { uint32_t linear; uint16_t length; bool used, write; };
struct dosvm_network {
    struct dosvm *vm;
    struct net_bridge *bridge;
    struct dosvm_key key;
    struct dosvm_ne2000 ne;
    struct dosvm_ipx ipx;
    struct network_pin pin[NETWORK_PINS];
    struct v86_frame saved, *dispatch_frame;
    struct dosvm_ipx_upcall active_call;
    uint64_t host_ms;
    uint32_t rx_overflows;
    bool active, saved_vif, allocated;
};
/* Fixed upper bound and no heap failure in a receive path. The packet pool
 * is supplied separately and shared with physical/native network owners. */
static struct dosvm_network networks[BRIDGE_VMS];
_Static_assert(sizeof(networks) <= 256u * 1024u, "DOS network state exceeds ledger bucket");
_Static_assert(DOSVM_IPX_ENTRY >= DOS_HOOK_END, "IPX ROM overlaps DOS hook ROM");
_Static_assert(DOSVM_IPX_RETURN + 3 <= 0x2000, "IPX ROM exceeds F1 page");
static struct dosvm_network *network(const struct dosvm *v)
{
    for (unsigned i = 0; i < BRIDGE_VMS; i++) if (networks[i].allocated && networks[i].vm == v &&
        networks[i].key.id == v->info.key.id && networks[i].key.generation == v->info.key.generation) return &networks[i];
    return 0;
}
static bool live(const struct dosvm_network *n)
{ return n->allocated && n->vm && n->key.generation == n->vm->info.key.generation && n->vm->info.state < DOSVM_STOPPING; }
static int guest_pin(void *arg, uint32_t linear, unsigned bytes, bool write, uint32_t *ticket)
{
    struct dosvm_network *n = arg;
    if (!live(n) || !bytes || bytes > 1500 || linear >= 0xa0000 || bytes > 0xa0000 - linear ||
        !dosvm_range(n->vm, linear, bytes, write)) return -EFAULT;
    for (unsigned i = 0; i < NETWORK_PINS; i++) if (!n->pin[i].used) {
        n->pin[i] = (struct network_pin){linear, (uint16_t)bytes, true, write}; *ticket = i + 1; return 0;
    }
    return -ENOSPC;
}
static void guest_unpin(void *arg, uint32_t ticket)
{
    struct dosvm_network *n = arg;
    if (ticket && ticket <= NETWORK_PINS) memset(&n->pin[ticket - 1], 0, sizeof(n->pin[0]));
}
static bool guest_pinned(struct dosvm_network *n, uint32_t at, unsigned bytes, bool write)
{
    for (unsigned i = 0; i < NETWORK_PINS; i++) {
        const struct network_pin *p = &n->pin[i];
        if (p->used && (!write || p->write) && at >= p->linear && bytes <= p->length &&
            at - p->linear <= p->length - bytes) return true;
    }
    return false;
}
static int guest_read(void *arg, uint32_t at, void *out, unsigned bytes)
{
    struct dosvm_network *n = arg;
    if (!guest_pinned(n, at, bytes, false)) return -EFAULT;
    return dosvm_read(n->vm, at, out, bytes);
}
static int guest_write(void *arg, uint32_t at, const void *data, unsigned bytes)
{
    struct dosvm_network *n = arg;
    if (!guest_pinned(n, at, bytes, true)) return -EFAULT;
    return dosvm_write(n->vm, at, data, bytes);
}
static int bridge_send(void *arg, const uint8_t *p, unsigned length)
{
    struct dosvm_network *n = arg;
    if (!live(n)) return -19;
    return net_bridge_send(n->bridge, (struct bridge_key){n->key.id,n->key.generation}, p, length, n->host_ms);
}
static void virtual_irq(void *arg, bool level)
{
    struct dosvm_network *n = arg;
    if (level && live(n)) (void)dosvm_irq_raise(n->key, DOSVM_NE2000_IRQ);
}
static int virtual_port(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{
    struct dosvm_network *n = arg;
    if (!n || n != network(v) || !live(n) || !value || (width != 1 && width != 2)) return -V86_EPERM;
    if (write) dosvm_ne2000_write(&n->ne, port, width, *value);
    else *value = dosvm_ne2000_read(&n->ne, port, width);
    return 0;
}
static void relinquish(void *arg)
{
    struct dosvm_network *n = arg;
    if (live(n)) { net_bridge_step(n->bridge); kpreempt_point(); }
}
static int guest_upcall(void *arg, const struct dosvm_ipx_upcall *call)
{
    struct dosvm_network *n = arg; struct dosvm *v = n->vm;
    struct v86_frame *f = n->dispatch_frame;
    if (!live(n) || call->generation != n->key.generation) return -EFAULT;
    if (!f || n->active || v->frame_pm || v->mouse.active || v->dpmi.depth ||
        !v->monitor.vif || v->monitor.shadow) return 0;
    if (f->tf.user_ss > 0xffff || (uint16_t)f->tf.user_esp < 4 ||
        !dosvm_range(v, ((uint32_t)call->cs << 4) + call->ip, 1, false)) return -EFAULT;
    uint16_t sp = (uint16_t)f->tf.user_esp - 4;
    uint16_t ret[2] = {DOSVM_IPX_RETURN,0xf000};
    if (dosvm_write(v, (f->tf.user_ss << 4) + sp, ret, sizeof(ret))) return -EFAULT;
    n->saved = *f; n->saved_vif = v->monitor.vif; n->active_call = *call; n->active = true;
    f->tf.user_esp = (f->tf.user_esp & 0xffff0000u) | sp;
    f->tf.cs = call->cs; f->tf.eip = call->ip;
    f->es = call->es; f->tf.esi = (f->tf.esi & 0xffff0000u) | call->si;
    f->tf.eax = (f->tf.eax & ~255u) | call->al;
    f->tf.eflags &= ~(V86_TF | 0x400u);
    v->monitor.vif = false; v->monitor.state = V86_RUNNING; return 1;
}
void dosvm_network_rom_init(struct dosvm *v)
{
    uint8_t *rom = P2V(v->frames[0xf1]);
    static const uint8_t entry[] = {0xcd,0x7a,0xcb}; /* INT7A; RETF */
    static const uint8_t done[] = {0xcd,0xfc,0xf4};
    memcpy(rom + (DOSVM_IPX_ENTRY & 4095), entry, sizeof(entry));
    memcpy(rom + (DOSVM_IPX_RETURN & 4095), done, sizeof(done));
}
bool dosvm_network_irq_available(const struct dosvm *v, unsigned irq)
{
    if (irq != DOSVM_NE2000_IRQ) return true;
    /* IRQ3 is otherwise unallocated in the current ISA devices (SB16=5,
     * PIT=0, keyboard=1). Reject future conflicting SB assignments. */
    return v->audio.sb.irq != irq;
}
int dosvm_network_attach(struct dosvm *v, struct net_bridge *bridge)
{
    if (!v || !bridge || network(v) || !dosvm_network_irq_available(v, DOSVM_NE2000_IRQ)) return -EINVAL;
    struct dosvm_network *n = 0;
    for (unsigned i = 0; i < BRIDGE_VMS; i++) if (!networks[i].allocated) { n = &networks[i]; break; }
    if (!n) return -ENOSPC;
    memset(n, 0, sizeof(*n)); n->allocated = true; n->vm = v; n->key = v->info.key; n->bridge = bridge;
    uint8_t mac[6];
    int error = net_bridge_enable(bridge, (struct bridge_key){n->key.id,n->key.generation}, g_ticks, mac);
    if (error) goto fail;
    const struct dosvm_ne2000_ops ne_ops = {n,bridge_send,virtual_irq};
    dosvm_ne2000_init(&n->ne, mac, &ne_ops);
    const struct dosvm_ipx_ops ipx_ops = {n,guest_pin,guest_unpin,guest_read,guest_write,bridge_send,relinquish,guest_upcall};
    error = dosvm_ipx_init(&n->ipx, n->key.generation, mac, &ipx_ops);
    if (error) goto fail;
    error = dosvm_port_register(n->key, DOSVM_NE2000_BASE, DOSVM_NE2000_LAST, virtual_port, n);
    if (error) goto fail;
    dosvm_network_rom_init(v); return 0;
fail:
    dosvm_ipx_destroy(&n->ipx);
    net_bridge_disable(bridge, (struct bridge_key){n->key.id,n->key.generation});
    memset(n, 0, sizeof(*n)); return error;
}
void dosvm_network_detach(struct dosvm *v)
{
    struct dosvm_network *n = network(v); if (!n) return;
    (void)dosvm_port_unregister(n->key, DOSVM_NE2000_BASE);
    dosvm_ipx_destroy(&n->ipx);
    dosvm_ne2000_reset(&n->ne);
    net_bridge_disable(n->bridge, (struct bridge_key){n->key.id,n->key.generation});
    memset(n, 0, sizeof(*n));
}
void dosvm_network_service(struct dosvm *v, uint64_t host_ms)
{
    struct dosvm_network *n = network(v); if (!n || !live(n)) return;
    n->host_ms = host_ms;
    if (!n->bridge->link) { dosvm_ipx_disconnect(&n->ipx); return; }
    /* Approximate BIOS rate with exact PIT source/reload ratio. Divide first
     * to avoid a long-uptime overflow in host_ms * 1193182. */
    uint64_t ticks = (host_ms / 65536000u) * 1193182u +
        (host_ms % 65536000u) * 1193182u / 65536000u;
    dosvm_ipx_service(&n->ipx, ticks); net_bridge_step(n->bridge);
    const struct bridge_vm *state = net_bridge_vm(n->bridge, (struct bridge_key){n->key.id,n->key.generation});
    if (state && state->rx_overflows != n->rx_overflows) {
        n->rx_overflows = state->rx_overflows; dosvm_ne2000_overflow(&n->ne);
    }
    uint8_t frame[1514];
    /* At most eight frames, bounded by admitted queue size; yield after each
     * complete frame, never while a guest transfer/publication is partial. */
    for (unsigned i = 0; i < BRIDGE_QUEUE; i++) {
        int bytes = net_bridge_receive(n->bridge, (struct bridge_key){n->key.id,n->key.generation}, frame, sizeof(frame));
        if (bytes <= 0) break;
        (void)dosvm_ipx_receive(&n->ipx, frame, (unsigned)bytes); kpreempt_point();
    }
}
int dosvm_network_dispatch(struct dosvm *v, struct v86_frame *f)
{
    struct dosvm_network *n = network(v); if (!n) return 0;
    n->dispatch_frame = f; int result = dosvm_ipx_dispatch(&n->ipx); n->dispatch_frame = 0;
    return result < 0 ? 0 : result; /* Bad ESR remains local, never kills the host. */
}
int dosvm_network_interrupt(struct dosvm *v, struct v86_frame *f, uint8_t vector)
{
    struct dosvm_network *n = network(v); if (!n) return -ENOSYS;
    if (vector == 0xfc) {
        if (!n->active || n->active_call.generation != n->key.generation || f->tf.cs != 0xf000 ||
            f->tf.eip != DOSVM_IPX_RETURN + 2 || f->tf.user_ss != n->saved.tf.user_ss ||
            f->tf.user_esp != n->saved.tf.user_esp) return -V86_EPERM;
        *f = n->saved; n->active = false; v->monitor.vif = n->saved_vif; v->monitor.state = V86_RUNNING; return 0;
    }
    struct dosvm_ipx_regs r = {(uint16_t)f->tf.eax,(uint16_t)f->tf.ebx,(uint16_t)f->tf.ecx,
        (uint16_t)f->tf.edx,(uint16_t)f->tf.esi,(uint16_t)f->tf.edi,(uint16_t)f->es};
    if (!dosvm_ipx_interrupt(&n->ipx, vector, &r)) return -ENOSYS;
    f->tf.eax = (f->tf.eax & 0xffff0000u) | r.ax;
    f->tf.ecx = (f->tf.ecx & 0xffff0000u) | r.cx;
    f->tf.edx = (f->tf.edx & 0xffff0000u) | r.dx;
    f->tf.edi = (f->tf.edi & 0xffff0000u) | r.di; f->es = r.es;
    return 0;
}
