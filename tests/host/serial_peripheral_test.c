/* Production codecs and UART registers; no guest/runtime qualification.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/serial_protocol.h>
#include <ciuki/uart16550.h>
static const uint8_t pnp[]={ '(',0,1,'M','S','H','0','0','0','1','\\','\\',
    'M','O','U','S','E','\\','P','N','P','0','F','0','C','\\','W','H','E','E','L','4','0',')' };
static void pnp_fixtures(void)
{
    struct serial_pnp_id id={0};
    assert(!serial_pnp_parse(pnp,sizeof(pnp),&id));
    assert(!strcmp(id.hid,"MSH0001") && !strcmp(id.class_name,"MOUSE") && serial_pnp_mouse(&id)==4);
    uint8_t low[sizeof(pnp)]; memcpy(low,pnp,sizeof(low));
    for (unsigned i=0;i<sizeof(low);i++) if (i!=1 && i!=2) low[i]-=32;
    low[sizeof(low)-3]='9'-32; low[sizeof(low)-2]='8'-32; /* wire checksum 98 */
    /* The literal alternate encoding has its own checksum, not ASCII 40. */
    unsigned sum=0; for (unsigned i=0;i<sizeof(low);i++) if (i!=sizeof(low)-3 && i!=sizeof(low)-2) sum+=low[i];
    assert((sum&255)==0x80); low[sizeof(low)-3]='8'-32; low[sizeof(low)-2]='0'-32;
    assert(!serial_pnp_parse(low,sizeof(low),&id) && serial_pnp_mouse(&id)==4);
    const uint8_t minimal[]={'M','(',0,1,'P','N','P','0','F','0','1',')'};
    assert(!serial_pnp_parse(minimal,sizeof(minimal),&id) && serial_pnp_mouse(&id)==1);
    uint8_t bad[257];
    for (unsigned i=0;i<sizeof(pnp);i++) {
        memcpy(bad,pnp,sizeof(pnp)); bad[i]^=0x80;
        assert(serial_pnp_parse(bad,sizeof(pnp),&id));
    }
    for (unsigned n=0;n<sizeof(pnp);n++) assert(serial_pnp_parse(pnp,n,&id));
    assert(serial_pnp_parse(pnp,257,&id));
    memcpy(bad,pnp,sizeof(pnp)); bad[sizeof(pnp)-3]='0'; assert(serial_pnp_parse(bad,sizeof(pnp),&id));
    memset(&id,0,sizeof(id)); strcpy(id.hid,"ZZZ0001"); strcpy(id.compatible,"ABC0001,PNP0F08");
    assert(serial_pnp_mouse(&id)==2); strcpy(id.class_name,"MODEM"); assert(!serial_pnp_mouse(&id));
    puts("PnP fixtures, encodings, checksum, delimiters and bounds: PASS");
}
struct enum_wire { uint64_t ms; unsigned modem, transitions[8], at[8], count, pos, phase;
    unsigned reply_phase, interval, drop_at, error_at, pause_jump; bool dsr, silent; };
