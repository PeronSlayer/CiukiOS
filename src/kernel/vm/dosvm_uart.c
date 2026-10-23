/* VMM-owned physical COM leases behind per-VM UARTs and INT14 services.
 * Windows 95/98 V86 device model; no host I/O bitmap access.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/com.h>
#include "dosvm_internal.h"
static const uint16_t uart_base[4]={0x3f8,0x2f8,0x3e8,0x2e8};
static unsigned uart_irq(unsigned slot) { return slot&1 ? 3 : 4; }
bool dosvm_uart_irq_claimed(const struct dosvm *v, unsigned irq)
{
    for (unsigned i=0;i<4;i++) if (uart_irq(i)==irq && v->uart[i].bound &&
        (v->uart[i].native>=0 || ((v->uart[i].model.mcr&8) && v->uart[i].model.ier))) return true;
    return false;
}
static bool peer_irq(const struct dosvm *v, unsigned irq)
{
    if (v->audio.sb.irq==irq) return true;
    if (irq==3) for (unsigned i=0;i<DOSVM_PORTS;i++)
        if (v->ports[i].handler && v->ports[i].first==0x300) return true; /* virtual NE2000 */
    return false;
}
static void update_irq(struct dosvm *v)
{
    for (unsigned line=0;line<2;line++) {
        unsigned irq=line+3; bool high=false;
        for (unsigned i=0;i<4;i++) if (uart_irq(i)==irq && v->uart[i].bound &&
            uart16550_irq(&v->uart[i].model)) high=true;
        if (high && !v->uart_irq[line]) (void)dosvm_irq_raise(v->info.key,irq);
        v->uart_irq[line]=high;
    }
}
static int configure(struct dosvm_uart *s, bool line)
{
    if (s->native<0) return 0;
    struct uart16550 *u=&s->model;
    if (line) {
        unsigned divisor=u->dll|((unsigned)u->dlm<<8);
        if (!divisor || 115200%divisor || (u->lcr&0x80)) return -EINVAL;
        struct ciuki_com_line l={.version=1,.size=sizeof(l),.baud=115200/divisor,
            .data_bits=5+(u->lcr&3),.stop_bits=u->lcr&4 ? 2 : 1,
            .parity=!(u->lcr&8) ? 0 : (u->lcr&0x20) ? ((u->lcr&0x10)?4:3) : ((u->lcr&0x10)?2:1)};
        struct ciuki_com_line old={.version=1,.size=sizeof(old)};
        int e=com_control((unsigned)s->native,s->generation,COM_GET_LINE,&old,sizeof(old),false);
        if (!e && memcmp(&l,&old,sizeof(l))) e=com_control((unsigned)s->native,s->generation,COM_SET_LINE,&l,sizeof(l),true);
        if (e) return e;
    }
    struct ciuki_com_modem m={1,sizeof(m),(u->mcr&0x10 ? 0 : u->mcr&3)|(u->lcr&0x40 ? CIUKI_COM_BREAK : 0),0};
    return com_control((unsigned)s->native,s->generation,COM_SET_MODEM,&m,sizeof(m),true);
}
static int send_byte(void *arg, uint8_t b)
{
    struct dosvm_uart *s=arg;
    if (s->failed) return -EIO;
    if (s->native<0) return 0;
    int e=com_transfer((unsigned)s->native,s->generation,&b,1,true);
    return e==1 ? 0 : e;
}
void dosvm_uart_service(struct dosvm *v, uint64_t ms)
{
    if (v->info.state>=DOSVM_STOPPING) return;
    uint32_t flags=irq_save();
    for (unsigned i=0;i<4;i++) {
        struct dosvm_uart *s=&v->uart[i]; if (!s->bound) continue;
        bool ready=true;
        if (s->native>=0 && !s->failed) {
            struct ciuki_com_status status={.version=1,.size=sizeof(status)};
            int e=com_control((unsigned)s->native,s->generation,COM_GET_STATUS,&status,sizeof(status),false);
            if (e) { s->failed=true; s->model.overrun|=8; uart16550_modem(&s->model,0); }
            else {
                uart16550_modem(&s->model,(uint8_t)status.modem);
                ready=com_transmitter_idle((unsigned)s->native,s->generation);
                int errors=com_vm_line_errors((unsigned)s->native,s->generation);
                if (errors<0) { s->failed=true; s->model.overrun|=8; errors=0; }
                bool loop=!!(s->model.mcr&0x10);
                if (!loop) s->model.overrun|=errors&2;
                for (unsigned n=0;n<16;n++) {
                    uint8_t b; e=com_transfer((unsigned)s->native,s->generation,&b,1,false);
                    if (e!=1) { if (e!=-EAGAIN) { s->failed=true; s->model.overrun|=8; } break; }
                    if (!loop) uart16550_receive(&s->model,b,(uint8_t)errors,ms*1000);
                    errors=0;
                }
                if (!loop) s->model.overrun|=errors&0x1c;
            }
        }
        uart16550_advance(&s->model,ms*1000,send_byte,s,ready);
    }
    update_irq(v); irq_restore(flags);
}
static int uart_bda(struct dosvm *v)
{
    unsigned count=0;
    for (unsigned i=0;i<4;i++) {
        uint16_t base=v->uart[i].bound ? uart_base[i] : 0;
        count+=v->uart[i].bound;
        if (dosvm_write(v,0x400+2*i,&base,2)) return -EFAULT;
    }
    uint16_t equipment;
    if (dosvm_read(v,0x410,&equipment,2)) return -EFAULT;
    equipment=(equipment&~0x0e00u)|(count<<9);
    return dosvm_write(v,0x410,&equipment,2);
}
int dosvm_uart_bind(struct dosvm_key key, unsigned slot, int native, const char **reason)
{
    if (reason) *reason="VM COM binding requires an unstarted VM and a valid slot.";
    struct dosvm *v=dosvm_lookup(key);
    if (!v || !dosvm_valid(key) || slot>=4 || native< -2 || native>=(int)CIUKI_COM_MAX) return -EINVAL;
    if (v->started || v->info.state>=DOSVM_STOPPING) return -EBUSY;
    if (native>=0 && peer_irq(v,uart_irq(slot))) {
        if (reason) *reason="Virtual COM IRQ is owned by another VM device.";
        return -EBUSY;
    }
    struct dosvm_uart *s=&v->uart[slot];
    if (s->bound && s->native==native) { if (reason) *reason="VM COM binding unchanged."; return 0; }
    struct dosvm_uart candidate={.native=native,.bound=native!= -2}; uart16550_init(&candidate.model);
    if (native>=0) {
        int e=com_lease_open((unsigned)native,CIUKI_COM_OWNER_VM,key.id,&candidate.generation,reason);
        if (e) return e;
        e=configure(&candidate,true);
        if (e) { com_close((unsigned)native,candidate.generation); if (reason) *reason="Physical COM configuration refused."; return e; }
    }
    if (s->bound && s->native>=0) com_close((unsigned)s->native,s->generation);
    *s=candidate; update_irq(v);
    if (reason) *reason=native<0 ? "VM COM bound to null device." : "VM owns physical COM lease.";
    return uart_bda(v);
}
int dosvm_uart_port(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{
    (void)arg;
    if (!value || (width!=1 && width!=2 && width!=4)) return -EINVAL;
    if (v->info.state>=DOSVM_STOPPING) return -ENODEV;
    unsigned slot=0;
    while (slot<4 && !(port>=uart_base[slot] && port<uart_base[slot]+8)) slot++;
    if (slot==4) return -ENODEV;
    if (!v->uart[slot].bound) {
        if (!write) *value=width==4 ? UINT32_MAX : width==2 ? 0xffff : 0xff;
        return 0;
    }
    if (width!=1) return -EINVAL;
    dosvm_uart_service(v,g_ticks);
    struct dosvm_uart *s=&v->uart[slot]; unsigned reg=port-uart_base[slot];
    uint32_t flags=irq_save();
    if (write) {
        struct uart16550 before=s->model;
        uart16550_write(&s->model,reg,(uint8_t)*value,g_ticks*1000);
        if (peer_irq(v,uart_irq(slot)) && (s->model.mcr&8) && s->model.ier) {
            s->model=before; s->model.overrun|=8;
        } else if ((reg==3 && !(s->model.lcr&0x80)) || reg==4) {
            int e=configure(s,reg==3);
            if (e) { s->model=before; s->model.overrun|=8; }
        }
    } else *value=uart16550_read(&s->model,reg,g_ticks*1000);
    update_irq(v); irq_restore(flags); return 0;
}
int dosvm_uart_init(struct dosvm *v)
{
    for (unsigned i=0;i<4;i++) {
        v->uart[i]=(struct dosvm_uart){.native=-1,.bound=true}; uart16550_init(&v->uart[i].model);
        int e=dosvm_port_register(v->info.key,uart_base[i],uart_base[i]+7,dosvm_uart_port,0);
        if (e) return e;
        uint8_t timeout=10; /* SeaBIOS rel-1.16.3 serial_setup: ten BIOS ticks */
        if (dosvm_write(v,0x400+2*i,&uart_base[i],2) || dosvm_write(v,0x47c+i,&timeout,1)) return -EFAULT;
    }
    return uart_bda(v);
}
void dosvm_uart_close(struct dosvm *v)
{
    for (unsigned i=0;i<4;i++) {
        struct dosvm_uart *s=&v->uart[i];
        if (s->bound && s->native>=0) com_close((unsigned)s->native,s->generation);
        memset(s,0,sizeof(*s)); s->native=-1;
    }
    v->uart_irq[0]=v->uart_irq[1]=false;
}
int dosvm_uart_bios(struct dosvm *v, struct v86_frame *f)
{
    unsigned slot=(uint16_t)f->tf.edx, fn=(f->tf.eax>>8)&255; uint8_t data=(uint8_t)f->tf.eax;
    if (slot>=4 || !v->uart[slot].bound || fn>3 || v->info.state>=DOSVM_STOPPING) {
        f->tf.eax=(f->tf.eax&0xffff00ff)|0x8000; return 0;
    }
    struct dosvm_uart *s=&v->uart[slot]; struct uart16550 *u=&s->model;
    if (fn==0) {
        static const unsigned divisors[]={1047,768,384,192,96,48,24,12};
        unsigned div=divisors[data>>5]; struct uart16550 before=*u;
        u->dll=(uint8_t)div; u->dlm=(uint8_t)(div>>8); u->lcr=data&31;
        if (configure(s,true)) { *u=before; u->overrun|=8; }
    }
    uint8_t status=0, modem=0;
    uint8_t timeout=0; (void)dosvm_read(v,0x47c+slot,&timeout,1);
    /* SeaBIOS clock.c: BDA timeout is in 18.2-Hz ticks, not seconds.
     * Round the PIT's 65536/1193182-second tick up to a host millisecond.
     * Zero is the explicit immediate-I/O policy for this virtual BIOS. */
    uint64_t until=g_ticks+((uint64_t)timeout*65536000+1193181)/1193182;
    for (unsigned polls=0;polls<255001;polls++) {
        dosvm_uart_service(v,g_ticks); status=uart16550_read(u,5,g_ticks*1000);
        if (fn==0 || fn==3) { modem=uart16550_read(u,6,g_ticks*1000); data=modem; break; }
        if (fn==1 && (status&0x60)==0x60 && !s->failed) {
            uart16550_write(u,0,data,g_ticks*1000); break;
        }
        if (fn==2 && (status&1)) { data=uart16550_read(u,0,g_ticks*1000); break; }
        if (v->info.state>=DOSVM_STOPPING || g_ticks>=until || !(read_eflags()&0x200)) { status|=0x80; break; }
        task_sleep_ms(1);
    }
    update_irq(v); f->tf.eax=(f->tf.eax&0xffff0000)|((uint32_t)status<<8)|data;
    return 0;
}
