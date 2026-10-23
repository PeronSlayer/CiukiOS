/* TI TL16C550D SLLS597E, sections "Principles of Operation" and registers.
 * Original bounded model; no physical port accesses or foreign emulator code.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/uart16550.h>
#include <ciuki/abi.h>
#include <ciuki/kernel.h>
static unsigned capacity(const struct uart16550 *u) { return u->fcr&1 ? 16 : 1; }
static unsigned trigger(const struct uart16550 *u)
{ static const unsigned t[]={1,4,8,14}; return u->fcr&1 ? t[u->fcr>>6] : 1; }
uint32_t uart16550_character_us(const struct uart16550 *u)
{
    unsigned divisor=u->dll|((unsigned)u->dlm<<8);
    if (!divisor) divisor=65536;
    unsigned half_bits=2+2*(5+(u->lcr&3))+((u->lcr&8)?2:0)+
        ((u->lcr&4) ? ((u->lcr&3)?4:3) : 2);
    return (uint32_t)(((uint64_t)divisor*half_bits*1000000+230399)/230400);
}
void uart16550_init(struct uart16550 *u)
{ memset(u,0,sizeof(*u)); u->dll=1; u->thre=true; }
static uint8_t lsr(const struct uart16550 *u)
{
    uint8_t result=u->overrun | (u->rx_count ? 1|u->errors[u->rx_head] : 0);
    if (!u->tx_count) result|=0x20;
    if (!u->tx_count && !u->shifting) result|=0x40;
    if (u->fcr&1) for (unsigned i=0;i<u->rx_count;i++)
        if (u->errors[(u->rx_head+i)%16]) { result|=0x80; break; }
    return result;
}
static uint8_t cause(const struct uart16550 *u)
{
    if ((u->ier&4) && (lsr(u)&0x1e)) return 6;
    if ((u->ier&1) && u->rx_count>=trigger(u)) return 4;
    if ((u->ier&1) && u->timeout && u->rx_count) return 12;
    if ((u->ier&2) && u->thre) return 2;
    if ((u->ier&8) && (u->msr&15)) return 0;
    return 1;
}
bool uart16550_irq(const struct uart16550 *u) { return (u->mcr&8) && cause(u)!=1; }
static void modem_update(struct uart16550 *u, uint8_t high)
{
    uint8_t old=u->msr&0xf0, changed=old^high;
    u->msr=(u->msr&15)|high|(changed&0x10 ? 1 : 0)|(changed&0x20 ? 2 : 0)|
        ((old&0x40) && !(high&0x40) ? 4 : 0)|(changed&0x80 ? 8 : 0);
}
static void modem_refresh(struct uart16550 *u)
{
    uint8_t high=u->external;
    if (u->mcr&0x10) high=(u->mcr&2 ? 0x10 : 0)|(u->mcr&1 ? 0x20 : 0)|
        (u->mcr&4 ? 0x40 : 0)|(u->mcr&8 ? 0x80 : 0);
    modem_update(u,high);
}
void uart16550_modem(struct uart16550 *u, uint8_t inputs)
{ u->external=inputs&0xf0; modem_refresh(u); }
void uart16550_receive(struct uart16550 *u, uint8_t b, uint8_t errors, uint64_t us)
{
    if (u->rx_count>=capacity(u)) { u->overrun|=2; return; }
    unsigned tail=(u->rx_head+u->rx_count)%16;
    u->rx[tail]=b&((1u<<(5+(u->lcr&3)))-1); u->errors[tail]=errors&0x1c;
    u->rx_count++; u->rx_time=us; u->timeout=false;
}
uint8_t uart16550_read(struct uart16550 *u, unsigned r, uint64_t us)
{
    if (r==0) {
        if (u->lcr&0x80) return u->dll;
        if (!u->rx_count) return 0;
        uint8_t b=u->rx[u->rx_head]; u->errors[u->rx_head]=0;
        u->rx_head=(u->rx_head+1)%16; u->rx_count--; u->rx_time=us; u->timeout=false;
        return b;
    }
    if (r==1) return u->lcr&0x80 ? u->dlm : u->ier;
    if (r==2) { uint8_t c=cause(u); if (c==2) u->thre=false; return c|(u->fcr&1 ? 0xc0 : 0); }
    if (r==3) return u->lcr;
    if (r==4) return u->mcr;
    if (r==5) { uint8_t s=lsr(u); u->overrun=0;
        if (u->rx_count) u->errors[u->rx_head]=0;
        return s; }
    if (r==6) { uint8_t s=u->msr; u->msr&=0xf0; return s; }
    return r==7 ? u->scr : 0xff;
}
void uart16550_write(struct uart16550 *u, unsigned r, uint8_t b, uint64_t us)
{
    if (r==0) {
        if (u->lcr&0x80) { u->dll=b; return; }
        if (u->tx_count==capacity(u)) return; /* full THR/FIFO discards a write */
        u->tx[(u->tx_head+u->tx_count)%16]=b&((1u<<(5+(u->lcr&3)))-1);
        u->tx_count++; u->thre=false; return;
    }
    if (r==1) {
        if (u->lcr&0x80) u->dlm=b;
        else { if (!(u->ier&2) && (b&2) && !u->tx_count) u->thre=true; u->ier=b&15; }
    } else if (r==2) {
        if ((b^u->fcr)&1) b|=6;
        if (b&2) { u->rx_head=u->rx_count=0; u->overrun=0; memset(u->errors,0,sizeof(u->errors)); u->timeout=false; }
        if (b&4) { u->tx_head=u->tx_count=0; u->thre=true; }
        u->fcr=b&0xc9;
    } else if (r==3) u->lcr=b;
    else if (r==4) { u->mcr=b&0x1f; modem_refresh(u); }
    else if (r==7) u->scr=b;
    (void)us;
}
void uart16550_advance(struct uart16550 *u, uint64_t us,
                      int (*send)(void *, uint8_t), void *ctx, bool ready)
{
    u->timeout=(u->fcr&1) && u->rx_count && us-u->rx_time>=4ull*uart16550_character_us(u);
    if (u->shifting && us>=u->tx_due) {
        if (u->mcr&0x10) {
            uart16550_receive(u,u->lcr&0x40 ? 0 : u->shift,u->lcr&0x40 ? 0x18 : 0,us); u->shifting=false;
        } else if (ready) u->shifting=false;
    }
    if (!u->shifting && u->tx_count) {
        uint8_t b=u->tx[u->tx_head]; int e=u->mcr&0x10 ? 0 : send ? send(ctx,b) : 0;
        if (e==-EAGAIN) return;
        if (e) { u->overrun|=8; return; }
        u->shift=b; u->shifting=true; u->tx_due=us+uart16550_character_us(u);
        u->tx_head=(u->tx_head+1)%16; u->tx_count--; if (!u->tx_count) u->thre=true;
    }
}
