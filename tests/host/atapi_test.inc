/* Included in the real ATA/sync/registry host harness. Packet mock shares
 * one task file with its real production ATA peer, never a second owner. */
struct packet_fake {
    struct fake disk;
    uint8_t status, reason, command, bytes[4096], cdb[16], sense_key, asc, ascq;
    unsigned phase_bytes, remaining, pos, packet_words, packet_length, packets, polls;
    unsigned phases[4], phase_index, phase_count;
    unsigned packet_unit, attentions;
    bool no_medium, zero_status, bad_iso, needs_start;
    FILE *image;
    uint32_t image_blocks;
    bool timeout, bad_direction, zero_count, over_count, check, packet_selected, responses;
};
static struct packet_fake *packet_running;
static uint8_t packet_in8(void *ctx, uint16_t port)
{
    struct packet_fake *f=ctx;
    if (!f->packet_selected) return fake_in8(&f->disk,port);
    unsigned r=port-f->disk.channel.base;
    if (port==f->disk.channel.control || r==7) { f->polls++; return f->timeout && f->command ? 0x80 : f->status; }
    if (r==2) return f->reason;
    if (r==4) return (uint8_t)(f->zero_count ? 0 : f->over_count ? 4000 : f->phase_bytes);
    if (r==5) return (uint8_t)((f->over_count ? 4000 : f->phase_bytes)>>8);
    return r==1 ? 0x20 : 0;
}
static void packet_out8(void *ctx, uint16_t port, uint8_t value)
{
    struct packet_fake *f=ctx; CHECK(flags&0x200 || port==f->disk.channel.base+7);
    unsigned r=port-f->disk.channel.base;
    if (r==6) {
        f->packet_selected=(unsigned)!!(value&16)==f->packet_unit;
        if (!f->packet_selected) { fake_out8(&f->disk,port,value); return; }
        f->status=f->zero_status ? 0 : 0x40; f->reason=3; f->command=0; return;
    }
    if (!f->packet_selected || port==f->disk.channel.control) { fake_out8(&f->disk,port,value); return; }
    if (r==7) {
        f->command=value; f->pos=0; f->packet_words=0;
        f->status=0x48; f->reason=1;
    }
}
static void packet_next(struct packet_fake *f)
{
    if (f->phase_index==f->phase_count) { f->status=f->zero_status ? 0 : 0x40; f->reason=3; return; }
    f->phase_bytes=f->phases[f->phase_index++]; f->remaining=f->phase_bytes;
    f->status=0x48; f->reason=f->bad_direction ? 0 : 2;
}
static uint16_t packet_in16(void *ctx, uint16_t port)
{
    struct packet_fake *f=ctx;
    if (!f->packet_selected) return fake_in16(&f->disk,port);
    CHECK(flags&0x200);
    if (f->command==0xa1) {
        uint16_t v=f->pos==0 ? (uint16_t)(0x8500|(f->packet_length==16)) : 0;
        if (++f->pos==256) { f->status=0x40; f->reason=3; }
        return v;
    }
    CHECK(f->remaining);
    unsigned i=f->pos; f->pos+=2;
    uint16_t v=f->bytes[i]|(uint16_t)f->bytes[i+1]<<8;
    if (f->remaining<=2) { f->remaining=0; packet_next(f); } else f->remaining-=2;
    return v;
}
static void packet_out16(void *ctx, uint16_t port, uint16_t value)
{
    struct packet_fake *f=ctx;
    if (!f->packet_selected) { fake_out16(&f->disk,port,value); return; }
    CHECK(flags&0x200); CHECK(f->packet_words<f->packet_length/2);
    fs_wr16(f->cdb+f->packet_words*2,value);
    if (++f->packet_words!=f->packet_length/2) return;
    f->packets++; f->phase_index=0; f->pos=0;
    if (f->cdb[0]==0 && (f->attentions || f->no_medium || f->needs_start)) {
        f->sense_key=f->attentions ? 6 : 2; f->asc=f->attentions ? 0x29 : 0x3a;
        f->ascq=0;
        if (!f->attentions && !f->no_medium && f->needs_start) { f->asc=4; f->ascq=2; }
        if (f->attentions) f->attentions--;
        f->status=0x41; f->reason=3; return;
    }
    if (f->check && f->cdb[0]!=3) { f->status=0x41; f->reason=3; return; }
    if (f->cdb[0]==3) {
        memset(f->bytes,0,18); f->bytes[0]=0x70; f->bytes[2]=f->sense_key; f->bytes[7]=10; f->bytes[12]=f->asc; f->bytes[13]=f->ascq;
        f->phases[0]=18; f->phase_count=1;
    } else if (f->responses) {
        memset(f->bytes,0,sizeof(f->bytes)); f->phase_count=0;
        switch (f->cdb[0]) {
        case 0: case 0x1e: break;
        case 0x1b:
            if (f->cdb[4]==2) f->no_medium=true;
            if (f->cdb[4]==1) { CHECK(f->cdb[1]==1); f->needs_start=false; }
            break;
        case 0x12: f->bytes[0]=5; f->phases[0]=36; f->phase_count=1; break;
        case 0x25:
            if (f->image) { uint32_t last=f->image_blocks-1; f->bytes[0]=last>>24; f->bytes[1]=last>>16; f->bytes[2]=last>>8; f->bytes[3]=last; }
            else f->bytes[3]=63;
            f->bytes[6]=8;
            f->phases[0]=8; f->phase_count=1; break;
        case 0x43:
            CHECK(f->cdb[8]==20 || (f->cdb[8]==12 && f->cdb[9]==0x40));
            f->bytes[1]=(uint8_t)(f->cdb[8]-2); f->bytes[2]=f->bytes[3]=1; f->bytes[5]=0x14; f->bytes[6]=1;
            f->phases[0]=f->cdb[8]; f->phase_count=1; break;
        case 0x28:
            if (f->image) {
                uint32_t lba=(uint32_t)f->cdb[2]<<24 | (uint32_t)f->cdb[3]<<16 | (uint32_t)f->cdb[4]<<8 | f->cdb[5];
                CHECK(!f->cdb[7] && f->cdb[8]==1 && lba<f->image_blocks);
                CHECK(!fseek(f->image,(long)lba*2048,SEEK_SET) && fread(f->bytes,2048,1,f->image)==1);
                if (f->bad_iso && lba==16) memset(f->bytes,0,2048);
                f->phases[0]=2048; f->phase_count=1; break;
            }
            CHECK(!f->cdb[2] && !f->cdb[3] && !f->cdb[4] && f->cdb[5]==7 && !f->cdb[7] && f->cdb[8]==1);
            for (unsigned i=0;i<2048;i++) f->bytes[i]=(uint8_t)(i*13);
            f->phases[0]=1024; f->phases[1]=1024; f->phase_count=2; break;
        default: CHECK(false);
        }
    }
    packet_next(f);
}
static uint64_t packet_now(void *ctx) { (void)ctx; return g_ticks; }
static uint64_t packet_deadline(void *ctx, uint64_t deadline) { (void)ctx; return deadline; }
static int packet_delay(void *ctx, uint32_t us) { (void)ctx; (void)us; return 0; }
static const struct ata_port_ops packet_ops={packet_in8,packet_out8,packet_in16,packet_out16,packet_now,packet_deadline,packet_delay,0,0};
static void packet_yield_check(void)
{
    /* Task-file ownership remains held even at every deliberate yield. */
    struct ata_channel *c=&packet_running->disk.channel;
    CHECK(c->mutex.owner==g_current && c->active==&c->devices[packet_running->packet_unit]);
    CHECK(flags&0x200);
}
static void packet_fresh(struct packet_fake *f, unsigned length)
{
    memset(f,0,sizeof(*f)); qualify(&f->disk,false);
    f->packet_length=length; f->packet_unit=1;
    struct ata_channel *c=&f->disk.channel; c->ops=&packet_ops; c->port_ctx=f;
    struct ata_device *d=&c->devices[1]; d->present=d->atapi=true;
    kmutex_lock(&c->mutex); CHECK(!identify_packet(d)); kmutex_unlock(&c->mutex);
    CHECK(d->identified && d->packet_type==5 && d->packet_bytes==length && d->block.sector_size==2048);
    packet_running=f;
}
static void test_packets(void)
{
    struct packet_fake f; uint8_t p[12]={0x12}, data[4098]; uint32_t done;
    for (unsigned length=12; length<=16; length+=4) {
        packet_fresh(&f,length);
        for (unsigned i=0;i<sizeof(f.bytes);i++) f.bytes[i]=(uint8_t)(i*13);
        f.phases[0]=2; f.phases[1]=4; f.phases[2]=3; f.phase_count=3;
        memset(data,0xa5,sizeof(data)); on_yield=packet_yield_check;
        CHECK(!atapi_packet(&f.disk.channel.devices[1],p,sizeof(p),data+1,9,&done));
        on_yield=0; CHECK(done==9 && data[0]==0xa5 && data[10]==0xa5 && f.packets==1);
        CHECK(!memcmp(data+1,f.bytes,9));
        uint8_t peer[512]; CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block,0,1,peer));
        CHECK(fs_rd16(peer)==sector_word(0,0));
    }
    for (unsigned fault=0;fault<5;fault++) {
        packet_fresh(&f,12); f.phases[0]=2; f.phase_count=1;
        f.bad_direction=fault==0; f.zero_count=fault==1; f.over_count=fault==2; f.timeout=fault==3;
        if (fault==4) { f.phases[0]=3; f.phases[1]=2; f.phase_count=2; }
        struct ata_device *d=&f.disk.channel.devices[1]; uint64_t start=g_ticks;
        CHECK(atapi_packet(d,p,sizeof(p),data,16,&done)==-FS_EIO);
        CHECK(f.disk.channel.stopped && d->block.quarantined && f.disk.channel.devices[0].block.quarantined);
        CHECK(g_ticks-start<=ATAPI_COMMAND_MS);
        unsigned issued=f.packets; CHECK(atapi_packet(d,p,sizeof(p),data,16,&done)==-FS_EQUARANTINED && f.packets==issued);
    }
    for (unsigned sense=0;sense<3;sense++) {
        packet_fresh(&f,12); f.check=true; f.sense_key=sense==2 ? 6 : 2; f.asc=sense==0 ? 0x3a : 4;
        struct ata_device *d=&f.disk.channel.devices[1]; uint32_t gen=d->block.media_generation;
        CHECK(atapi_packet(d,p,sizeof(p),data,16,&done)==-FS_EIO);
        CHECK(!f.disk.channel.stopped && !f.disk.channel.devices[0].block.quarantined);
        CHECK(d->block.media_generation>gen && d->sense_key==f.sense_key && f.packets==2);
        uint8_t peer[512]; CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block,0,1,peer));
    }
    packet_fresh(&f,12); f.responses=true;
    struct ata_device *d=&f.disk.channel.devices[1]; uint8_t peer[512];
    CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block,0,1,peer));
    CHECK(!atapi_ready(d) && d->media==ATAPI_READY && d->block.capacity==64 && d->session_lba==0 && f.packets==5);
    CHECK(!d->block.read(&d->block,7,1,data)); CHECK(!memcmp(data,f.bytes,2048));
    unsigned issued=f.packets; CHECK(!atapi_ready(d) && f.packets==issued+1);
    CHECK(atapi_packet(d,(uint8_t[]){0x2a},1,data,2048,&done)==-FS_EOPNOTSUPP);
    uint32_t gen=d->block.media_generation;
    CHECK(!atapi_eject(d) && f.cdb[0]==0x1b && f.cdb[4]==2 && d->media==ATAPI_NO_MEDIUM && !d->block.capacity && d->block.media_generation>gen);
    CHECK(!atapi_start(d) && f.cdb[4]==3);
    CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block,0,1,peer));
    CHECK(!d->block.write && !d->block.flush && !d->media_writes);
    packet_fresh(&f,12); f.responses=true; f.attentions=1; f.zero_status=true;
    d=&f.disk.channel.devices[1];
    CHECK(!atapi_ready(d) && d->media==ATAPI_READY && f.packets==7 && !d->block.quarantined);
    packet_fresh(&f,12); f.responses=true; f.attentions=4;
    d=&f.disk.channel.devices[1];
    CHECK(atapi_ready(d)==-FS_EIO && f.packets==6 && f.attentions==1 && !d->block.quarantined);
    CHECK(!atapi_ready(d) && d->media==ATAPI_READY);
    packet_fresh(&f,12); f.responses=true; f.needs_start=true;
    d=&f.disk.channel.devices[1];
    CHECK(atapi_ready(d)==-FS_EIO && !f.needs_start && f.packets==3 && f.cdb[0]==0x1b && f.cdb[4]==1);
    CHECK(!atapi_ready(d) && d->media==ATAPI_READY);
    puts("ATAPI production packets: PASS (12/16-byte identify, odd/short/multi-phase, canaries, sense, deadline, peer mutex/quarantine, ready/read/eject/start commands)");
}
static struct storage optical_store;
static struct vfs_table optical_handles;
static void test_optical_lifecycle(void)
{
    FILE *image=fopen("build/host/optical/fixture-0.iso","rb");
    CHECK(image); if (!image) return;
    CHECK(!fseek(image,0,SEEK_END)); long size=ftell(image); CHECK(size>0 && !(size%2048));
    for (unsigned unit=0;unit<2;unit++) {
        struct packet_fake f; packet_fresh(&f,12);
        struct ata_channel *c=&f.disk.channel;
        /* Repeat discovery on a secondary master as well as the fixture's
         * secondary slave. The other unit remains a real ATA peer. */
        c->irq=15;
        if (!unit) {
            struct ata_device peer=c->devices[0];
            memset(&c->devices[0],0,sizeof(c->devices[0]));
            c->devices[0].channel=c; c->devices[0].present=c->devices[0].atapi=true;
            c->devices[0].block.ctx=&c->devices[0];
            c->devices[1]=peer; c->devices[1].unit=1; c->devices[1].block.ctx=&c->devices[1];
            f.packet_unit=0; f.packet_selected=false;
            kmutex_lock(&c->mutex); CHECK(!identify_packet(&c->devices[0])); kmutex_unlock(&c->mutex);
        }
        struct ata_device *d=&c->devices[unit]; unsigned slot=2+unit;
        f.responses=true; f.image=image; f.image_blocks=(uint32_t)(size/2048); f.no_medium=true;
        CHECK(!storage_setup(&optical_store,32768));
        CHECK(!optical_bind(&optical_store,slot,d));
        optical_service(&optical_store,0);
        CHECK(d->media==ATAPI_NO_MEDIUM && !optical_store.optical[slot].mounted && !vfs_drive_present(&optical_store.vfs,3));
        unsigned issued=f.packets; optical_service(&optical_store,999); CHECK(f.packets==issued);
        f.no_medium=false; f.attentions=1;
        optical_service(&optical_store,1000);
        CHECK(optical_store.optical[slot].mounted && vfs_drive_present(&optical_store.vfs,3));
        CHECK(!vfs_table_init(&optical_store.vfs,&optical_handles,2));
        int h=vfs_open(&optical_handles,"D:/Caf\xc3\xa9/Gr\xc3\xbc\xc3\x9f" "e.txt",VFS_READ,VFS_DENY_NONE,0);
        CHECK(h>=0); uint8_t bytes[80]; size_t count=0;
        CHECK(!vfs_read(&optical_handles,h,bytes,sizeof(bytes),&count));
        static const char text[]="Ciuki: caff\xc3\xa8, Gr\xc3\xbc\xc3\x9f" "e.\n";
        CHECK(count==sizeof(text)-1 && !memcmp(bytes,text,count));
        CHECK(vfs_open(&optical_handles,"D:/README.TXT",VFS_WRITE,VFS_DENY_NONE,0)==-FS_EROFS);
        CHECK(optical_eject(&optical_store,3,d->block.media_generation-1)==-FS_EIO);
        CHECK(optical_eject(&optical_store,3,d->block.media_generation)==-FS_EBUSY);
        uint32_t generation=d->block.media_generation;
        f.no_medium=true; optical_service(&optical_store,2000);
        CHECK(!vfs_drive_present(&optical_store.vfs,3) && !optical_store.optical[slot].mounted && d->block.media_generation>generation);
        CHECK(vfs_read(&optical_handles,h,bytes,1,&count)==-FS_EIO && !count);
        f.no_medium=false; optical_service(&optical_store,3000);
        CHECK(vfs_drive_present(&optical_store.vfs,3));
        CHECK(vfs_read(&optical_handles,h,bytes,1,&count)==-FS_EIO && !count);
        CHECK(!vfs_close(&optical_handles,h));
        CHECK(!optical_eject(&optical_store,3,d->block.media_generation) && !vfs_drive_present(&optical_store.vfs,3));
        struct fs_ro_volume occupied={.mounted=true,.cache=&optical_store.cache};
        optical_store.vfs.ro_volumes[3]=&occupied;
        f.no_medium=false; optical_service(&optical_store,4000);
        CHECK(vfs_drive_present(&optical_store.vfs,4) && optical_store.vfs.ro_volumes[4]==&optical_store.optical[slot]);
        f.no_medium=true; optical_service(&optical_store,5000);
        CHECK(!vfs_drive_present(&optical_store.vfs,4));
        f.no_medium=false; f.bad_iso=true; optical_service(&optical_store,6000);
        CHECK(!optical_store.optical[slot].mounted && optical_store.optical_bindings[slot].result<0 && !d->block.quarantined);
        issued=f.packets; optical_service(&optical_store,7000);
        CHECK(f.packets==issued+1); /* readiness only, no repeated ISO mount */
        optical_store.vfs.ro_volumes[3]=0;
        CHECK(!d->media_writes && !d->block.write && !d->block.flush && !d->dma_enabled && !c->stopped);
        vfs_table_destroy(&optical_handles); storage_destroy(&optical_store);
    }
    fclose(image);
    puts("PASS optical lifecycle: secondary master/slave, empty boot, 1s insertion polling, reset attention, Joliet reads, EROFS, busy/stale eject, removal/stale handles, reinsertion, zero writes/PIO");
}
