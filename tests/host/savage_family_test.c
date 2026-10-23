/* Additional family evidence using the unchanged legacy register renderer.
 * Only the early status encoding and COB enable are adapted at the fake bus.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "legacy_savage_fixture.inc"
#define s3savage_driver family_driver
#include "../../src/kernel/drivers/video/s3savage/s3savage_register.c"
#undef s3savage_driver

/* Mode setting is outside this fake bus. Test the actual adapter's selection,
 * without executing or replacing the production full-screen provider. */
bool supersavage_display_supported(const struct fb_device *d,uint16_t mode)
{ (void)d; (void)mode; return false; }
int supersavage_display_prepare(const struct fb_device *d,uint16_t mode)
{ (void)d; (void)mode; return -EOPNOTSUPP; }
int supersavage_display_begin(uint16_t mode) { (void)mode; return -EOPNOTSUPP; }
int supersavage_display_write(uint32_t off,const void *p,uint32_t n)
{ (void)off; (void)p; (void)n; return -EOPNOTSUPP; }
int supersavage_display_end(void) { return -EOPNOTSUPP; }
void supersavage_display_complete(bool ok) { (void)ok; }

static bool early_bus, wrong_status, corrupt_cob;
static unsigned early_reads, later_reads, cob_programs;
static struct resource family_leases[8];
static unsigned family_claimed, family_maps;
static bool fail_cob_claim, fail_cob_activate, fail_cursor_claim;
static bool family_marker_path(const char *path)
{
    char expected[80];
    ksnprintf(expected,sizeof(expected),"C:/system/settings/accel-s3savage-5333-%04x.cfg",binding_gpu.device);
    return !strcmp(path,expected);
}
static int family_claim(enum res_type type,uint32_t start,uint32_t end,const char *owner,bool share)
{
    CHECK(!share && start<end && family_claimed+1<ARRAY_SIZE(family_leases));
    if (!strcmp(owner,"savage-cob") && fail_cob_claim) return -EBUSY;
    if (!strcmp(owner,"savage-cursor") && fail_cursor_claim) return -EBUSY;
    for (unsigned i=1;i<=family_claimed;i++) {
        const struct resource *r=&family_leases[i];
        CHECK(r->state==RS_RELEASED || type!=r->type || start>=r->end || end<=r->start);
    }
    unsigned h=++family_claimed;
    family_leases[h]=(struct resource){type,start,end,owner,100+h,RS_CLAIMED,false};
    return (int)h;
}
static const struct resource *family_get(unsigned h)
{ CHECK(h<=family_claimed); return h ? &family_leases[h] : &reservation; }
static int family_activate(int h,gen_t generation)
{
    struct resource *r=&family_leases[h];
    CHECK(r->generation==generation && r->state==RS_CLAIMED);
    if (!strcmp(r->owner,"savage-cob") && fail_cob_activate) return -EIO;
    r->state=RS_ACTIVE; return 0;
}
static int family_quarantine(int h,gen_t generation)
{ CHECK(family_leases[h].generation==generation); family_leases[h].state=RS_QUARANTINED; return 0; }
static int family_quiesce(int h,gen_t generation,registry_idle_proof_fn proof)
{
    struct resource *r=&family_leases[h];
    CHECK(r->generation==generation && r->state==RS_ACTIVE);
    r->state=RS_QUIESCING; return proof(h,generation) ? 0 : -EIO;
}
static int family_release(int h,gen_t generation)
{ CHECK(family_leases[h].generation==generation && family_leases[h].state==RS_QUIESCING); family_leases[h].state=RS_RELEASED; return 0; }
static int family_takeover(int h,gen_t generation,const char *successor,registry_idle_proof_fn proof)
{
    (void)successor;
    CHECK(family_leases[h].generation==generation);
    if (!proof(h,generation)) return -EIO;
    family_leases[h].state=RS_RELEASED; return 0;
}
static void *family_reserve(uint32_t bytes)
{ CHECK(bytes==SAVAGE_MMIO_BYTES || bytes==PAGE_SIZE); return (void *)(uintptr_t)0x1000; }
static void *family_map(void *window,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    CHECK(window && capacity==bytes); family_maps++;
    if (bytes==SAVAGE_MMIO_BYTES) {
        CHECK(phys==(binding_gpu.bar[0]&~15u)+(early_bus ? SAVAGE_EARLY_MMIO : 0));
        return binding_window;
    }
    uint32_t cob=0;
    if (early_bus) CHECK(savage2d_cob_span(&device,g_boot.display.vram_bytes,&cob));
    CHECK(bytes==PAGE_SIZE && phys==device.phys+(cob ? cob : g_boot.display.vram_bytes)-PAGE_SIZE);
    return m.cursor_page; /* Never map the COB or engine-only rows. */
}
static uint32_t family_read(void *ctx,uint32_t reg)
{
    if (early_bus && reg==SAVAGE_EARLY_STATUS) {
        early_reads++;
        uint32_t value=model_read(ctx,SAVAGE_STATUS);
        if (wrong_status) return 0x00e00000u; /* later idle is NOT early idle */
        return value==UINT32_MAX || !value ? value : 0x80000u|(value&0xffffu);
    }
    if (reg==SAVAGE_STATUS) { CHECK(!early_bus); later_reads++; }
    uint32_t value=model_read(ctx,reg);
    if (corrupt_cob && reg==0x48c14 && cob_programs) value^=1u;
    return value;
}
static void family_write(void *ctx,uint32_t reg,uint32_t value)
{
    struct model *p=ctx;
    if (reg==0x48c14) cob_programs++;
    if (early_bus && reg>=SAVAGE_BCI && reg<SAVAGE_BCI+BCI_FIFO_WORDS*4) {
        CHECK((p->reg[BCI_CONTROL/4]&15u)==12u);
        CHECK((p->reg[0x48c14/4]&0xe0000fffu)==0xe0000fc0u);
        CHECK(p->reg[0x48c10/4]==0x78207220u && !p->reg[0x48c0c/4]);
        /* Existing packet interpreter asserts the later no-COB enable.
         * Validate the early enable first, translate only that fixture input. */
        uint32_t control=p->reg[BCI_CONTROL/4];
        p->reg[BCI_CONTROL/4]=(control&~15u)|8u;
        model_write(ctx,reg,value);
        p->reg[BCI_CONTROL/4]=control;
    } else model_write(ctx,reg,value);
}
static void family_fault(void *ctx)
{
    model_fault(ctx);
    if (family_resources) hw_fault(ctx);
}
static struct savage_io family_io(void)
{
    struct savage_io io=model_io();
    io.read32=family_read; io.write32=family_write; io.fault=family_fault;
    return io;
}
static uint32_t family_fixture(uint16_t id,unsigned bpp)
{
    boot_fixture_mode(false,bpp);
    g_boot.display.tag=CBI_DISPLAY_TAG;
    binding_gpu.device=id;
    early_bus=early_chip(savage2d_chip(id)); wrong_status=corrupt_cob=false;
    early_reads=later_reads=cob_programs=0;
    test_engine=(struct savage2d){0};
    if (early_bus) { binding_gpu.bar[0]=device.phys; binding_gpu.bar[1]=0; }
    uint32_t cob=0;
    if (early_bus) CHECK(savage2d_cob_span(&device,g_boot.display.vram_bytes,&cob));
    return cob;
}
static void family_commands(uint16_t id,unsigned bpp)
{
    uint32_t cob=family_fixture(id,bpp);
    struct savage_io io=family_io();
    enum savage_chip chip=savage2d_chip(id);
    const struct display_driver *table[]={&family_driver};
    const struct pci_func *found=NULL;
    unsigned ports=m.port_reads+m.port_writes, writes=m.engine_writes;
    CHECK(display_driver_select(&device,table,1,&found)==&family_driver && found==&binding_gpu);
    CHECK(!strcmp(family_driver.name,"S3 Savage") && !strcmp(family_driver.id,"s3savage"));
    CHECK(!m.engine_writes && m.port_reads+m.port_writes==ports);
    CHECK(!eligibility(&device,&found));
    CHECK(writes==m.engine_writes && !binding_claims && !marker_commits);
    CHECK(!savage2d_start_chip(&test_engine,&io,&device,chip,m.cursor_page,0x100000,cob));
    acceleration=&test_engine.ops;
    CHECK(test_engine.status_reg==(early_bus ? SAVAGE_EARLY_STATUS : SAVAGE_STATUS));
    CHECK((test_engine.descriptor&9u)==(chip_config(chip) ? 1u : 9u));
    CHECK(m.seq[0x26]==(chip==SAVAGE_SUPER ? 0x40 : 0));
    CHECK(m.reg[BCI_CONTROL/4]==(early_bus ? 12u : 8u));
    CHECK(cob_programs==(early_bus ? 1u : 0u));
    CHECK(m.register_packets==(chip==SAVAGE_4 ? 0u : 4u));
    CHECK((early_bus ? early_reads : later_reads)>0);
    CHECK(test_engine.ops.capabilities(&test_engine)==3u && test_engine.cpu_safe);

    uint8_t *expected=malloc(device.size), *before=malloc(device.size);
    CHECK(expected && before);
    memcpy(expected,device.mapped,device.size);
    struct fb_device sw=device; sw.mapped=expected;
    struct fb_rect r={10,12,35,20};
    uint32_t colour=swdib_pack(&device,0x12345678u);
    swdib_fill(&sw,&r,colour);
    CHECK(!test_engine.ops.fill_rect(&test_engine,&r,colour));
    CHECK(!memcmp(expected,device.mapped,device.size));
    CHECK((m.packet[0]&~(XP|YP))==0x48cc8000u);
    for (int y=-1;y<=1;y++) for (int x=-1;x<=1;x++) {
        struct fb_rect src={12,14,30,16};
        memcpy(before,device.mapped,device.size);
        swdib_copy(&sw,&src,src.x+x,src.y+y);
        CHECK(!test_engine.ops.copy_rect(&test_engine,&src,src.x+x,src.y+y));
        CHECK(!memcmp(expected,device.mapped,device.size));
        CHECK(!!(m.packet[0]&XP)==(x<=0) && !!(m.packet[0]&YP)==(y<=0));
    }
    uint8_t pattern[8]={0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81};
    swdib_pattern(&sw,&r,pattern,0xabcdefu,0x765432u);
    CHECK(!test_engine.ops.pattern_rect(&test_engine,&r,pattern,0xabcdefu,0x765432u));
    CHECK(!memcmp(expected,device.mapped,device.size));
    uint8_t rows[8*8*4];
    for (unsigned i=0;i<sizeof(rows);i++) rows[i]=(uint8_t)(i*37u);
    struct fb_rect dst={17,19,8,8}, clip={19,20,4,6};
    m.fifo_model=true; m.fifo_drain=8;
    CHECK(!test_engine.ops.verify_write(&test_engine) && test_engine.write_state==1);
    CHECK(!memcmp(expected,device.mapped,device.size));
    swdib_write(&sw,&dst,&clip,rows,8*(bpp/8));
    CHECK(!test_engine.ops.write_rect(&test_engine,&dst,&clip,rows,8*(bpp/8)));
    CHECK(!memcmp(expected,device.mapped,device.size) && !m.fifo_overflow);
    CHECK(m.images==2 && test_engine.writes==2);
    uint32_t tested=0;
    CHECK(!fbdev_accel_validate(g_ticks+5000,NULL,NULL,&tested) && tested==15u);
    CHECK(!memcmp(expected,device.mapped,device.size)); /* DM9 restores saved block */
    struct accel_cursor cursor={.foreground=0xabcdefu,.background=0x123456u};
    cursor.mask[0]=0xc0000000u; cursor.image[0]=0x40000000u;
    CHECK(!test_engine.ops.cursor_set(&test_engine,&cursor) && !(m.cr[0x45]&1u));
    CHECK(m.cursor_page[0]==0x3f && m.cursor_page[1]==0xff && m.cursor_page[2]==0x40 && m.cursor_page[3]==0x00); /* f2-99 MSB-first byte order */
    CHECK(!test_engine.ops.cursor_move(&test_engine,-7,511));
    CHECK(m.cr[0x4e]==7 && m.cr[0x4f]==0 && m.cr[0x48]==1 && m.cr[0x49]==255);
    CHECK(!test_engine.ops.cursor_show(&test_engine,true) && (m.cr[0x45]&1u));
    CHECK(!test_engine.ops.sync(&test_engine));
    unsigned packets=m.packets;
    struct fb_rect invalid={INT32_MAX,0,4,1};
    CHECK(test_engine.ops.fill_rect(&test_engine,&invalid,0)==-EINVAL && m.packets==packets);
    CHECK(!test_engine.ops.stop(&test_engine) && !(m.cr[0x45]&1u));
    CHECK(!(m.reg[BCI_CONTROL/4]&15u) && !m.reg[0x48c14/4]);
    free(before); free(expected); finish();
}
static void family_faults(uint16_t id)
{
    for (unsigned fault_kind=0;fault_kind<4;fault_kind++) {
        uint32_t cob=family_fixture(id,32);
        struct savage_io io=family_io();
        CHECK(!savage2d_start_chip(&test_engine,&io,&device,savage2d_chip(id),m.cursor_page,0x100000,cob));
        acceleration=&test_engine.ops;
        struct accel_cursor cursor={0};
        CHECK(!test_engine.ops.cursor_set(&test_engine,&cursor));
        CHECK(!test_engine.ops.cursor_show(&test_engine,true));
        if (fault_kind==0) m.fail_after_submit=true;
        if (fault_kind==1) m.fail_after_submit=m.frozen=true;
        if (fault_kind==2) m.dead=true;
        if (fault_kind==3) m.fail_after_submit=true;
        struct fb_rect r={0,0,4,4};
        unsigned reads=m.status_reads, packets=m.packets;
        CHECK(test_engine.ops.fill_rect(&test_engine,&r,0x11223344u)==-EIO);
        CHECK(test_engine.quarantined && !test_engine.active && !test_engine.cpu_safe);
        CHECK(!test_engine.ops.capabilities(&test_engine) && !(m.cr[0x45]&1u));
        CHECK(m.status_reads-reads<=WAIT_POLLS && m.fault_count==1);
        CHECK(test_engine.ops.fill_rect(&test_engine,&r,0)==-ENODEV && m.packets<=packets+1);
        if (fault_kind==3) m.stuck=m.fail_after_submit=false;
        CHECK(test_engine.ops.sync(&test_engine)==(fault_kind==3 ? 0 : -EIO));
        reads=m.status_reads;
        CHECK(test_engine.ops.sync(&test_engine)==(fault_kind==3 ? 0 : -EIO));
        CHECK(m.status_reads==reads); /* one final proof, then cached */
        CHECK(savage2d_start_chip(&test_engine,&io,&device,savage2d_chip(id),NULL,0,cob)==-EIO);
        finish();
    }
}
static unsigned invalid_bar(const struct pci_func *p) { (void)p; return 6; }
static void family_refusals(void)
{
    uint32_t cob=family_fixture(0x8a20,32);
    struct savage_io io=family_io();
    unsigned writes=m.port_writes;
    CHECK(savage2d_start_chip(&test_engine,&io,&device,SAVAGE_3D,NULL,0,0)==-EINVAL);
    CHECK(savage2d_start_chip(&test_engine,&io,&device,SAVAGE_UNKNOWN,NULL,0,0)==-EINVAL);
    CHECK(savage2d_start_chip(&test_engine,&io,&device,SAVAGE_3D,m.cursor_page,cob, cob)==-EINVAL);
    CHECK(savage2d_start_chip(&test_engine,&io,&device,SAVAGE_3D,NULL,0,UINT32_MAX)==-EINVAL);
    CHECK(!m.engine_writes && writes==m.port_writes);
    const struct pci_func *found=NULL;
    const struct display_driver *table[]={&family_driver};
    struct display_driver bad=family_driver;
    bad.framebuffer_bar_for=invalid_bar;
    const struct display_driver *bad_table[]={&bad};
    CHECK(!display_driver_select(&device,bad_table,1,&found) && !found);
    for (unsigned refusal=0;refusal<9;refusal++) {
        struct pci_func saved=binding_gpu;
        struct fb_device fb=device;
        if (refusal==0) binding_gpu.vendor=0x1234;
        if (refusal==1) binding_gpu.device=0x9102;
        if (refusal==2) binding_gpu.command=1;
        if (refusal==3) binding_gpu.bar[0]|=4u;
        if (refusal==4) fb.phys+=0x100000;
        if (refusal==5) fb.generation++;
        if (refusal==6) g_boot.display.vram_bytes=1;
        if (refusal==7) binding_pci_count=2;
        if (refusal==8) { g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=device.phys,.length=device.size,.type=CBI_E820_RAM}; }
        CHECK(!display_driver_select(&fb,table,1,&found) && !found);
        binding_gpu=saved; binding_pci_count=1; g_boot.display.vram_bytes=8u*1024*1024; g_boot.e820_count=0;
    }
    CHECK(!m.engine_writes && writes==m.port_writes && !marker_commits && !binding_claims);
    g_boot.display.tag=0;
    CHECK(!strcmp(eligibility(&device,&found),"cob_unavailable"));
    CHECK(!m.engine_writes && writes==m.port_writes && !marker_commits && !binding_claims);
    g_boot.display.tag=CBI_DISPLAY_TAG;
    for (unsigned vram=0;vram<=20*1024*1024;vram+=1024*1024) {
        uint32_t offset=0;
        bool valid=savage2d_cob_span(&device,vram,&offset);
        CHECK(valid==(vram>=4*1024*1024 && vram<=16*1024*1024));
        if (valid) CHECK(offset>=device.size && offset+SAVAGE_COB_BYTES<=vram &&
            offset+SAVAGE_COB_BYTES<=SAVAGE_COB_LIMIT &&
            !(offset&(SAVAGE_COB_BYTES-1)) && !((offset+SAVAGE_COB_BYTES)%0x400000u));
    }
    wrong_status=true;
    CHECK(savage2d_start_chip(&test_engine,&io,&device,SAVAGE_3D,NULL,0,cob)==-EIO);
    CHECK(test_engine.quarantined && !test_engine.cpu_safe && !m.packets);
    finish();
    cob=family_fixture(0x8a20,32); io=family_io(); corrupt_cob=true;
    CHECK(savage2d_start_chip(&test_engine,&io,&device,SAVAGE_3D,NULL,0,cob)==-EIO);
    CHECK(test_engine.quarantined && !strcmp(test_engine.reason,"cob_readback_failed") && !m.packets);
    finish();
}
static void family_lifecycle(uint16_t id,unsigned injection)
{
    uint32_t cob=family_fixture(id,32);
    struct savage_io io=family_io();
    family_resources=true; family_claimed=family_maps=0;
    fail_cob_claim=injection==1; fail_cob_activate=injection==2;
    fail_cursor_claim=injection==4;
    memset(family_leases,0,sizeof(family_leases));
    cob_claim=offscreen_claim=-1; cursor_mapping=NULL;
    g_boot.display.tag=CBI_DISPLAY_TAG;
    marker_thread_running=true; accel_initializing=true;
    const struct accel_ops *ops=init_with_io(&device,&settings_vfs,&io);
    marker_thread_running=false; accel_initializing=false;
    if (injection==1 || injection==2) {
        CHECK(!ops && engine.quarantined && !m.engine_writes && !m.port_writes);
        CHECK(!cob_programs && family_maps==1);
    } else {
        CHECK(ops==&engine.ops && engine.active && engine.cpu_safe && !disk_size);
        if (!ops) {
            printf("family lifecycle pci=%04x reason=%s\n",id,summary_state.reason);
            family_resources=false; cob_claim=offscreen_claim=reg_claim=port_claim=cursor_claim=-1;
            cursor_mapping=NULL; registers=NULL; finish(); return;
        }
        CHECK(engine.cob_offset==cob && family_maps==(injection==4 ? 1u : 2u));
        CHECK(!strcmp(last_marker_path,early_bus ?
            "C:/system/settings/accel-s3savage-5333-8a20.cfg" :
            "C:/system/settings/accel-s3savage-5333-8a25.cfg"));
        CHECK(cob ? cob_claim>=0 : cob_claim<0);
        if (injection==4) {
            CHECK(ops->capabilities(ops->ctx)==1u && !engine.cursor);
            struct accel_cursor cursor={0}; unsigned writes=m.port_writes;
            CHECK(ops->cursor_set(ops->ctx,&cursor)==-ENODEV && m.port_writes==writes);
            CHECK((uint64_t)engine.rows*device.pitch<=cob);
        } else {
            CHECK(cursor_phys+PAGE_SIZE<=device.phys+(cob ? cob : g_boot.display.vram_bytes));
            CHECK((uint64_t)engine.rows*device.pitch<=cursor_phys-device.phys);
        }
        int saved_cob=cob_claim;
        if (injection==3) {
            m.fail_after_submit=true;
            struct fb_rect r={0,0,4,4};
            CHECK(ops->fill_rect(ops->ctx,&r,0)==-EIO && engine.quarantined);
            CHECK(cob_claim==saved_cob && !engine.cpu_safe);
            CHECK(family_leases[saved_cob].state==RS_QUARANTINED);
            CHECK(savage2d_fallback_ops()==&engine.ops && !engine.cpu_safe);
            unsigned reads=m.status_reads;
            CHECK(savage2d_fallback_ops()==&engine.ops && m.status_reads==reads);
        } else {
            CHECK(!ops->stop(ops->ctx) && cob_claim<0);
            for (unsigned i=1;i<=family_claimed;i++) CHECK(family_leases[i].state==RS_RELEASED);
        }
    }
    family_resources=false; cob_claim=offscreen_claim=reg_claim=port_claim=cursor_claim=-1;
    cursor_mapping=NULL; registers=NULL;
    fail_cob_claim=fail_cob_activate=fail_cursor_claim=false; finish();
}
int main(void)
{
    static const uint16_t ids[]={0x8a20,0x8a21,0x8c10,0x8c11,0x8c12,0x8c13,
        0x8a22,0x8a25,0x8a26,0x8d01,0x8d02,0x8d03,0x8d04,
        0x8c22,0x8c24,0x8c26,0x8c2a,0x8c2b,0x8c2c,0x8c2d,0x8c2e,0x8c2f};
    CHECK(family_driver.match_count==ARRAY_SIZE(ids));
    CHECK(!savage2d_supported(NULL));
    for (unsigned i=0;i<ARRAY_SIZE(ids);i++) {
        family_commands(ids[i],16); family_commands(ids[i],32); family_faults(ids[i]);
        struct pci_func p={.class_code=3,.vendor=0x5333,.device=ids[i]};
        CHECK(family_driver.mode_adapter(&p)==(ids[i]==0x8c2e));
        printf("Savage family pci=5333:%04x: 16/32bpp F/C/P/W/U goldens, SYNC, timeout, quarantine PASS\n",ids[i]);
    }
    family_refusals();
    family_lifecycle(0x8a20,0); family_lifecycle(0x8a25,0);
    for (unsigned injection=1;injection<=4;injection++) family_lifecycle(0x8a20,injection);
    printf("Savage family: %u failures\n",failures);
    return failures ? 1 : 0;
}