static int modem(void *arg,unsigned v)
{ struct enum_wire *w=arg; w->modem=v; assert(w->count<8); w->transitions[w->count]=v; w->at[w->count++]=(unsigned)w->ms; if(v==3) w->phase++; return 0; }
static int setup(void *arg) { struct enum_wire *w=arg; assert(w->ms==200); return 0; }
static int dsr(void *arg) { struct enum_wire *w=arg; return w->dsr && (!w->drop_at || w->ms<w->drop_at); }
static int rx(void *arg,uint8_t *b)
{
    struct enum_wire *w=arg;
    if (w->silent || w->modem!=3 || w->phase!=(w->reply_phase ? w->reply_phase : 2) || w->pos==sizeof(pnp)) return -EAGAIN;
    if (w->ms<w->at[w->count-1]+w->pos*(w->interval ? w->interval : 1)) return -EAGAIN;
    if (w->error_at && w->pos==w->error_at) return -EIO;
    *b=pnp[w->pos++]; return 1;
}
static void pause_ms(void *arg) { struct enum_wire *w=arg; w->ms+=w->pause_jump ? w->pause_jump : 1; }
static uint64_t now(void *arg) { return ((struct enum_wire *)arg)->ms; }
static void enumeration(void)
{
    struct enum_wire w={.dsr=true}; struct serial_pnp_ops o={modem,setup,dsr,rx,pause_ms,now,&w};
    struct serial_pnp_id id; assert(!serial_pnp_enumerate(&o,&id));
    assert(w.count==7 && w.transitions[0]==1 && w.at[0]==0 && w.transitions[1]==0 && w.at[1]==200 &&
        w.transitions[2]==1 && w.at[2]==400 && w.transitions[3]==3 && w.at[3]==600 &&
        w.transitions[4]==0 && w.at[4]==800 && w.transitions[5]==1 && w.at[5]==1000 &&
        w.transitions[6]==3 && w.at[6]==1000);
    w=(struct enum_wire){0}; assert(serial_pnp_enumerate(&o,&id)==-ENODEV && w.ms==200 && w.count==1);
    w=(struct enum_wire){.dsr=true,.silent=true}; assert(serial_pnp_enumerate(&o,&id)==-ETIMEDOUT && w.ms==1200);
    w=(struct enum_wire){.dsr=true,.reply_phase=1}; assert(!serial_pnp_enumerate(&o,&id) && w.phase==1);
    w=(struct enum_wire){.dsr=true,.reply_phase=1,.drop_at=610};
    assert(serial_pnp_enumerate(&o,&id)==-ENODEV && w.ms==610);
    w=(struct enum_wire){.dsr=true,.reply_phase=1,.error_at=5}; assert(serial_pnp_enumerate(&o,&id)==-EIO);
    w=(struct enum_wire){.dsr=true,.reply_phase=1,.interval=300};
    assert(serial_pnp_enumerate(&o,&id)==-EINVAL && w.ms==800); /* per-character timeout */
    w=(struct enum_wire){.dsr=true,.reply_phase=1,.interval=100};
    assert(serial_pnp_enumerate(&o,&id)==-EINVAL && w.ms==2800); /* total 2.2-second bound */
    w=(struct enum_wire){.dsr=true,.pause_jump=250};
    assert(serial_pnp_enumerate(&o,&id)==-ETIMEDOUT && w.ms==250 && w.count==1); /* missed stage tolerance */
    puts("PnP DTR/RTS/DSR, second phase and bounded response timing: PASS");
}
static void mice(void)
{
    struct serial_mouse_decoder d={.protocol=1}; struct serial_mouse_packet p;
    assert(!serial_mouse_byte(&d,0,false,1,&p));
    assert(!serial_mouse_byte(&d,0x6f,false,2,&p));
    assert(!serial_mouse_byte(&d,0x3f,false,3,&p));
    assert(serial_mouse_byte(&d,0x3e,false,4,&p) && p.dx==-1 && p.dy==-2 && p.buttons==1);
    assert(!serial_mouse_byte(&d,0x40,false,5,&p));
    assert(!serial_mouse_byte(&d,0x40,false,6,&p)); /* resync at a fresh header */
    assert(!serial_mouse_byte(&d,1,false,7,&p));
    assert(serial_mouse_byte(&d,2,false,8,&p) && p.dx==1 && p.dy==2 && !p.buttons);
    d=(struct serial_mouse_decoder){.protocol=2};
    assert(!serial_mouse_byte(&d,0x40,false,1,&p)); assert(!serial_mouse_byte(&d,0,false,2,&p));
    assert(serial_mouse_byte(&d,0,false,3,&p));
    assert(serial_mouse_byte(&d,0x20,false,4,&p) && p.buttons==4);
    assert(!serial_mouse_byte(&d,0x40,false,5,&p)); assert(!serial_mouse_byte(&d,0,false,6,&p));
    assert(serial_mouse_byte(&d,0,false,7,&p) && p.buttons==4);
    assert(serial_mouse_byte(&d,0,false,8,&p) && !p.buttons);
    d=(struct serial_mouse_decoder){.protocol=4};
    assert(!serial_mouse_byte(&d,0x40,false,1,&p)); assert(!serial_mouse_byte(&d,0,false,2,&p));
    assert(!serial_mouse_byte(&d,0,false,3,&p));
    assert(serial_mouse_byte(&d,0x1f,false,4,&p) && p.wheel==1 && p.buttons==4);
    d=(struct serial_mouse_decoder){.protocol=3};
    uint8_t packet[]={0x80,0x80,0x7f,0x81,0x80};
    for (unsigned i=0;i<4;i++) assert(!serial_mouse_byte(&d,packet[i],false,i,&p));
    assert(serial_mouse_byte(&d,packet[4],false,4,&p) && p.dx==-255 && p.dy==1 && p.buttons==7);
    d=(struct serial_mouse_decoder){.protocol=1};
    assert(!serial_mouse_byte(&d,0x40,false,1,&p)); assert(!serial_mouse_byte(&d,1,true,2,&p));
    assert(!serial_mouse_byte(&d,1,false,3,&p));
    assert(!serial_mouse_byte(&d,0x40,false,4,&p)); assert(!serial_mouse_byte(&d,1,false,200,&p));
    puts("Microsoft, Logitech middle, wheel and five-byte Mouse Systems: PASS");
}
static unsigned sent;
static int sink(void *unused,uint8_t b) { (void)unused; assert(b==0x55); sent++; return 0; }
static void uart(void)
{
    struct uart16550 a,b; uart16550_init(&a); uart16550_init(&b);
    assert(uart16550_read(&a,5,0)==0x60 && uart16550_read(&a,2,0)==1);
    uart16550_write(&a,3,0x83,0); uart16550_write(&a,0,12,0); uart16550_write(&a,1,0,0);
    assert(uart16550_read(&a,0,0)==12 && uart16550_read(&a,1,0)==0);
    uart16550_write(&a,3,3,0); assert(uart16550_character_us(&a)==1042);
    uart16550_write(&a,2,0x41,0); uart16550_write(&a,1,15,0); uart16550_write(&a,4,8,0);
    assert(uart16550_irq(&a) && uart16550_read(&a,2,0)==0xc2 && !uart16550_irq(&a));
    for (unsigned i=0;i<3;i++) uart16550_receive(&a,(uint8_t)i,0,0);
    assert(!uart16550_irq(&a)); uart16550_advance(&a,4167,0,0,true); assert(!uart16550_irq(&a));
    uart16550_advance(&a,4168,0,0,true); assert(uart16550_read(&a,2,4168)==0xcc);
    uart16550_receive(&a,3,4,5000); assert(uart16550_read(&a,2,5000)==0xc4);
    for (unsigned i=4;i<17;i++) uart16550_receive(&a,(uint8_t)i,0,5000);
    assert(a.rx_count==16 && uart16550_read(&a,2,5000)==0xc6);
    assert((uart16550_read(&a,5,5000)&0x82)==0x82);
    assert(uart16550_read(&a,0,5000)==0 && uart16550_read(&a,0,5000)==1 && uart16550_read(&a,0,5000)==2);
    assert(uart16550_read(&a,2,5000)==0xc6); assert(uart16550_read(&a,5,5000)&4);
    uart16550_write(&a,2,7,0); assert(!a.rx_count && !a.tx_count);
    uart16550_modem(&a,0xf0); assert(uart16550_read(&a,6,0)==0xfb);
    uart16550_modem(&a,0); assert(uart16550_read(&a,6,0)==15 && uart16550_read(&a,6,0)==0);
    uart16550_write(&a,0,0x55,0); assert(!(uart16550_read(&a,5,0)&0x60));
    uart16550_advance(&a,0,sink,0,true); assert(sent==1 && (uart16550_read(&a,5,0)&0x60)==0x20);
    uart16550_advance(&a,1042,sink,0,false); assert(!(uart16550_read(&a,5,1042)&0x40));
    uart16550_advance(&a,1042,sink,0,true); assert(uart16550_read(&a,5,1042)&0x40);
    uart16550_write(&a,4,0x1b,2000); assert((uart16550_read(&a,6,2000)&0xf0)==0xb0);
    uart16550_write(&a,0,0x55,2000); uart16550_advance(&a,2000,sink,0,true);
    uart16550_advance(&a,3042,sink,0,true); assert(sent==1 && uart16550_read(&a,0,3042)==0x55);
    uart16550_write(&a,7,0xa5,0); assert(uart16550_read(&a,7,0)==0xa5 && uart16550_read(&b,7,0)==0);
    assert(!b.rx_count && !b.tx_count && !b.ier && !b.msr);
    const unsigned thresholds[]={1,4,8,14};
    for (unsigned t=0;t<4;t++) {
        uart16550_init(&a); uart16550_write(&a,2,(uint8_t)(1|(t<<6)),0);
        uart16550_write(&a,4,8,0); uart16550_write(&a,1,1,0);
        for (unsigned n=1;n<=thresholds[t];n++) {
            uart16550_receive(&a,1,0,0); assert(uart16550_irq(&a)==(n==thresholds[t]));
        }
        uart16550_read(&a,0,0); assert(!uart16550_irq(&a));
    }
    uart16550_init(&a); uart16550_write(&a,2,1,0);
    for (unsigned n=0;n<20;n++) uart16550_write(&a,0,(uint8_t)n,0);
    assert(a.tx_count==16); uart16550_write(&a,2,0,0); assert(!a.tx_count);
    uart16550_write(&a,0,1,0); uart16550_write(&a,0,2,0); assert(a.tx_count==1);
    uart16550_init(&a); uart16550_write(&a,3,0x43,0); uart16550_write(&a,4,0x18,0); uart16550_write(&a,1,4,0);
    uart16550_write(&a,0,0x55,0); uart16550_advance(&a,0,0,0,true); uart16550_advance(&a,87,0,0,true);
    assert(uart16550_read(&a,2,87)==6 && (uart16550_read(&a,5,87)&0x18)==0x18);
    assert(uart16550_read(&a,0,87)==0 && !(uart16550_read(&a,5,87)&0x1c));
    puts("16550A DLAB/RBR/THR/FIFO/IRQ/LSR/MSR/timeout/loopback/isolation: PASS");
}
int main(void) { setvbuf(stdout,0,_IONBF,0); pnp_fixtures(); enumeration(); mice(); uart(); return 0; }
