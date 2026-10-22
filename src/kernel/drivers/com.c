/* UART service and aggregate ISA owner. Register facts: TI TL16C550D/750,
 * Exar ST16C650A; docs/design/peripheral-driver-coverage.md, COM v1.
 * Detection algorithm informed by Linux v6.12 8250_port.c (GPL-2.0+),
 * based on serial.c by Linus Torvalds/Theodore Ts'o, split by Russell King
 * (C) 2001. Ciuki adaptation: restored probes, measured depth, fixed budget,
 * registry leases, no Linux TTY framework or copied upstream code.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/com.h>
#include <ciuki/task.h>
#include <ciuki/work.h>

struct com_port {
    struct com_resource resource;
    struct com_io io;
    struct ciuki_com_status status;
    struct ciuki_com_line line;
    struct kwait wait;
    uint8_t *queues;
    unsigned rx_head, tx_head;
    uint8_t ier, lcr, mcr, fcr;
    int claim;
    gen_t claim_generation;
    int irq_claim;
    gen_t irq_generation;
    bool present, deferred, closing, has_efr;
    uint32_t deferred_generation;
    const char *reason;
};
static struct com_port ports[CIUKI_COM_MAX];
static const char *const names[CIUKI_COM_MAX] = {
    "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8"
};
static irq_handler_t log_handler[2];
static bool legacy_attached[2], initialized;
static bool irq_owned[16];
static uint32_t open_generation;
static uint8_t real_read(void *c, uint16_t p) { (void)c; return inb(p); }
static void real_write(void *c, uint16_t p, uint8_t v) { (void)c; outb(p,v); }
static void real_pause(void *c) { (void)c; task_sleep_ms(1); }
static const struct com_io real_io = { real_read, real_write, real_pause, 0 };
static uint8_t rd(struct com_port *p, unsigned r)
{ return p->io.read(p->io.context,(uint16_t)(p->resource.base+r*p->resource.stride)); }
static void wr(struct com_port *p, unsigned r, uint8_t v)
{ p->io.write(p->io.context,(uint16_t)(p->resource.base+r*p->resource.stride),v); }
static bool leases_valid(struct com_port *p)
{
    if (p->claim<0 || !registry_valid(p->claim,p->claim_generation)) return false;
    if (p->irq_claim>=0) return registry_valid(p->irq_claim,p->irq_generation);
    /* Borrowed aggregate/logger IRQ claim. The registry's fixed 64-slot
     * inventory bounds this read-only check; no independent physical claim. */
    for (unsigned i=0;i<registry_count();i++) {
        const struct resource *r=registry_get(i);
        if (r && r->type==RES_IRQ && r->start==p->resource.irq && r->end==r->start+1 &&
            (!strncmp(r->owner,"uart",5) || !strncmp(r->owner,"com-isa",8)) &&
            registry_valid((int)i,r->generation)) return true;
    }
    return false;
}
static bool live(struct com_port *p, uint32_t generation)
{ return p->queues && generation && p->status.generation==generation && (p->status.flags&CIUKI_COM_READY) && leases_valid(p); }

