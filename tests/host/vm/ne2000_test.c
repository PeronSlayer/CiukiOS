/* SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
#include "../../../src/kernel/vm/dosvm_ne2000.h"
#include <ciuki/v86.h>
static struct dosvm_ne2000 d;
static unsigned sent,edges;
static bool level;
static int send_error;
static uint8_t copied[1514];
static int send(void *arg,const uint8_t *p,unsigned n)
{ (void)arg; sent++; memcpy(copied,p,n); return send_error; }
static void irq(void *arg,bool n) { (void)arg; level=n; if(n) edges++; }
static void w(unsigned r,uint8_t n) { dosvm_ne2000_write(&d,0x300+r,1,n); }
static unsigned r(unsigned n) { return dosvm_ne2000_read(&d,0x300+n,1); }
static void remote(unsigned at,unsigned bytes,uint8_t command)
{ w(0,0x22); w(8,at); w(9,at>>8); w(10,bytes); w(11,bytes>>8); w(0,command); }
static void ring(void)
{ w(0,0x21); w(1,0x46); w(2,0x80); w(3,0x46); w(12,4); w(14,1); w(0,0x61); w(7,0x47); w(0,0x22); w(7,0x7f); w(15,0x7f); }
int main(void)
{
    const uint8_t mac[6]={2,1,2,3,4,5}; struct dosvm_ne2000_ops ops={0,send,irq};
    dosvm_ne2000_init(&d,mac,&ops); CHECK(r(0)==0x21 && r(7)==0x80 && !level);
    w(14,1); remote(0,32,0x0a);
    for(unsigned i=0;i<16;i++) { unsigned v=dosvm_ne2000_read(&d,0x310,2); CHECK((v&255)==(v>>8)); if(i<6) CHECK((v&255)==mac[i]); if(i>=14) CHECK(v==0x5757); }
    CHECK(d.count==0 && (r(7)&0x40)); w(7,0x7f);
    remote(0x4000,3,0x12); dosvm_ne2000_write(&d,0x310,2,0x1234); dosvm_ne2000_write(&d,0x310,2,0x5678);
    CHECK(d.count==0 && d.ram[0]==0x34 && d.ram[3]==0x56);
    remote(0x4000,4,0x0a); CHECK(dosvm_ne2000_read(&d,0x310,2)==0x1234); CHECK(dosvm_ne2000_read(&d,0x310,2)==0x5678);
    remote(0xffff,2,0x12); dosvm_ne2000_write(&d,0x310,2,0); CHECK(d.bad_dma==1 && !d.count);
    remote(0x7fff,4,0x12); dosvm_ne2000_write(&d,0x310,2,0xabcd); dosvm_ne2000_write(&d,0x310,2,0); CHECK(d.bad_dma==2);
    ring(); uint8_t p[1514]; ipx_frame(p,mac,mac,64);
    w(15,0); CHECK(dosvm_ne2000_receive(&d,p,64)==1 && !level); CHECK(d.current==0x48);
    unsigned at=0x700; CHECK(d.ram[at]==1 && d.ram[at+1]==0x48 && d.ram[at+2]==68 && !memcmp(d.ram+at+4,p,64));
    w(15,1); CHECK(level); w(7,1); CHECK(!level);
    w(0,0xa2); CHECK(r(1)==0x46 && r(2)==0x80 && r(12)==4 && r(14)==1); w(0,0x22);
    /* Ring wraps after complete header+data+CRC, never writes outside RAM. */
    w(0,0x62); w(7,0x7f); w(0,0x22); w(3,0x7e);
    ipx_frame(p,mac,mac,300); CHECK(dosvm_ne2000_receive(&d,p,300)==1 && d.current==0x47);
    CHECK(!memcmp(d.ram+0x3f04,p,252) && !memcmp(d.ram+0x600,p+252,48));
    remote(0x7ffe,4,0x12); dosvm_ne2000_write(&d,0x310,2,0xbeef); dosvm_ne2000_write(&d,0x310,2,0xcafe);
    CHECK(d.ram[0x3ffe]==0xef && d.ram[0x600]==0xfe && d.remote==0x4602);
    w(0,0x62); w(7,0x47); w(0,0x22); w(3,0x48);
    CHECK(dosvm_ne2000_receive(&d,p,300)==-ENOSPC && (d.isr&0x10));
    w(1,0); CHECK(dosvm_ne2000_receive(&d,p,64)==-ENOSPC); ring();
    memcpy(d.ram,p,64); w(4,0x40); w(5,64); w(6,0); w(0,0x26);
    CHECK(sent==1 && !memcmp(copied,p,64) && d.tsr==1 && !(d.command&4));
    send_error=-11; w(0,0x26); CHECK(sent==2 && d.tsr==8 && (d.isr&8));
    w(4,0x7f); w(5,255); w(6,255); w(0,0x26); CHECK(sent==2 && d.tsr==8);
    uint32_t generation=d.generation; CHECK(dosvm_ne2000_read(&d,0x31f,1)==0);
    CHECK(d.generation!=generation && d.isr==0x80 && !level && !d.imr);
    CHECK(dosvm_ne2000_read(&d,0x2ff,1)==UINT32_MAX); CHECK(dosvm_ne2000_read(&d,0x310,4)==UINT32_MAX);
    ring(); w(7,255); CHECK(!(d.isr&0x7f));
    w(0,0xe2); CHECK(r(15)==0); w(15,255); CHECK(r(15)==0);
    printf("ne2000: %u checks passed (IRQ edges %u)\n",checks,edges); return 0;
}
