/* Full INT13 + virtual FDC + existing guest DMA fixture.
 * SPDX-License-Identifier: GPL-2.0-only */
static void floppy_out(struct dosvm *v,unsigned reg,uint8_t b)
{ uint32_t x=b; CHECK(!floppy_port(v,0x3f0+reg,1,true,&x,0)); }
static uint8_t floppy_in(struct dosvm *v,unsigned reg)
{ uint32_t x=0; CHECK(!floppy_port(v,0x3f0+reg,1,false,&x,0)); return x; }
static void floppy_cmd(struct dosvm *v,const uint8_t *p,unsigned n)
{ for(unsigned i=0;i<n;i++)floppy_out(v,5,p[i]); }
static void floppy_guest_dma(struct dosvm *v,uint16_t address,uint16_t count,bool read)
{
    dosvm_dma_io(v,0x0a,true,6);dosvm_dma_io(v,0x0c,true,0);
    dosvm_dma_io(v,0x0b,true,read ? 0x46 : 0x4a);
    dosvm_dma_io(v,4,true,address);dosvm_dma_io(v,4,true,address>>8);
    dosvm_dma_io(v,0x81,true,1);dosvm_dma_io(v,5,true,count);dosvm_dma_io(v,5,true,count>>8);
    dosvm_dma_io(v,0x0a,true,2);
}
static void test_floppy_vm(void)
{
    floppy_image_bytes=calloc(1,1440u*1024);CHECK(floppy_image_bytes);
    for(unsigned i=0;i<512;i++)floppy_image_bytes[i]=(uint8_t)(i*31);
    struct dosvm *v=create(),*other=create();
    uint16_t equipment=0xc822; /* four COM ports and three LPT ports */
    CHECK(!dosvm_write(v,0x410,&equipment,2));
    CHECK(!dosvm_floppy_image(v->info.key,0,"C:\\TESTFLOP.IMG",true));
    CHECK(!dosvm_read(v,0x410,&equipment,2) && equipment==0xc823);
    struct v86_frame equipment_frame=disk_frame(0xabcd);
    equipment_frame.tf.eax|=0x12340000;
    CHECK(!dosvm_bios(v,&equipment_frame,0x11) && equipment_frame.tf.eax==0x1234c823);
    CHECK(dosvm_floppy_image(other->info.key,0,"C:\\TESTFLOP.IMG",true)==-FS_EACCES);
    struct v86_frame f=disk_frame(0x08ef);f.tf.edx=0;
    CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&1) && !(f.tf.eax&255) && (f.tf.ecx&63)==18 && (f.tf.ebx&255)==4 && f.es==0xf000 && (uint16_t)f.tf.edi==0x1e3);
    CHECK(dosvm_floppy_equipment(v)==0x23 && dosvm_floppy_equipment(other)==0x22);
    uint8_t got[512],data[512];memset(data,0xb6,sizeof(data));
    f=disk_frame(0x0201);f.tf.edx=0;f.tf.ebx=0;
    CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&1) && (f.tf.eax&255)==1);
    CHECK(!dosvm_read(v,0x10000,got,512) && !memcmp(got,floppy_image_bytes,512));
    CHECK(!dosvm_write(v,0x10000,data,512));f=disk_frame(0x0301);f.tf.edx=0;f.tf.ebx=0;
    CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&1) && !memcmp(data,floppy_image_bytes,512));
    f=disk_frame(0x0201);f.tf.edx=0;f.tf.ebx=0xffff;
    CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&1) && ((f.tf.eax>>8)&255)==9);
    f=disk_frame(0x0202);f.tf.edx=0;f.tf.ecx=18;
    CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&1) && (f.tf.eax&255)==2); /* head crossing */
    f=disk_frame(0x0201);f.tf.edx=1;CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&1));
    f=disk_frame(0x0201);f.tf.edx=0;f.es=0xf000;CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&1));
    short_read=true;f=disk_frame(0x0201);f.tf.edx=0;f.tf.ebx=0;
    CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&1) && !(f.tf.eax&255));short_read=false;
    floppy_out(v,2,0);floppy_out(v,2,0x1c);
    CHECK(floppy_in(v,4)==0x80);
    for(unsigned i=0;i<4;i++){uint8_t cmd=8;floppy_cmd(v,&cmd,1);CHECK(floppy_in(v,4)==0xd0);CHECK(floppy_in(v,5)==(0xc0|i));CHECK(!floppy_in(v,5));}
    floppy_guest_dma(v,0,511,true);
    const uint8_t read[]={0x46,0,0,0,1,2,1,0x1b,0xff};floppy_cmd(v,read,sizeof(read));
    CHECK(floppy_in(v,4)==0xd0 && !floppy_in(v,5));
    CHECK(!floppy_in(v,5) && !floppy_in(v,5) && floppy_in(v,5)==1 && !floppy_in(v,5) && floppy_in(v,5)==1 && floppy_in(v,5)==2);
    CHECK(floppy_in(v,4)==0x80 && (dosvm_dma_io(v,8,false,0)&4));
    CHECK(v->audio.dma.ch[2].masked && v->audio.dma.ch[2].count==0xffff);
    CHECK(!dosvm_read(v,0x10000,got,512) && !memcmp(got,data,512));
    memset(data,0x7b,512);CHECK(!dosvm_write(v,0x10000,data,512));
    floppy_guest_dma(v,0,511,false);
    const uint8_t write[]={0x45,0,0,0,1,2,1,0x1b,0xff};floppy_cmd(v,write,sizeof(write));
    CHECK(!floppy_in(v,5));for(unsigned i=1;i<7;i++)(void)floppy_in(v,5);
    CHECK(!memcmp(data,floppy_image_bytes,512));
    uint8_t ids[72];for(unsigned s=0;s<18;s++){ids[s*4]=0;ids[s*4+1]=0;ids[s*4+2]=s+1;ids[s*4+3]=2;}
    CHECK(!dosvm_write(v,0x10000,ids,sizeof(ids)));floppy_guest_dma(v,0,sizeof(ids)-1,false);
    const uint8_t format[]={0x4d,0,2,18,0x6c,0xe5};floppy_cmd(v,format,sizeof(format));
    CHECK(!floppy_in(v,5));for(unsigned i=1;i<7;i++)(void)floppy_in(v,5);
    CHECK(floppy_image_bytes[0]==0xe5 && floppy_image_bytes[18*512-1]==0xe5);
    f=disk_frame(0x0512);f.tf.edx=0;f.tf.ebx=0;
    CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&1) && floppy_image_bytes[0]==0xf6);
    CHECK(!(other->monitor.pic[0].irr&0x40));
    (void)dosvm_dma_io(v,8,false,0);floppy_guest_dma(v,0,1023,true);
    floppy_cmd(v,read,sizeof(read));CHECK(floppy_in(v,5)&0x40);CHECK(floppy_in(v,5)==0x80);
    for(unsigned i=2;i<7;i++)(void)floppy_in(v,5);
    CHECK(v->audio.dma.ch[2].count==511 && !(dosvm_dma_io(v,8,false,0)&4)); /* EOT without TC */
    floppy_guest_dma(v,0,1023,true);
    const uint8_t multitrack[]={0xc6,0,0,0,18,2,18,0x1b,0xff};floppy_cmd(v,multitrack,sizeof(multitrack));
    CHECK(!floppy_in(v,5) && !floppy_in(v,5) && !floppy_in(v,5));
    CHECK(!floppy_in(v,5) && floppy_in(v,5)==1 && floppy_in(v,5)==2 && floppy_in(v,5)==2);
    uint8_t tracks[1024];CHECK(!dosvm_read(v,0x10000,tracks,sizeof(tracks)) && tracks[0]==0xf6 && !tracks[512]);
    /* Wrong direction and a page wrap fail locally without guest writes. */
    floppy_guest_dma(v,0xff00,511,true);floppy_cmd(v,read,sizeof(read));CHECK(floppy_in(v,5)&0x40);
    for(unsigned i=1;i<7;i++)(void)floppy_in(v,5);
    floppy_guest_dma(v,0,511,false);floppy_cmd(v,read,sizeof(read));CHECK(floppy_in(v,5)&0x40);
    for(unsigned i=1;i<7;i++)(void)floppy_in(v,5);
    uint32_t ata=0;CHECK(!floppy_port(v,0x3f6,1,false,&ata,0) && ata==0xff);
    destroy(v,true);CHECK(!floppy_image_owner);
    CHECK(!dosvm_floppy_image(other->info.key,1,"C:\\TESTFLOP.IMG",false));
    CHECK(dosvm_floppy_equipment(other)==0x63);
    f=disk_frame(0x0301);f.tf.edx=1;f.tf.ebx=0;CHECK(!dosvm_bios(other,&f,0x13) && (f.tf.eflags&1) && ((f.tf.eax>>8)&255)==3);
    destroy(other,true);empty();free(floppy_image_bytes);floppy_image_bytes=0;
    puts("floppy VM: PASS (INT13, image exclusion, guest DMA, TC, result phase, teardown)");
}
#ifdef CIUKI_FDC_FIXTURE
extern void host_floppy_open(void),host_floppy_close(void),host_floppy_native(bool),host_floppy_change(void),host_floppy_error(uint8_t),host_floppy_blank(void);
extern unsigned host_floppy_reads(void);
static void test_floppy_physical_vm(void)
{
    host_floppy_open();struct dosvm *v=create(),*other=create();
    host_floppy_native(true);CHECK(dosvm_floppy_assign(v->info.key,0,0)==-FS_EBUSY);host_floppy_native(false);
    host_floppy_blank();
    CHECK(!dosvm_floppy_assign(v->info.key,0,0));
    CHECK(dosvm_floppy_assign(other->info.key,0,0)==-FS_EBUSY);
    uint8_t ids[72];for(unsigned s=0;s<18;s++){ids[s*4]=0;ids[s*4+1]=0;ids[s*4+2]=s+1;ids[s*4+3]=2;}
    CHECK(!dosvm_write(v,0x10000,ids,sizeof(ids)));
    struct v86_frame format=disk_frame(0x0512);format.tf.edx=0;format.tf.ebx=0;
    CHECK(!dosvm_bios(v,&format,0x13) && !(format.tf.eflags&1)); /* no READ ID on blank media */
    struct v86_frame f=disk_frame(0x0201);f.tf.edx=0;f.tf.ebx=0;
    CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&1));
    uint8_t data[512];CHECK(!dosvm_read(v,0x10000,data,512));for(unsigned i=0;i<512;i++)CHECK(data[i]==0xf6);
    unsigned reads=host_floppy_reads();f=disk_frame(0x0201);f.tf.edx=0;f.es=0xf000;
    CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&1) && host_floppy_reads()==reads);
    host_floppy_change();f=disk_frame(0x1600);f.tf.edx=0;
    CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&1) && ((f.tf.eax>>8)&255)==6);
    CHECK(floppy_in(v,7)&0x80);
    const uint8_t recal[]={7,0};floppy_cmd(v,recal,sizeof(recal));uint8_t sense=8;floppy_cmd(v,&sense,1);
    CHECK(floppy_in(v,5)==0x20);CHECK(!floppy_in(v,5));CHECK(!(floppy_in(v,7)&0x80));
    host_floppy_error(0x20);f=disk_frame(0x0201);f.tf.edx=0;f.tf.ebx=0;
    CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&1) && ((f.tf.eax>>8)&255)==0x10);
    floppy_out(v,2,0x1c);floppy_guest_dma(v,0,511,true);
    const uint8_t read[]={0x46,0,0,0,1,2,1,0x1b,0xff};floppy_cmd(v,read,sizeof(read));
    CHECK(floppy_in(v,5)&0x40);CHECK(floppy_in(v,5)==0x20);for(unsigned i=2;i<7;i++)(void)floppy_in(v,5);
    host_floppy_error(0);destroy(v,true);destroy(other,true);empty();
    host_floppy_native(true);host_floppy_native(false);host_floppy_close();
    puts("floppy physical VM: PASS (native/VM exclusion, blank format, INT13, media revalidation, CRC status, retained DMA lifetime)");
}
#endif