static unsigned depth_probe(struct com_port *p, uint8_t fcr)
{
    /* Private loopback, no external bytes. Divisor 1, wait a character
     * between writes, including the last, then count the bounded RX FIFO.
     * All modified registers are restored by probe(), even on failure. */
    wr(p,3,0x80); wr(p,0,1); wr(p,1,0);
    if (rd(p,0)!=1 || rd(p,1)!=0) return 0;
    wr(p,3,3);
    wr(p,4,0x10);
    wr(p,3,0x80); wr(p,2,fcr|6); wr(p,3,3);
    for (unsigned i=0;i<65;i++) {
        if (!(rd(p,5)&0x20)) return 1;
        wr(p,0,(uint8_t)i); p->io.pause(p->io.context);
    }
    unsigned n=0;
    while (n<65 && (rd(p,5)&1)) { (void)rd(p,0); n++; }
    return n;
}
static int probe(struct com_port *p)
{
    uint8_t lcr=rd(p,3), mcr, ier, scr, dll, dlm, efr=0;
    if (lcr==0xff) return -ENODEV;
    wr(p,3,0); ier=rd(p,1); mcr=rd(p,4); scr=rd(p,7);
    /* FCR cannot be read. Refuse unknown inherited FIFO state. */
    if ((rd(p,2)&0xc0) || (ier&15)) { wr(p,3,lcr); return -EBUSY; }
    uint8_t idle=rd(p,5);
    if (idle==0xff || (idle&1) || !(idle&0x40)) { wr(p,3,lcr); return -EBUSY; }
    wr(p,1,0); wr(p,4,0x1a);
    bool exists=(rd(p,6)&0xf0)==0x90; /* LOOP maps RTS/OUT2 to CTS/DCD */
    wr(p,4,mcr);
    if (!exists) { wr(p,1,ier); wr(p,3,lcr); return -ENODEV; }
    wr(p,3,0x80); dll=rd(p,0); dlm=rd(p,1);
    wr(p,3,0); wr(p,7,0xa5); bool scratch=rd(p,7)==0xa5;
    wr(p,7,0x5a); scratch=scratch && rd(p,7)==0x5a; wr(p,7,scr);
    wr(p,2,1); uint8_t fifo=rd(p,2)&0xc0;
    unsigned chip=scratch ? CIUKI_COM_16450 : CIUKI_COM_8250, depth=1;
    bool enhanced=false, divisor_ok=true;
    if (fifo==0x80) chip=CIUKI_COM_16550;
    else if (fifo==0xc0) {
        chip=CIUKI_COM_16550A; depth=16;
        wr(p,3,0xbf); efr=rd(p,2);
        wr(p,2,0x10); enhanced=rd(p,2)==0x10;
        wr(p,2,0); enhanced=enhanced && rd(p,2)==0;
        wr(p,3,0);
        wr(p,2,0x21); unsigned plain=rd(p,2)&0xe0;
        wr(p,2,1); wr(p,3,0x80); wr(p,2,0x21);
        unsigned banked=rd(p,2)&0xe0; wr(p,2,1); wr(p,3,0);
        uint8_t mode=1;
        if (!enhanced && plain==0xc0 && banked==0xe0) {
            chip=CIUKI_COM_16750; mode=0x21;
        }
        unsigned measured=depth_probe(p,mode);
        divisor_ok=measured!=0;
        if (chip==CIUKI_COM_16750 && measured==64) depth=64;
        else if (enhanced && measured==32) { chip=CIUKI_COM_16650; depth=32; }
        else { chip=CIUKI_COM_16550A; depth=measured>=16 ? 16 : 1; }
    }
    /* Restore every readable configuration and the known FIFO-off baseline. */
    if (enhanced) { wr(p,3,0xbf); wr(p,2,efr); }
    wr(p,3,0x80); wr(p,2,0); wr(p,0,dll); wr(p,1,dlm);
    wr(p,3,0); wr(p,2,0); wr(p,4,mcr); wr(p,7,scr); wr(p,1,ier); wr(p,3,lcr);
    p->status.chip=chip; p->status.fifo_depth=depth; p->has_efr=enhanced;
    return divisor_ok ? 0 : -EIO;
}

