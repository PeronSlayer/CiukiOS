/* Production resource glue with fake privileged I/O and mapping boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/kernel/drivers/video/s3legacy/private.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
/* Do not execute privileged CPU inline helpers in the host fixture. */
#define CIUKI_CPU_H
static uint8_t inb(uint16_t);
static void outb(uint16_t,uint8_t);
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t v) { (void)v; }
#include "../../src/kernel/drivers/video/s3legacy/platform.c"

struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
static struct resource resources[8];
static unsigned count,claims,activates,quiesces,releases,quarantines,maps,revokes;
static unsigned fail_claim,fail_map,fail_activate;
static uint8_t cr[256],index_reg,misc=1;
static unsigned irq_depth;
static _Alignas(4096) uint8_t regs[65536],cursor[4096];
static struct s3_device s;
static uint8_t inb(uint16_t p)
{ CHECK(p==0x3cc || p==0x3d4 || p==0x3d5); return p==0x3cc ? misc : p==0x3d4 ? index_reg : cr[index_reg]; }
static void outb(uint16_t p,uint8_t v)
{ CHECK(p==0x3d4 || p==0x3d5); if (p==0x3d4) index_reg=v; else cr[index_reg]=v; }
uint64_t ktime_cycles(void) { return 12345; }
int udelay(uint32_t us) { CHECK(us==5); return 0; }
void framebuffer_write_drain(void) { }
int registry_claim(enum res_type type,uint32_t base,uint32_t end,const char *owner,bool share)
{
    claims++; CHECK(!share && !strcmp(owner,"s3test"));
    if (claims==fail_claim) return -EBUSY;
    unsigned h=count++;
    resources[h]=(struct resource){type,base,end,owner,h+1,RS_CLAIMED,false};
    return (int)h;
}
const struct resource *registry_get(unsigned h) { CHECK(h<count); return &resources[h]; }
int registry_activate(int h,gen_t g)
{ CHECK(resources[h].generation==g); activates++; if (activates==fail_activate) return -EIO; resources[h].state=RS_ACTIVE; return 0; }
int registry_quiesce(int h,gen_t g,registry_idle_proof_fn proof)
{
    CHECK(resources[h].state==RS_ACTIVE && resources[h].generation==g);
    quiesces++;
    if (!proof(h,g)) return -EIO;
    resources[h].state=RS_CLAIMED; return 0;
}
int registry_release(int h,uint32_t g)
{ CHECK(resources[h].generation==g && resources[h].state==RS_CLAIMED); releases++; resources[h].state=RS_RELEASED; return 0; }
int registry_quarantine(int h,gen_t g)
{ CHECK(resources[h].generation==g); quarantines++; resources[h].state=RS_QUARANTINED; return 0; }
void *vmm_reserve_mmio(uint32_t capacity)
{ CHECK(capacity==65536 || capacity==4096); return capacity==65536 ? regs : cursor; }
void *vmm_replace_mmio(void *window,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    maps++;
    CHECK(bytes==capacity && (window==regs || window==cursor));
    CHECK(phys==(window==regs ? 0xe1000000u : 0xe03ff000u));
    return maps==fail_map ? 0 : window;
}
void vmm_revoke_mmio(void *p,uint32_t bytes)
{ CHECK(s.cpu_safe && s.cursor_hidden && !s.active && ((p==regs && bytes==65536) || (p==cursor && bytes==4096))); revokes++; }
static void setup(void)
{
    memset(&g_boot,0,sizeof(g_boot)); memset(resources,0,sizeof(resources));
    memset(cr,0,sizeof(cr)); count=claims=activates=quiesces=releases=quarantines=maps=revokes=0;
    fail_claim=fail_map=fail_activate=irq_depth=0; misc=1; index_reg=0x77;
    g_boot.display.vram_bytes=4*1024*1024;
    s=(struct s3_device){.id="s3test",.cpu_safe=true,.cursor_hidden=true,
        .reg_claim=-1,.port_claim=-1,.cursor_claim=-1,
        .pci={.command=3},.fb={.phys=0xe0000000,.size=640*480*2}};
}
int main(void)
{
    setup(); CHECK(!s3_access_begin(&s));
    CHECK(claims==3 && activates==3 && maps==2 && s.registers==regs && s.cursor==cursor);
    CHECK(resources[0].type==RES_PORT && resources[0].start==0x3c0 && resources[0].end==0x3e0);
    CHECK(resources[1].type==RES_MMIO && resources[1].start==0xe1000000 && resources[1].end==0xe1010000);
    CHECK(resources[2].start==0xe03ff000 && s.cursor_offset==0x3ff000);
    cr[0x38]=0x12; cr[0x39]=0x34;
    s.io.cr_write(s.io.ctx,0x45,0x55);
    CHECK(index_reg==0x77 && cr[0x38]==0x12 && cr[0x39]==0x34 && cr[0x45]==0x55);
    CHECK(s.io.cr_read(s.io.ctx,0x45)==0x55 && index_reg==0x77);
    s.io.write(s.io.ctx,0x82e8,0x1234,16); CHECK(s.io.read(s.io.ctx,0x82e8,16)==0x1234);
    s.io.write(s.io.ctx,0xa500,0x12345678,32); CHECK(s.io.read(s.io.ctx,0xa500,32)==0x12345678);
    s.cpu_safe=false; CHECK(s3_access_end(&s)==-EIO && !releases && !revokes);
    s.cpu_safe=true; s.mmio_changed=true; s.saved_cr53=0x10; cr[0x53]=0x18;
    CHECK(!s3_access_end(&s) && releases==3 && revokes==2 && !s.registers && !s.cursor && !s.io.read);
    CHECK(cr[0x53]==0x10 && cr[0x38]==0x12 && cr[0x39]==0x34);
    setup(); fail_claim=2; CHECK(s3_access_begin(&s)==-EBUSY && releases==1 && !maps);
    setup(); fail_claim=1; CHECK(s3_access_begin(&s)==-EBUSY && !releases && !maps);
    setup(); fail_activate=2; CHECK(s3_access_begin(&s)==-EBUSY && quarantines==1 && releases==1);
    setup(); fail_map=1; CHECK(s3_access_begin(&s)==-ENOMEM && releases==2 && !s.io.read);
    setup(); fail_map=2; CHECK(!s3_access_begin(&s) && !s.cursor); CHECK(!s3_access_end(&s) && releases==3);
    setup(); s.pci.command=2; CHECK(s3_access_begin(&s)==-ENODEV && !claims);
    setup(); misc=0; CHECK(s3_access_begin(&s)==-EOPNOTSUPP && releases==1 && !maps);
    setup(); g_boot.display.vram_bytes=0; CHECK(s3_access_begin(&s)==-ENODEV && !claims);
    setup(); s.fb.phys=0xff800000; CHECK(s3_access_begin(&s)==-ENODEV && !claims);
    setup(); g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.type=CBI_E820_RAM,.base=0xe1000000,.length=65536};
    CHECK(s3_access_begin(&s)==-ENODEV && !claims);
    setup(); s.fb.size=4*1024*1024; CHECK(!s3_access_begin(&s) && claims==2 && !s.cursor);
    CHECK(!s3_access_end(&s) && releases==2);
    puts("PASS f2-118 production resource glue: extents, claims, ports, mappings, failure cleanup, unsafe retention");
    return 0;
}