static void quarantine(struct com_port *p, const char *reason)
{
    wr(p,1,0); p->status.flags&=~CIUKI_COM_READY;
    p->status.flags|=CIUKI_COM_QUARANTINED; p->reason=reason;
    if (p->claim>=0) registry_quarantine(p->claim,p->claim_generation);
    kwait_wake_irq(&p->wait,false);
}
static void service_deferred(void *arg);
static void transmit(struct com_port *p)
{
    unsigned n=p->status.tx_queued;
    if (n>p->status.fifo_depth) n=p->status.fifo_depth;
    if (n>8) n=8;
    while (n--) {
        wr(p,0,p->queues[CIUKI_COM_QUEUE+p->tx_head]);
        p->tx_head=(p->tx_head+1)%CIUKI_COM_QUEUE;
        p->status.tx_queued--; p->status.transmitted++;
    }
    if (!p->status.tx_queued) p->ier&=~2u;
}
static bool service(struct com_port *p)
{
    if (!live(p,p->status.generation) || p->deferred) return false;
    uint8_t iir=rd(p,2);
    if (iir==0xff) { quarantine(p,"UART stopped responding; resources retained."); return true; }
    if (iir&1) return false;
    unsigned cause=iir&14;
    /* One cause and <=8 bytes per port/pass (8 slots plus logger fit the
     * existing aggregate IRQ budget). A deferred continuation drains
     * asserted RX before rearming the edge; logger callback always runs. */
    if (cause==4 || cause==12 || cause==6) {
        unsigned n=0; uint8_t lsr=0;
        for (;n<8;n++) {
            lsr=rd(p,5); p->status.errors|=lsr&0x1e;
            if (!(lsr&1)) break;
            uint8_t c=rd(p,0); p->status.received++;
            if (p->status.rx_queued==CIUKI_COM_QUEUE) p->status.dropped++;
            else { p->queues[(p->rx_head+p->status.rx_queued)%CIUKI_COM_QUEUE]=c; p->status.rx_queued++; }
        }
    } else if (cause==2) {
        transmit(p);
    } else if (cause==0) {
        uint8_t msr=rd(p,6); p->status.modem=(p->status.modem&15)|(msr&0xff);
    } else quarantine(p,"Unknown UART interrupt; resources retained.");
    /* ISA IRQ is edge-triggered: other causes may already hold the line
     * asserted. Mask this UART and finish in a bounded worker, then rearm. */
    if (p->status.flags&CIUKI_COM_READY) {
        wr(p,1,0); p->deferred=true; p->deferred_generation=p->status.generation;
        if (!kwork_queue(service_deferred,p)) { p->deferred=false; quarantine(p,"IRQ continuation unavailable; resources retained."); }
    }
    kwait_wake_irq(&p->wait,true);
    return true;
}
static void service_deferred(void *arg)
{
    struct com_port *p=arg;
    uint32_t f=irq_save(); p->deferred=false;
    if (live(p,p->deferred_generation)) {
        /* IIR suppresses causes with IER=0. Drain RX from LSR instead. */
        unsigned n=0;
        for (;n<16;n++) {
            uint8_t lsr=rd(p,5); p->status.errors|=lsr&0x1e;
            if (lsr==0xff) { quarantine(p,"UART stopped responding; resources retained."); break; }
            if (!(lsr&1)) break;
            uint8_t c=rd(p,0); p->status.received++;
            if (p->status.rx_queued==CIUKI_COM_QUEUE) p->status.dropped++;
            else { p->queues[(p->rx_head+p->status.rx_queued)%CIUKI_COM_QUEUE]=c; p->status.rx_queued++; }
        }
        /* RX has higher IIR priority. Service TX too so continuous incoming
         * traffic cannot starve a full-duplex application's output. */
        if ((p->status.flags&CIUKI_COM_READY) && p->status.tx_queued && (rd(p,5)&0x20)) transmit(p);
        if (n==16) {
            p->deferred=true;
            if (!kwork_queue(service_deferred,p)) { p->deferred=false; quarantine(p,"RX continuation unavailable; resources retained."); }
        } else if (p->status.flags&CIUKI_COM_READY) {
            uint8_t msr=rd(p,6); p->status.modem=(p->status.modem&15)|msr;
            wr(p,1,p->ier);
        }
        kwait_wake_irq(&p->wait,true);
    }
    irq_restore(f);
}
static void legacy_irq(struct trap_frame *tf)
{
    unsigned irq=tf->vector-0x20;
    if (irq!=3 && irq!=4) return;
    if (log_handler[irq-3]) log_handler[irq-3](tf);
    for (unsigned i=0;i<CIUKI_COM_MAX;i++)
        if (ports[i].present && ports[i].resource.origin!=CIUKI_COM_PCI && ports[i].resource.irq==irq)
            (void)service(&ports[i]);
}
bool com_legacy_attach(unsigned irq, irq_handler_t logger)
{
    if (irq!=3 && irq!=4) return false;
    if (logger) log_handler[irq-3]=logger;
    if (!legacy_attached[irq-3]) { irq_set_handler(irq,legacy_irq); legacy_attached[irq-3]=true; }
    return true;
}
static bool pci_irq(unsigned irq, const char *owner)
{
    bool handled=false;
    for (unsigned i=0;i<CIUKI_COM_MAX;i++)
        if (ports[i].present && ports[i].resource.origin==CIUKI_COM_PCI && ports[i].resource.irq==irq &&
            !strncmp(names[i],owner,5)) handled=service(&ports[i]) || handled;
    return handled;
}
static int reserve_route(struct com_port *p, unsigned index)
{
    unsigned irq=p->resource.irq;
    if (p->resource.origin!=CIUKI_COM_PCI) {
        if (!irq_owned[irq]) {
            bool logger=false;
            for (unsigned i=0;i<registry_count();i++) {
                const struct resource *r=registry_get(i);
                if (r && r->type==RES_IRQ && r->start==irq && r->end==irq+1 &&
                    r->state!=RS_RELEASED && r->state!=RS_QUARANTINED && !strncmp(r->owner,"uart",5)) logger=true;
            }
            if (!logger) {
                int h=registry_claim(RES_IRQ,irq,irq+1,"com-isa",false);
                if (h<0) return h;
                p->irq_claim=h; p->irq_generation=registry_get((unsigned)h)->generation;
            }
            irq_owned[irq]=true;
        }
        if (p->irq_claim<0 && !leases_valid(p)) return -EBUSY;
        return 0;
    }
    int h=registry_claim(RES_IRQ,irq,irq+1,names[index],true);
    if (h<0) return h;
    p->irq_claim=h; p->irq_generation=registry_get((unsigned)h)->generation;
    return 0;
}
static void release_route(struct com_port *p)
{
    if (p->irq_claim<0) return;
    registry_release(p->irq_claim,p->irq_generation); p->irq_claim=-1;
    if (p->resource.origin!=CIUKI_COM_PCI) irq_owned[p->resource.irq]=false;
}
static int attach_route(struct com_port *p, unsigned index)
{
    unsigned irq=p->resource.irq;
    if (p->irq_claim>=0 && registry_activate(p->irq_claim,p->irq_generation)) return -EBUSY;
    if (p->resource.origin!=CIUKI_COM_PCI) { com_legacy_attach(irq,0); pic_unmask(irq); return 0; }
    return irq_chain_add(irq,pci_irq,names[index]);
}
int com_admit(const struct com_resource *r, const struct com_io *io)
{
    if (!r || !io || !io->read || !io->write || !io->pause || !r->base || r->base==0xffff ||
        r->base<0x100 || r->stride!=1 || r->base>0xfff8 || !r->clock ||
        r->origin<CIUKI_COM_BDA || r->origin>CIUKI_COM_PCI) return -EINVAL;
    unsigned index=r->number ? r->number-1 : CIUKI_COM_MAX;
    if (r->number>CIUKI_COM_MAX) return -EINVAL;
    for (unsigned i=0;i<CIUKI_COM_MAX;i++) {
        if (ports[i].present && r->base<ports[i].resource.base+8 && ports[i].resource.base<r->base+8) {
            if (r->base!=ports[i].resource.base) return -EBUSY;
            if (r->origin==CIUKI_COM_PNP && r->irq!=ports[i].resource.irq &&
                (ports[i].status.flags&CIUKI_COM_READY)) {
                struct com_port *p=&ports[i];
                if (p->queues || p->closing || p->deferred) return -EBUSY;
                /* Assigned routing takes precedence over the BDA convention.
                 * This acquired UART is inactive; preserve its registers and
                 * logical number. The old aggregate IRQ owner stays pinned. */
                p->resource.irq=p->status.irq=r->irq;
                p->resource.origin=p->status.origin=CIUKI_COM_PNP;
                p->irq_claim=-1; p->irq_generation=0;
                if (!r->routed || (r->irq!=3 && r->irq!=4) || reserve_route(p,i) || attach_route(p,i))
                    quarantine(p,"Assigned PnP IRQ unavailable; resources retained.");
                return (int)i;
            }
            /* Assigned PnP may supply routing missing from a nonstandard
             * BDA base. Preserve the logical number, never reprobe a live,
             * reserved or quarantined port. */
            if (r->origin==CIUKI_COM_PNP && r->routed && !ports[i].status.flags && ports[i].claim<0) {
                struct com_resource assigned=*r; assigned.number=i+1;
                ports[i].present=false; return com_admit(&assigned,io);
            }
            return (int)i;
        }
        if (!r->number && !ports[i].present && index==CIUKI_COM_MAX) index=i;
    }
    if (index==CIUKI_COM_MAX || ports[index].present) return -ENOSPC;
    struct com_port *p=&ports[index]; p->present=true; p->resource=*r; p->io=*io; p->claim=p->irq_claim=-1;
    p->status=(struct ciuki_com_status){.version=1,.size=sizeof(p->status),.number=index+1,
        .base=r->base,.irq=r->irq,.origin=r->origin,.rx_capacity=CIUKI_COM_QUEUE,.tx_capacity=CIUKI_COM_QUEUE};
    kwait_init(&p->wait);
    if (r->base==g_boot.uart_base && serial_present()) {
        p->status.flags=CIUKI_COM_LOGGER; p->status.fifo_depth=serial_fifo_size();
        p->reason="Reserved for kernel log; generation probing prohibited."; return (int)index;
    }
    p->reason="IRQ route unqualified; no UART probe performed.";
    if (!r->routed || r->irq>=16 || (r->origin!=CIUKI_COM_PCI && r->irq!=3 && r->irq!=4)) return (int)index;
    int h=registry_claim(RES_PORT,r->base,r->base+8,names[index],false);
    if (h<0) { p->reason="Port claim conflict; no UART probe performed."; return (int)index; }
    p->claim=h; p->claim_generation=registry_get((unsigned)h)->generation;
    if (reserve_route(p,index)) {
        registry_release(h,p->claim_generation); p->claim=-1;
        p->reason="IRQ claim conflict; no UART probe performed."; return (int)index;
    }
    int e=probe(p);
    if (e) {
        release_route(p);
        registry_release(h,p->claim_generation); p->claim=-1;
        p->reason=e==-EBUSY ? "Inherited FIFO or interrupts active; configuration retained." : "UART register probe failed.";
        return (int)index;
    }
    if (attach_route(p,index) || registry_activate(h,p->claim_generation)) {
        if (p->irq_claim>=0) registry_quarantine(p->irq_claim,p->irq_generation);
        p->status.flags=CIUKI_COM_QUARANTINED;
        registry_quarantine(h,p->claim_generation);
        p->reason="IRQ dispatcher unavailable; resources retained."; return (int)index;
    }
    p->status.flags=CIUKI_COM_READY; p->reason="Qualified UART; exclusive native COM available.";
    return (int)index;
}
void com_bda_discover(const uint16_t bases[4])
{
    for (unsigned i=0;i<4;i++) {
        unsigned irq=bases[i]==0x3f8 || bases[i]==0x3e8 ? 4 : bases[i]==0x2f8 || bases[i]==0x2e8 ? 3 : 0;
        struct com_resource r={i+1,bases[i],irq,CIUKI_COM_BDA,1843200,1,irq!=0};
        (void)com_admit(&r,&real_io);
    }
}
int com_pnp_assigned(const char *id, uint32_t base, uint32_t length, unsigned irq)
{
    if (!id || (strncmp(id,"PNP0500",8) && strncmp(id,"PNP0501",8)) || length!=8) return -EINVAL;
    struct com_resource r={0,base,irq,CIUKI_COM_PNP,1843200,1,irq==3 || irq==4};
    return com_admit(&r,&real_io);
}
void com_pci_discover(const struct pci_func *f)
{
    /* Exact conventional layout from 8250_pci.c: Oxford OX16PCI954.
     * The SIIG subsystem has a different clock; reject it explicitly. */
    if (!f || f->class_code!=7) return;
    unsigned count=0;
    if (f->vendor==0x1415 && f->device==0x9501 && f->sub_vendor!=0x131f) count=4;
    if (f->vendor==0x1b36 && f->device>=2 && f->device<=4) count=1u<<(f->device-2);
    if (!count) return;
    uint32_t base=f->bar[0]&~3u;
    if (!(f->bar[0]&1) || !base || base>0x10000-count*8) return;
    for (unsigned i=0;i<count;i++) {
        struct com_resource r={0,base+i*8,f->irq_line,CIUKI_COM_PCI,1843200,1,
            !!(f->command&1) && pci_irq_routed(f)};
        (void)com_admit(&r,&real_io);
    }
}
void com_init(void)
{
    if (initialized) return;
    initialized=true;
    uint16_t bda[4]; memcpy(bda,P2V(0x400),sizeof(bda)); com_bda_discover(bda);
    if (serial_present() && g_boot.uart_base) {
        struct com_resource r={0,g_boot.uart_base,0,CIUKI_COM_BDA,1843200,1,false};
        r.irq=r.base==0x3f8 || r.base==0x3e8 ? 4 : 3;
        (void)com_admit(&r,&real_io);
        klog("[com] logger base=%04x irq=%u owner=uart native_open=reserved",r.base,r.irq);
    }
    for (unsigned i=0;i<pci_count();i++) com_pci_discover(pci_get(i));
    struct registry_stats stats; registry_snapshot(&stats);
    klog("[com] metadata_bytes=%u registry_live=%u registry_limit=64",(unsigned)sizeof(ports),stats.live);
}
bool com_snapshot(unsigned i, struct ciuki_com_status *out, const char **reason)
{
    if (i>=CIUKI_COM_MAX || !ports[i].present) return false;
    uint32_t f=irq_save(); *out=ports[i].status;
    const char *why=ports[i].reason;
    if ((out->flags&CIUKI_COM_READY) && !leases_valid(&ports[i])) {
        out->flags=(out->flags&~CIUKI_COM_READY)|CIUKI_COM_QUARANTINED;
        why="Resource lease invalidated; native COM unavailable.";
    }
    if (reason) *reason=why;
    irq_restore(f); return true;
}
static struct ciuki_com_line default_line(void)
{ return (struct ciuki_com_line){1,sizeof(struct ciuki_com_line),38400,8,1,CIUKI_COM_PARITY_NONE,0,0,0,0}; }
static bool program(struct com_port *p)
{
    unsigned divisor=p->resource.clock/(16*p->line.baud);
    p->lcr=(uint8_t)((p->lcr&0x40) | (p->line.data_bits-5) | (p->line.stop_bits==2 ? 4 : 0));
    if (p->line.parity) p->lcr|=(uint8_t)(8 | ((p->line.parity-1)<<4));
    if (p->has_efr) { wr(p,3,0xbf); wr(p,2,0); }
    wr(p,3,p->lcr); wr(p,1,0); wr(p,3,p->lcr|0x80);
    wr(p,0,(uint8_t)divisor); wr(p,1,(uint8_t)(divisor>>8));
    bool ok=rd(p,0)==(divisor&255) && rd(p,1)==(divisor>>8);
    p->fcr=p->status.fifo_depth>1 ? (uint8_t)(1 | p->line.rx_trigger<<6) : 0;
    if (p->status.chip==CIUKI_COM_16750 && p->status.fifo_depth==64) p->fcr|=0x20;
    wr(p,2,p->fcr|6); wr(p,3,p->lcr);
    if (p->status.fifo_depth==1) {
        /* An 8250/16450 has no FIFO reset command. Discard stale input
         * before the new generation enables RX; never carry an old byte. */
        for (unsigned n=0;n<64 && (rd(p,5)&1);n++) (void)rd(p,0);
        if (rd(p,5)&1) ok=false;
    }
    wr(p,4,p->mcr); wr(p,1,p->ier);
    if (!ok) quarantine(p,"UART divisor readback failed; resources retained.");
    return ok;
}
int com_open(unsigned i, uint32_t *generation)
{
    if (i>=CIUKI_COM_MAX || !generation || !ports[i].present) return -ENODEV;
    struct com_port *p=&ports[i];
    if (p->status.flags&(CIUKI_COM_LOGGER|CIUKI_COM_OPEN)) return -EBUSY;
    if (!(p->status.flags&CIUKI_COM_READY) || !leases_valid(p)) return -ENODEV;
    uint8_t *queues=kzalloc(2*CIUKI_COM_QUEUE); if (!queues) return -ENOMEM;
    uint32_t f=irq_save();
    if (p->queues || p->deferred || p->closing || !(p->status.flags&CIUKI_COM_READY) || !leases_valid(p) || open_generation==UINT32_MAX) {
        irq_restore(f); kfree(queues); return -EBUSY;
    }
    p->queues=queues; p->rx_head=p->tx_head=0; p->status.rx_queued=p->status.tx_queued=0;
    p->status.errors=p->status.received=p->status.transmitted=p->status.dropped=0;
    p->status.generation=++open_generation; p->status.flags|=CIUKI_COM_OPEN;
    p->line=default_line(); p->lcr=0; p->ier=13; p->mcr=0x0b;
    if (!program(p)) {
        p->queues=0; p->status.generation=0; p->status.flags&=~CIUKI_COM_OPEN;
        irq_restore(f); kfree(queues); return -EIO;
    }
    p->status.modem=rd(p,6); *generation=p->status.generation; irq_restore(f); return 0;
}
void com_close(unsigned i, uint32_t generation)
{
    if (i>=CIUKI_COM_MAX) return;
    struct com_port *p=&ports[i]; uint32_t f=irq_save();
    if (!p->queues || p->status.generation!=generation) { irq_restore(f); return; }
    wr(p,1,0); p->ier=0; wr(p,3,p->lcr&~0x40u); wr(p,4,0);
    uint8_t *queues=p->queues; p->queues=0; p->status.flags&=~CIUKI_COM_OPEN;
    p->closing=true;
    p->status.generation=0; p->status.rx_queued=p->status.tx_queued=0;
    /* FIFO reset discards holding bytes, not the shift register. Wait in
     * thread context for that last character, never with IRQs excluded. */
    wr(p,3,0x80); wr(p,2,p->fcr|6); wr(p,2,0); wr(p,3,p->lcr&~0x40u);
    uint8_t lsr=rd(p,5);
    bool idle=lsr!=0xff && (lsr&0x40);
    irq_restore(f);
    for (unsigned n=0;!idle && n<20 && (f&0x200);n++) {
        p->io.pause(p->io.context);
        uint32_t saved=irq_save(); lsr=rd(p,5); idle=lsr!=0xff && (lsr&0x40); irq_restore(saved);
    }
    f=irq_save();
    if (!idle) quarantine(p,"Close idle timeout; resources retained.");
    p->closing=false;
    kwait_wake_irq(&p->wait,false); irq_restore(f); kfree(queues);
}
int com_transfer(unsigned i, uint32_t g, void *data, unsigned bytes, bool write)
{
    if (i>=CIUKI_COM_MAX || (!data && bytes)) return -EINVAL;
    struct com_port *p=&ports[i]; uint8_t *b=data; uint32_t f=irq_save();
    if (!live(p,g)) { irq_restore(f); return -ENODEV; }
    unsigned n=write ? CIUKI_COM_QUEUE-p->status.tx_queued : p->status.rx_queued;
    if (n>bytes) n=bytes;
    for (unsigned k=0;k<n;k++) {
        if (write) { p->queues[CIUKI_COM_QUEUE+(p->tx_head+p->status.tx_queued)%CIUKI_COM_QUEUE]=b[k]; p->status.tx_queued++; }
        else { b[k]=p->queues[p->rx_head]; p->rx_head=(p->rx_head+1)%CIUKI_COM_QUEUE; p->status.rx_queued--; }
    }
    if (write && n && !(p->ier&2)) { p->ier|=2; if (!p->deferred) wr(p,1,p->ier); }
    irq_restore(f); return n || !bytes ? (int)n : -EAGAIN;
}
struct com_waiter { struct com_port *port; uint32_t generation; bool write; bool (*cancelled)(void); };
static bool wait_ready(void *arg)
{
    struct com_waiter *w=arg;
    return !live(w->port,w->generation) || (w->cancelled && w->cancelled()) ||
        (w->write ? w->port->status.tx_queued<CIUKI_COM_QUEUE : w->port->status.rx_queued!=0);
}
int com_wait(unsigned i, uint32_t g, bool write, unsigned timeout, bool (*cancelled)(void))
{
    if (i>=CIUKI_COM_MAX || timeout>60000) return -EINVAL;
    struct com_waiter w={&ports[i],g,write,cancelled};
    if (!timeout) return -EAGAIN;
    bool ready=kwait_wait_until(&w.port->wait,wait_ready,&w,deadline_after_ms(timeout));
    if (!live(w.port,g)) return -ENODEV;
    if (cancelled && cancelled()) return -EINTR;
    return ready ? 0 : -ETIMEDOUT;
}
int com_control(unsigned i, uint32_t g, uint32_t op, void *record, unsigned bytes, bool writable)
{
    unsigned size=op==COM_GET_LINE || op==COM_SET_LINE ? sizeof(struct ciuki_com_line) :
        op==COM_GET_STATUS ? sizeof(struct ciuki_com_status) : op==COM_SET_MODEM ? sizeof(struct ciuki_com_modem) : 0;
    if (!size) return -ENOTTY;
    if (!record || size!=bytes) return -EINVAL;
    uint32_t *header=record;
    if (header[0]!=1 || header[1]!=size || i>=CIUKI_COM_MAX) return -EINVAL;
    struct com_port *p=&ports[i]; uint32_t f=irq_save(); int e=0;
    if (!live(p,g)) e=-ENODEV;
    else if ((op==COM_SET_LINE || op==COM_SET_MODEM) && !writable) e=-EACCES;
    else if (op==COM_GET_STATUS) {
        if (((struct ciuki_com_status *)record)->reserved) e=-EINVAL;
        else *(struct ciuki_com_status *)record=p->status;
    } else if (op==COM_GET_LINE) {
        if (((struct ciuki_com_line *)record)->reserved) e=-EINVAL;
        else *(struct ciuki_com_line *)record=p->line;
    }
    else if (op==COM_SET_MODEM) {
        struct ciuki_com_modem *m=record;
        if (m->reserved || (m->outputs&~7u)) e=-EINVAL;
        else { p->mcr=(uint8_t)(8 | (m->outputs&3));
            p->lcr=(p->lcr&~0x40u) | ((m->outputs&CIUKI_COM_BREAK) ? 0x40 : 0);
            wr(p,4,p->mcr); wr(p,3,p->lcr); }
    } else {
        struct ciuki_com_line *l=record;
        uint64_t scale=(uint64_t)l->baud*16;
        if (!scale || scale>p->resource.clock || p->resource.clock%scale || p->resource.clock/scale>65535 ||
            l->data_bits<5 || l->data_bits>8 || l->stop_bits<1 || l->stop_bits>2 || l->parity>4 ||
            l->rx_trigger>3 || l->read_timeout_ms>60000 || l->write_timeout_ms>60000 || l->reserved) e=-EINVAL;
        else if (p->status.tx_queued || !(rd(p,5)&0x40) || p->status.rx_queued || (rd(p,5)&1)) e=-EBUSY;
        else { p->line=*l; if (!program(p)) e=-EIO; }
    }
    irq_restore(f); return e;
}
