/* Included in the real ATA/sync/registry host harness. Packet mock shares
 * one task file with its real production ATA peer, never a second owner. */
struct packet_fake {
    struct fake disk;
    void (*eject_check)(void);
    uint8_t status, reason, command, bytes[65536], cdb[16], sense_key, asc, ascq;
    unsigned phase_bytes, remaining, pos, packet_words, packet_length, packets, polls;
    unsigned phases[4], phase_index, phase_count;
    unsigned packet_unit, attentions;
    bool no_medium, zero_status, bad_iso, needs_start;
    FILE *image;
    uint32_t image_blocks;
    bool timeout, bad_direction, zero_count, over_count, check, packet_selected, responses;
    /* f5-12B2 CD audio: TOC (default one data track at 0), page 2Ah bytes,
     * sub-channel position, and a CHECK CONDITION injected on READ CD. */
    unsigned toc_count; uint8_t toc_control[8]; uint32_t toc_start[8], toc_leadout;
    uint8_t caps[20]; bool caps_fail, read_cd_fail, play_fail, capacity_fail;
    unsigned stop_packets;
    uint8_t audio_status, sub_track, sub_index; uint32_t sub_abs, sub_rel;
    unsigned read_cd_packets, play_packets, pause_packets, subchannel_packets, caps_packets, toc_packets;
    uint32_t read_cd_lba, read_cd_sectors; uint8_t play_start[3], play_end[3], pause_resume;
    /* fx-hlis5: the host's byte count limit (LBA mid/high), string bursts. */
    unsigned host_limit, bursts, burst_words;
    /* Round 2: deterministic phase boundaries and mid-command failures. */
    unsigned read_phase_bytes, fail_after_phase, failed_at, phase_errors;
    void (*phase_complete)(struct packet_fake *);
};
static void packet_be32_put(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static uint8_t packet_data_byte(unsigned offset) { return (uint8_t)(offset*13+offset/2048); }
/* Deterministic CD-DA sample pattern: sector and offset recoverable. */
static uint8_t packet_cdda_byte(uint32_t lba, uint32_t offset) { return (uint8_t)(lba*31+offset*7+(offset>>8)); }
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
    if (r==4) f->host_limit=(f->host_limit&0xff00u)|value;
    if (r==5) f->host_limit=(f->host_limit&0xffu)|(unsigned)value<<8;
    if (r==7) {
        f->command=value; f->pos=0; f->packet_words=0;
        f->status=0x48; f->reason=1;
    }
}
static void packet_next(struct packet_fake *f)
{
    if (f->cdb[0]==0x28 && f->phase_index) {
        if (f->phase_complete) {
            void (*callback)(struct packet_fake *)=f->phase_complete;
            f->phase_complete=0; callback(f);
        }
        if (f->fail_after_phase==f->phase_index) {
            f->fail_after_phase=0; f->failed_at=f->pos; f->phase_errors++;
            f->status=0x41; f->reason=3; return;
        }
    }
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
    if (f->cdb[0]==0x1b && f->cdb[4]==2 && f->eject_check) f->eject_check();
    if (f->cdb[0]==0 && (f->attentions || f->no_medium || f->needs_start)) {
        f->sense_key=f->attentions ? 6 : 2; f->asc=f->attentions ? 0x29 : 0x3a;
        f->ascq=0;
        if (!f->attentions && !f->no_medium && f->needs_start) { f->asc=4; f->ascq=2; }
        if (f->attentions) f->attentions--;
        f->status=0x41; f->reason=3; return;
    }
    if (f->check && f->cdb[0]!=3) { f->status=0x41; f->reason=3; return; }
    if ((f->read_cd_fail && f->cdb[0]==0xbe) || (f->play_fail && f->cdb[0]==0x47) || (f->caps_fail && f->cdb[0]==0x5a) ||
        (f->capacity_fail && f->cdb[0]==0x25)) {
        f->status=0x41; f->reason=3; return;
    }
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
            if (f->cdb[8]==12 && f->cdb[9]==0x40) { /* SFF session information */
                f->bytes[1]=10; f->bytes[2]=f->bytes[3]=1;
                f->phases[0]=12; f->phase_count=1; break;
            }
            /* MMC-3 Table 230/233: TIME=0, format 0 in byte 2, 804-byte allocation. */
            CHECK(!f->cdb[1] && !f->cdb[2] && !f->cdb[6] && f->cdb[7]==3 && f->cdb[8]==0x24);
            f->toc_packets++;
            {
                unsigned count=f->toc_count ? f->toc_count : 1;
                uint32_t leadout=f->toc_count ? f->toc_leadout : (f->image ? f->image_blocks : 64);
                f->bytes[0]=(uint8_t)((2+8*(count+1))>>8); f->bytes[1]=(uint8_t)(2+8*(count+1));
                f->bytes[2]=1; f->bytes[3]=(uint8_t)count;
                for (unsigned i=0;i<count;i++) {
                    uint8_t *d=f->bytes+4+8*i; d[1]=(uint8_t)(0x10|(f->toc_count ? f->toc_control[i] : 4)); d[2]=(uint8_t)(i+1);
                    packet_be32_put(d+4,f->toc_count ? f->toc_start[i] : 0);
                }
                uint8_t *d=f->bytes+4+8*count; d[1]=0x10; d[2]=0xaa; packet_be32_put(d+4,leadout);
                f->phases[0]=4+8*(count+1); f->phase_count=1;
            }
            break;
        case 0x5a: /* MODE SENSE(10) page 2Ah: 8-byte header, no block descriptors */
            CHECK(f->cdb[2]==0x2a && f->cdb[8]==64 && !f->cdb[7]);
            f->caps_packets++;
            f->bytes[1]=8+20-2; f->bytes[8]=0x2a; f->bytes[9]=18;
            memcpy(f->bytes+10,f->caps+2,18);
            f->phases[0]=8+20; f->phase_count=1; break;
        case 0xbe: { /* READ CD: CD-DA sector type, user data only, no sub-channel */
            CHECK(f->cdb[1]==0x04 && f->cdb[9]==0x10 && !f->cdb[10] && !f->cdb[11] && !f->cdb[6]);
            uint32_t lba=(uint32_t)f->cdb[2]<<24|(uint32_t)f->cdb[3]<<16|(uint32_t)f->cdb[4]<<8|f->cdb[5];
            uint32_t sectors=(uint32_t)f->cdb[7]<<8|f->cdb[8];
            CHECK(sectors>=1 && sectors<=27);
            f->read_cd_packets++; f->read_cd_lba=lba; f->read_cd_sectors=sectors;
            for (uint32_t i=0;i<sectors*2352;i++) f->bytes[i]=packet_cdda_byte(lba+i/2352,i%2352);
            f->phases[0]=sectors*2352; f->phase_count=1; break;
        }
        case 0x47:
            CHECK(!f->cdb[1] && !f->cdb[2] && !f->cdb[9]);
            f->play_packets++; memcpy(f->play_start,f->cdb+3,3); memcpy(f->play_end,f->cdb+6,3);
            f->audio_status=0x11; break;
        case 0x4e: /* STOP PLAY/SCAN: all bytes but the opcode reserved */
            for (unsigned i=1;i<12;i++) CHECK(!f->cdb[i]);
            f->stop_packets++; f->audio_status=0x13; break;
        case 0x4b:
            CHECK(!f->cdb[1] && !f->cdb[7] && f->cdb[8]<=1);
            f->pause_packets++; f->pause_resume=f->cdb[8]; f->audio_status=f->cdb[8] ? 0x11 : 0x12; break;
        case 0x42: /* READ SUB-CHANNEL: LBA, SubQ, format 01h, 16 bytes */
            CHECK(!f->cdb[1] && f->cdb[2]==0x40 && f->cdb[3]==1 && !f->cdb[6] && f->cdb[8]==16);
            f->subchannel_packets++;
            f->bytes[1]=f->audio_status; f->bytes[3]=12; f->bytes[4]=1; f->bytes[5]=0x10;
            f->bytes[6]=f->sub_track; f->bytes[7]=f->sub_index;
            packet_be32_put(f->bytes+8,f->sub_abs); packet_be32_put(f->bytes+12,f->sub_rel);
            f->phases[0]=16; f->phase_count=1; break;
        case 0x28: {
            /* fx-hlis5: READ(10) of up to 32 blocks (ISO 9660 runs); a
             * multi-block transfer uses DRQ phases of whole blocks within
             * the host's byte count limit, as drives do. */
            uint32_t lba=(uint32_t)f->cdb[2]<<24 | (uint32_t)f->cdb[3]<<16 | (uint32_t)f->cdb[4]<<8 | f->cdb[5];
            uint32_t blocks=(uint32_t)f->cdb[7]<<8 | f->cdb[8];
            CHECK(blocks>=1 && blocks<=32);
            if (f->image) {
                CHECK(lba<f->image_blocks && blocks<=f->image_blocks-lba);
                CHECK(!fseek(f->image,(long)lba*2048,SEEK_SET) && fread(f->bytes,2048,blocks,f->image)==blocks);
                if (f->bad_iso && lba<=16 && 16<lba+blocks) memset(f->bytes+(16-lba)*2048,0,2048);
            } else {
                CHECK(!f->cdb[2] && !f->cdb[3] && !f->cdb[4] && f->cdb[5]==7);
                for (unsigned i=0;i<blocks*2048;i++) f->bytes[i]=packet_data_byte(i);
                if (blocks==1) { f->phases[0]=1024; f->phases[1]=1024; f->phase_count=2; break; }
            }
            unsigned per=f->host_limit/2048*2048; CHECK(per);
            if (f->read_phase_bytes) { CHECK(f->read_phase_bytes<=f->host_limit); per=f->read_phase_bytes; }
            f->phase_count=0;
            for (unsigned left=blocks*2048;left;) { unsigned n=left<per ? left : per; CHECK(f->phase_count<4); f->phases[f->phase_count++]=n; left-=n; }
            break;
        }
        default: CHECK(false);
        }
    }
    packet_next(f);
}
static uint64_t packet_now(void *ctx) { (void)ctx; return g_ticks; }
static uint64_t packet_deadline(void *ctx, uint64_t deadline) { (void)ctx; return deadline; }
static int packet_delay(void *ctx, uint32_t us) { (void)ctx; (void)us; return 0; }
static const struct ata_port_ops packet_ops={packet_in8,packet_out8,packet_in16,packet_out16,packet_now,packet_deadline,packet_delay,0,0};
/* fx-hlis5: the production port backend has string I/O (native_insw). */
static void packet_insw(void *ctx, uint16_t port, void *buf, uint32_t words)
{
    struct packet_fake *f=ctx;
    if (!f->packet_selected) { fake_insw(&f->disk,port,buf,words); return; }
    f->bursts++; if (words>f->burst_words) f->burst_words=words;
    for (uint32_t i=0;i<words;i++) fs_wr16((uint8_t *)buf+i*2,packet_in16(ctx,port));
}
static void packet_outsw(void *ctx, uint16_t port, const void *buf, uint32_t words)
{
    struct packet_fake *f=ctx;
    if (!f->packet_selected) { fake_outsw(&f->disk,port,buf,words); return; }
    for (uint32_t i=0;i<words;i++) packet_out16(ctx,port,fs_rd16((const uint8_t *)buf+i*2));
}
static const struct ata_port_ops packet_burst_ops={packet_in8,packet_out8,packet_in16,packet_out16,packet_now,packet_deadline,packet_delay,
                                                    packet_insw,packet_outsw};
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
/* fx-hlis5: with string I/O the data phases move as bursts of at most 1024
 * words (2048 bytes): odd, short and multi-phase transfers keep their bytes
 * and canaries; a 32-block READ(10) crosses the byte-count limit. */
static void test_packet_bursts(void)
{
    struct packet_fake f; uint8_t p[12]={0x12}, data[65536+2]; uint32_t done;
    packet_fresh(&f,12); f.disk.channel.ops=&packet_burst_ops;
    for (unsigned i=0;i<sizeof(f.bytes);i++) f.bytes[i]=(uint8_t)(i*13);
    f.phases[0]=2; f.phases[1]=4; f.phases[2]=3; f.phase_count=3;
    memset(data,0xa5,sizeof(data)); on_yield=packet_yield_check;
    CHECK(!atapi_packet(&f.disk.channel.devices[1],p,sizeof(p),data+1,9,&done));
    on_yield=0; CHECK(done==9 && data[0]==0xa5 && data[10]==0xa5 && !memcmp(data+1,f.bytes,9));
    CHECK(f.bursts==3 && f.burst_words==2);
    packet_fresh(&f,12); f.disk.channel.ops=&packet_burst_ops; f.responses=true;
    struct ata_device *d=&f.disk.channel.devices[1]; uint8_t peer[512];
    CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block,0,1,peer));
    CHECK(!atapi_ready(d) && d->media==ATAPI_READY);
    unsigned issued=f.packets; f.bursts=f.burst_words=0; memset(data,0xa5,sizeof(data)); on_yield=packet_yield_check;
    CHECK(!d->block.read(&d->block,7,16,data+1)); on_yield=0;
    CHECK(f.packets==issued+1 && f.cdb[0]==0x28 && f.cdb[8]==16 && data[0]==0xa5 && data[32769]==0xa5);
    for (unsigned i=0;i<32768;i++) if (data[1+i]!=packet_data_byte(i)) { CHECK(!"burst data"); break; }
    CHECK(f.bursts==16 && f.burst_words==1024 && d->read_blocks>=16);
    /* 64 KiB cannot be encoded as one DRQ count. Test both the maximum
     * legal phase (0xfffe + 2) and three smaller phases; neither the
     * host limit nor offsets may wrap at 16 bits (ATA/ATAPI-6 8.23.4–5). */
    const unsigned phase_sizes[]={0xfffe,32766};
    for (unsigned test=0;test<2;test++) {
        unsigned per=phase_sizes[test];
        f.read_phase_bytes=per; issued=f.packets; f.bursts=f.burst_words=0;
        memset(data,0xa5,sizeof(data)); on_yield=packet_yield_check;
        CHECK(!d->block.read(&d->block,7,32,data+1)); on_yield=0;
        CHECK(f.packets==issued+1 && f.cdb[0]==0x28 && f.cdb[7]==0 && f.cdb[8]==32);
        CHECK(f.host_limit==0xfffe && f.phase_count==(per==0xfffe ? 2u : 3u) && f.phase_index==f.phase_count);
        CHECK(data[0]==0xa5 && data[65537]==0xa5 && f.burst_words==1024);
        CHECK(f.bursts==33);
        for (unsigned i=0;i<65536;i++) if (data[1+i]!=packet_data_byte(i)) { CHECK(!"64 KiB burst data"); break; }
    }
    /* A completed phase followed by CHECK CONDITION must report no valid
     * transfer, even though the transport buffer contains partial bytes. */
    f.read_phase_bytes=32766; f.fail_after_phase=1; f.sense_key=3; f.asc=0x11;
    uint8_t read32[12]={0x28,0,0,0,0,7,0,0,32};
    memset(data,0xa5,sizeof(data)); done=123; issued=f.packets;
    CHECK(atapi_packet(d,read32,sizeof(read32),data+1,65536,&done)==-FS_EIO);
    CHECK(!done && f.failed_at==32766 && f.phase_errors==1 && f.packets==issued+2);
    for (unsigned i=0;i<32766;i++) if (data[1+i]!=packet_data_byte(i)) { CHECK(!"partial phase data"); break; }
    CHECK(data[0]==0xa5 && data[32767]==0xa5 && data[65537]==0xa5);
    CHECK(!d->block.quarantined && !f.disk.channel.stopped);
    CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block,0,1,peer));
    puts("PASS fx-hlis5 ATAPI string-I/O bursts: odd/multi-phase canaries, 16/32-block READ(10), 0xfffe boundary, post-DRQ error with done=0, peer intact");
}
/* Exercise cache publication on the production packet transport, after one
 * successful DRQ. No scheduling timing or second thread is required. */
static struct {
    struct block_cache *cache;
    struct cache_block *reserved[32];
    unsigned mode, count, captured, callbacks;
} packet_fill;
static unsigned packet_cached_blocks(struct block_cache *c,struct blkdev *d)
{
    unsigned count=0; /* Caller holds the cache lock. */
    for (unsigned i=0;i<cache_hash_buckets(c);i++)
        for (struct cache_block *b=*cache_hash_bucket(c,i);b;b=b->hash_next)
            if (b->dev==d) count++;
    return count;
}
static void packet_fill_phase(struct packet_fake *f)
{
    struct block_cache *c=packet_fill.cache;
    struct blkdev *d=&f->disk.channel.devices[f->packet_unit].block;
    packet_fill.callbacks++;
    CHECK(f->phase_index==1 && f->pos && f->pos<packet_fill.count*2048);
    fs_lock_take(&c->lock);
    CHECK(packet_cached_blocks(c,d)==packet_fill.count);
    for (unsigned i=0;i<cache_hash_buckets(c);i++)
        for (struct cache_block *b=*cache_hash_bucket(c,i);b;b=b->hash_next) if (b->dev==d) {
            CHECK(b->busy && b->sector_size==2048 && b->capacity==2048);
            CHECK(packet_fill.captured<32);
            if (packet_fill.captured<32) packet_fill.reserved[packet_fill.captured++]=b;
        }
    fs_lock_drop(&c->lock);
    uint8_t sector[2048]; memset(sector,0xa5,sizeof(sector));
    CHECK(cache_peek(c,d,7,sector)==-FS_EAGAIN);
    for (unsigned i=0;i<sizeof(sector);i++) if (sector[i]!=0xa5) { CHECK(!"busy fill published"); break; }
    if (packet_fill.mode==2 || packet_fill.mode==4) d->media_generation++;
    if (packet_fill.mode==3 || packet_fill.mode==5) cache_invalidate(c,d,false);
}
static void test_packet_cache_fills(void)
{
    /* First prove 64 KiB read-ahead really issues one 32-sector command,
     * then fail it after phase one. Also invalidate both run and single-
     * sector (2048-byte) fills by media generation and by cache epoch. */
    for (unsigned mode=0;mode<6;mode++) {
        struct packet_fake f; packet_fresh(&f,12);
        f.disk.channel.ops=&packet_burst_ops; f.responses=true;
        struct ata_device *d=&f.disk.channel.devices[1];
        CHECK(!atapi_ready(d));
        struct block_cache c; CHECK(!cache_init(&c,2u*1024*1024));
        CHECK(!cache_attach(&c,&d->block) && c.slots[1]/2>=32);
        unsigned count=mode<2 ? 32 : mode<4 ? 2 : 1;
        memset(&packet_fill,0,sizeof(packet_fill));
        packet_fill.cache=&c; packet_fill.mode=mode; packet_fill.count=count;
        f.read_phase_bytes=count==32 ? 32766 : 2048;
        if (mode) f.phase_complete=packet_fill_phase;
        if (mode==1) { f.fail_after_phase=1; f.sense_key=3; f.asc=0x11; }
        uint32_t gen=d->block.media_generation, epoch=c.device_epoch;
        unsigned issued=f.packets; uint64_t blocks=d->read_blocks;
        uint8_t demand[2048+2]; memset(demand,0xa5,sizeof(demand));
        CHECK(cache_read_ahead(&c,&d->block,7,count,demand+1)==(mode ? -FS_EIO : 0));
        CHECK(demand[0]==0xa5 && demand[2049]==0xa5);
        if (!mode) {
            CHECK(f.host_limit==0xfffe && f.cdb[8]==32 && f.phase_count==3);
            CHECK(f.packets==issued+1 && d->read_blocks==blocks+32);
            for (unsigned i=0;i<2048;i++) CHECK(demand[i+1]==packet_data_byte(i));
            uint8_t sector[2048];
            for (unsigned block=0;block<32;block++) {
                CHECK(!cache_peek(&c,&d->block,7+block,sector));
                for (unsigned i=0;i<2048;i++) if (sector[i]!=packet_data_byte(block*2048+i)) { CHECK(!"cached run data"); break; }
            }
        } else {
            CHECK(packet_fill.callbacks==1 && packet_fill.captured==count && !c.fills);
            CHECK(f.packets==issued+(mode==1 ? 2u : 1u));
            if (mode==1) CHECK(f.phase_errors==1 && f.failed_at==32766 && d->read_blocks==blocks);
            CHECK(d->block.media_generation==gen+((mode==2 || mode==4) ? 1u : 0u));
            CHECK((c.device_epoch!=epoch)==(mode==3 || mode==5));
            for (unsigned i=0;i<sizeof(demand);i++) if (demand[i]!=0xa5) { CHECK(!"failed demand changed"); break; }
            fs_lock_take(&c.lock); CHECK(!packet_cached_blocks(&c,&d->block));
            for (unsigned bucket=0;bucket<cache_hash_buckets(&c);bucket++)
                for (struct cache_block *b=*cache_hash_bucket(&c,bucket);b;b=b->hash_next)
                    for (unsigned i=0;i<packet_fill.captured;i++) CHECK(b!=packet_fill.reserved[i]);
            fs_lock_drop(&c.lock);
            for (unsigned i=0;i<packet_fill.captured;i++) {
                struct cache_block *b=packet_fill.reserved[i];
                CHECK(!b->dev && !b->hash_next && !b->busy && !b->dirty && !b->pins);
            }
            uint8_t sector[2048]; memset(sector,0xa5,sizeof(sector));
            for (unsigned block=0;block<count;block++) CHECK(cache_peek(&c,&d->block,7+block,sector)==-FS_EAGAIN);
            for (unsigned i=0;i<sizeof(sector);i++) if (sector[i]!=0xa5) { CHECK(!"failed cache data published"); break; }
            /* New/current generation must perform fresh I/O, then cache it. */
            issued=f.packets;
            CHECK(!cache_read_ahead(&c,&d->block,7,count,demand+1) && f.packets==issued+1);
            CHECK(demand[0]==0xa5 && demand[2049]==0xa5);
            for (unsigned i=0;i<2048;i++) CHECK(demand[i+1]==packet_data_byte(i));
        }
        CHECK(!d->block.quarantined && !f.disk.channel.stopped);
        uint8_t peer[512]; CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block,0,1,peer));
        cache_destroy(&c);
    }
    memset(&packet_fill,0,sizeof(packet_fill));
    puts("PASS fx-hlis5 optical cache fills: 64 KiB/three DRQs, post-DRQ error, demand untouched, reservations unhashed, media/epoch invalidation of 2048-byte and run fills, fresh retry");
}
static struct storage optical_store;
static struct vfs_table optical_handles;
static void check_eject_unmounted(void)
{
    for (unsigned slot=0;slot<STORAGE_DISKS;slot++) CHECK(!optical_store.optical[slot].mounted);
    for (unsigned drive=3;drive<26;drive++) CHECK(!optical_store.vfs.ro_volumes[drive]);
}
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
        c->index=1;
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
        captured_logs[0]=0; capture_logs=true;
        optical_service(&optical_store,0);
        CHECK(d->media==ATAPI_NO_MEDIUM && !optical_store.optical[slot].mounted && !vfs_drive_present(&optical_store.vfs,3));
        /* fx-emptydrive: the empty drive owns D: from its first pass, after
         * the hard-disk letters; paths on it are not ready, not missing. */
        CHECK(optical_store.optical_bindings[slot].drive==3 && vfs_drive_slot(&optical_store.vfs,3)==VFS_SLOT_OPTICAL);
        CHECK(vfs_letter_taken(&optical_store.vfs,3) && optical_store.next_drive==4);
        CHECK(strstr(captured_logs,"stage=letter drive=D result=0") && strstr(captured_logs,"drive=D mode=ro") && strstr(captured_logs,"reason=no_medium"));
        capture_logs=false;
        {
            struct vfs_table probe; CHECK(!vfs_table_init(&optical_store.vfs,&probe,2));
            CHECK(vfs_open(&probe,"D:/README.TXT",VFS_READ,VFS_DENY_NONE,0)==-FS_ENXIO);
            CHECK(vfs_open(&probe,"E:/README.TXT",VFS_READ,VFS_DENY_NONE,0)==-FS_ENOENT);
            vfs_table_destroy(&probe);
        }
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
        CHECK(optical_store.optical_bindings[slot].drive==3 && vfs_drive_slot(&optical_store.vfs,3)==VFS_SLOT_OPTICAL); /* kept */
        CHECK(vfs_read(&optical_handles,h,bytes,1,&count)==-FS_EIO && !count);
        f.no_medium=false; optical_service(&optical_store,3000);
        CHECK(vfs_drive_present(&optical_store.vfs,3));
        CHECK(vfs_read(&optical_handles,h,bytes,1,&count)==-FS_EIO && !count);
        CHECK(!vfs_close(&optical_handles,h));
        f.eject_check=check_eject_unmounted;
        CHECK(!optical_eject(&optical_store,3,d->block.media_generation) && !vfs_drive_present(&optical_store.vfs,3));
        f.eject_check=0;
        CHECK(vfs_drive_slot(&optical_store.vfs,3)==VFS_SLOT_OPTICAL); /* the letter outlives the eject */
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
    /* fx-emptydrive: hard-disk volumes C:, D: and E: (next_drive 5): the
     * empty CD-ROM drive takes F:, and a disc inserted later mounts there. */
    {
        struct packet_fake f; packet_fresh(&f,12);
        struct ata_device *d=&f.disk.channel.devices[1];
        f.responses=true; f.image=image; f.image_blocks=(uint32_t)(size/2048); f.no_medium=true;
        CHECK(!storage_setup(&optical_store,32768));
        optical_store.next_drive=5;
        CHECK(!optical_bind(&optical_store,1,d));
        optical_service(&optical_store,0);
        CHECK(optical_store.optical_bindings[1].drive==5 && optical_store.next_drive==6 && vfs_drive_slot(&optical_store.vfs,5)==VFS_SLOT_OPTICAL);
        f.no_medium=false; f.attentions=1; optical_service(&optical_store,1000);
        CHECK(optical_store.vfs.ro_volumes[5]==&optical_store.optical[1] && !vfs_drive_present(&optical_store.vfs,3));
        storage_destroy(&optical_store);
    }
    fclose(image);
    puts("PASS optical lifecycle: secondary master/slave, empty boot, 1s insertion polling, reset attention, Joliet reads, EROFS, busy/stale eject, removal/stale handles, reinsertion, zero writes/PIO");
    puts("PASS fx-emptydrive optical letters: empty drive owns D: (after C:) or F: (after three hard-disk letters) from its first pass, record names it, D: paths not ready (FS_ENXIO), letter kept after removal and eject, disc mounts at the owned letter");
}

/* f5-12B2: CD-audio packet paths (optical_cd.c) on the same packet fake:
 * TOC cache and track typing, page 2Ah capability bits, READ CD CDB and
 * data, range refusals before any packet, PLAY AUDIO MSF / PAUSE / READ
 * SUB-CHANNEL, injected sense, audio-only readiness without a mount. */
static void cd_mixed_toc(struct packet_fake *f)
{
    f->toc_count=3; f->toc_leadout=9000;
    f->toc_control[0]=4; f->toc_start[0]=0;      /* data track 1 */
    f->toc_control[1]=0; f->toc_start[1]=1000;   /* audio track 2 */
    f->toc_control[2]=2; f->toc_start[2]=5000;   /* audio track 3, copy permitted */
}
static void cd_caps_all(struct packet_fake *f)
{ memset(f->caps,0,sizeof(f->caps)); f->caps[4]=0x01; f->caps[5]=0x03; f->caps[6]=0x29; f->caps[7]=0x01; f->caps[10]=1; f->caps[11]=0; }
static void test_cd_audio(void)
{
    struct packet_fake f; static uint8_t audio[27*2352];
    uint8_t status,track,index; uint32_t absolute,relative;
    /* Conversions (MMC-3 4.2): LBA 0 is 00:02:00, 1000 is 00:15:25, 5150 is 01:10:50. */
    uint8_t msf[3]; atapi_lba_to_msf(0,msf); CHECK(msf[0]==0 && msf[1]==2 && msf[2]==0);
    atapi_lba_to_msf(1000,msf); CHECK(msf[0]==0 && msf[1]==15 && msf[2]==25 && atapi_msf_to_lba(msf)==1000);
    atapi_lba_to_msf(5150,msf); CHECK(msf[0]==1 && msf[1]==10 && msf[2]==50 && atapi_msf_to_lba(msf)==5150);
    atapi_lba_to_msf(333000,msf); CHECK(msf[0]==74 && msf[1]==2 && msf[2]==0);

    /* Data-only disc (the F4 default): ready as before, CD audio refused
     * before any packet, capabilities fetched once. */
    packet_fresh(&f,12); f.responses=true; cd_caps_all(&f);
    struct ata_device *d=&f.disk.channel.devices[1];
    CHECK(!atapi_ready(d) && d->media==ATAPI_READY && f.packets==5 && f.toc_packets==1);
    CHECK(d->toc.valid && d->toc.count==1 && !d->toc.first_audio && !d->toc.audio_only && d->toc.leadout==64 && !d->audio_only);
    unsigned issued=f.packets;
    CHECK(atapi_cd_read_audio(d,d->block.media_generation,0,1,audio)==-FS_EINVAL && f.packets==issued);
    CHECK(atapi_cd_play(d,d->block.media_generation,0,10)==-FS_EINVAL && f.packets==issued);
    CHECK(atapi_cd_read_audio(d,d->block.media_generation+1,0,1,audio)==-FS_EAGAIN && f.packets==issued);
    capture_logs=true; captured_logs[0]=0;
    CHECK(!atapi_cd_caps(d) && f.caps_packets==1 && d->cd_caps_valid && (d->cd_caps[5]&3)==3 && (d->cd_caps[4]&1));
    CHECK(!atapi_cd_caps(d) && f.caps_packets==1); /* cached per drive */
    CHECK(!atapi_cd_toc(d,false) && f.toc_packets==1); /* cached per generation */
    CHECK(strstr(captured_logs,"stage=caps audio_play=1 cdda_read=1 cdda_accurate=1 eject=1 volume_levels=256"));
    capture_logs=false;

    /* Mixed-mode disc: data track 1 then two audio tracks. */
    packet_fresh(&f,12); f.responses=true; cd_caps_all(&f); cd_mixed_toc(&f);
    d=&f.disk.channel.devices[1];
    CHECK(!atapi_ready(d) && d->media==ATAPI_READY && d->toc.count==3 && !d->toc.first_audio && !d->toc.audio_only);
    CHECK(d->toc.start[1]==1000 && d->toc.control[2]==2 && d->toc.leadout==9000 && d->session_lba==0);
    uint32_t gen=d->block.media_generation; issued=f.packets;
    /* Refused before any packet: data track, straddling into data, beyond
     * the lead-out, before track 1, zero or oversize counts, stale generation. */
    CHECK(atapi_cd_read_audio(d,gen,999,2,audio)==-FS_EINVAL);
    CHECK(atapi_cd_read_audio(d,gen,500,1,audio)==-FS_EINVAL);
    CHECK(atapi_cd_read_audio(d,gen,8990,27,audio)==-FS_EINVAL);
    CHECK(atapi_cd_read_audio(d,gen,9000,1,audio)==-FS_EINVAL);
    CHECK(atapi_cd_read_audio(d,gen,1000,0,audio)==-FS_EINVAL);
    CHECK(atapi_cd_read_audio(d,gen,1000,28,audio)==-FS_EINVAL);
    CHECK(atapi_cd_read_audio(d,gen,1000,1,0)==-FS_EINVAL);
    CHECK(atapi_cd_read_audio(d,gen+1,1000,1,audio)==-FS_EAGAIN);
    CHECK(atapi_cd_play(d,gen,900,1100)==-FS_EINVAL && atapi_cd_play(d,gen,1000,1000)==-FS_EINVAL && atapi_cd_play(d,gen,1000,9001)==-FS_EINVAL);
    CHECK(f.packets==issued && !f.read_cd_packets && !f.play_packets);
    /* One 27-sector READ CD: CDB and the 63,504-byte CD-DA stream. */
    memset(audio,0xee,sizeof(audio)); on_yield=packet_yield_check;
    CHECK(!atapi_cd_read_audio(d,gen,1000,27,audio)); on_yield=0;
    CHECK(f.read_cd_packets==1 && f.read_cd_lba==1000 && f.read_cd_sectors==27 && f.packets==issued+1);
    bool same=true; for (uint32_t i=0;i<27*2352;i++) if (audio[i]!=packet_cdda_byte(1000+i/2352,i%2352)) same=false;
    CHECK(same && d->cd_audio_sectors==27 && d->cd_audio_packets==1);
    /* Crossing from one audio track into the next audio track is allowed. */
    CHECK(!atapi_cd_read_audio(d,gen,4990,27,audio) && f.read_cd_lba==4990 && f.read_cd_sectors==27);
    CHECK(!atapi_cd_read_audio(d,gen,8999,1,audio) && f.read_cd_sectors==1);
    /* The ATA peer still answers after CD traffic. */
    uint8_t peer[512]; CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block,0,1,peer));
    /* Analog path: PLAY AUDIO MSF absolute addresses, pause/resume, position. */
    CHECK(!atapi_cd_play(d,gen,1000,5150) && f.play_packets==1 && d->cd_play_commands==1);
    CHECK(f.play_start[0]==0 && f.play_start[1]==15 && f.play_start[2]==25 && f.play_end[0]==1 && f.play_end[1]==10 && f.play_end[2]==50);
    f.sub_track=2; f.sub_index=1; f.sub_abs=1300; f.sub_rel=300;
    CHECK(!atapi_cd_position(d,gen,&status,&track,&index,&absolute,&relative) && f.subchannel_packets==1);
    CHECK(status==0x11 && track==2 && index==1 && absolute==1300 && relative==300);
    CHECK(!atapi_cd_pause(d,gen,false) && f.pause_packets==1 && f.pause_resume==0);
    CHECK(!atapi_cd_position(d,gen,&status,&track,&index,&absolute,&relative) && status==0x12);
    CHECK(!atapi_cd_pause(d,gen,true) && f.pause_packets==2 && f.pause_resume==1);
    CHECK(atapi_cd_pause(d,gen+1,true)==-FS_EAGAIN && atapi_cd_position(d,gen+1,&status,&track,&index,&absolute,&relative)==-FS_EAGAIN);
    /* STOP PLAY/SCAN 4Eh (MMC-3 5.37): all bytes but the opcode zero; stale generation refused before a packet. */
    CHECK(!atapi_cd_stop(d,gen) && f.stop_packets==1 && f.cdb[0]==0x4e);
    CHECK(!atapi_cd_position(d,gen,&status,&track,&index,&absolute,&relative) && status==0x13);
    issued=f.packets; CHECK(atapi_cd_stop(d,gen+1)==-FS_EAGAIN && f.packets==issued);
    /* Injected ILLEGAL REQUEST on READ CD: EIO for that request only, the
     * generation and the peer untouched, nothing retried. */
    f.read_cd_fail=true; f.sense_key=5; f.asc=0x64; f.ascq=0; issued=f.packets;
    CHECK(atapi_cd_read_audio(d,gen,1000,4,audio)==-FS_EIO && f.packets==issued+2 && d->block.media_generation==gen);
    CHECK(d->sense_key==5 && d->asc==0x64 && !d->block.quarantined && !f.disk.channel.stopped);
    CHECK(!f.disk.channel.devices[0].block.read(&f.disk.channel.devices[0].block,0,1,peer));
    /* MEDIUM NOT PRESENT mid-stream: the generation moves, the TOC expires. */
    f.sense_key=2; f.asc=0x3a;
    CHECK(atapi_cd_read_audio(d,gen,1000,4,audio)==-FS_EIO && d->media==ATAPI_NO_MEDIUM && d->block.media_generation>gen && !d->toc.valid);
    CHECK(atapi_cd_read_audio(d,gen,1000,4,audio)==-FS_ENOENT); /* not ready: refused before a packet */
    f.read_cd_fail=false;
    /* A drive without CD-DA commands refuses the digital path; one without
     * Audio Play refuses the analog one; both before any packet. */
    packet_fresh(&f,12); f.responses=true; cd_caps_all(&f); cd_mixed_toc(&f); f.caps[5]=0; d=&f.disk.channel.devices[1];
    CHECK(!atapi_ready(d) && !atapi_cd_caps(d)); gen=d->block.media_generation; issued=f.packets;
    CHECK(atapi_cd_read_audio(d,gen,1000,1,audio)==-FS_EOPNOTSUPP && f.packets==issued);
    CHECK(!atapi_cd_play(d,gen,1000,2000) && f.play_packets==1);
    packet_fresh(&f,12); f.responses=true; cd_caps_all(&f); cd_mixed_toc(&f); f.caps[4]=0; d=&f.disk.channel.devices[1];
    CHECK(!atapi_ready(d) && !atapi_cd_caps(d)); gen=d->block.media_generation; issued=f.packets;
    CHECK(atapi_cd_play(d,gen,1000,2000)==-FS_EOPNOTSUPP && f.packets==issued);
    CHECK(!atapi_cd_read_audio(d,gen,1000,1,audio));
    /* A drive that rejects MODE SENSE leaves the bits unknown: both paths stay open. */
    packet_fresh(&f,12); f.responses=true; cd_mixed_toc(&f); f.caps_fail=true; f.sense_key=5; f.asc=0x24; d=&f.disk.channel.devices[1];
    CHECK(!atapi_ready(d)); gen=d->block.media_generation;
    CHECK(atapi_cd_caps(d)==-FS_EIO && !d->cd_caps_valid && !d->block.quarantined);
    CHECK(!atapi_cd_read_audio(d,gen,1000,1,audio) && !atapi_cd_play(d,gen,1000,2000));

    /* Drives that refuse READ CAPACITY on CD-DA (ILLEGAL REQUEST / MEDIUM ERROR) are still
     * ready when the TOC says audio; a data disc keeps the F4 refusal; a missing
     * medium or a transport error never reaches the TOC. */
    packet_fresh(&f,12); f.responses=true; cd_caps_all(&f);
    f.toc_count=2; f.toc_leadout=20000; f.toc_control[0]=0; f.toc_start[0]=0; f.toc_control[1]=0; f.toc_start[1]=12000;
    f.capacity_fail=true; f.sense_key=5; f.asc=0x30; f.ascq=2; d=&f.disk.channel.devices[1];
    CHECK(!atapi_ready(d) && d->media==ATAPI_READY && d->toc.first_audio && d->block.capacity==20000 && d->session_lba==0);
    issued=f.packets; CHECK(!atapi_ready(d) && f.packets==issued+1); /* repeat poll: the test-unit-ready only */
    packet_fresh(&f,12); f.responses=true; cd_caps_all(&f); cd_mixed_toc(&f);
    f.capacity_fail=true; f.sense_key=3; f.asc=0x30; d=&f.disk.channel.devices[1]; capture_logs=true; captured_logs[0]=0;
    CHECK(atapi_ready(d)==-FS_EIO && d->media!=ATAPI_READY && !d->block.capacity && f.toc_packets==1);
    CHECK(strstr(captured_logs,"stage=capacity result=-5") && !strstr(captured_logs,"stage=toc"));
    unsigned toc_seen=f.toc_packets; captured_logs[0]=0;
    CHECK(atapi_ready(d)==-FS_EIO && f.toc_packets==toc_seen+1);
    CHECK(!strstr(captured_logs,"stage=toc")); /* the refusal is logged as F4 does, never as a TOC result */
    capture_logs=false;
    packet_fresh(&f,12); f.responses=true; cd_mixed_toc(&f);
    f.capacity_fail=true; f.sense_key=2; f.asc=0x3a; d=&f.disk.channel.devices[1];
    CHECK(atapi_ready(d)==-FS_EIO && d->media==ATAPI_NO_MEDIUM && !f.toc_packets);
    packet_fresh(&f,12); f.responses=true; cd_mixed_toc(&f);
    f.capacity_fail=true; f.sense_key=4; f.asc=0x44; d=&f.disk.channel.devices[1]; /* hardware failure: not a CD-DA refusal */
    CHECK(atapi_ready(d)==-FS_EIO && !f.toc_packets && d->media!=ATAPI_READY);

    /* Audio-only disc: ready for CD audio, never mounted, no ISO probe. */
    packet_fresh(&f,12); f.responses=true; cd_caps_all(&f);
    f.toc_count=2; f.toc_leadout=20000; f.toc_control[0]=0; f.toc_start[0]=0; f.toc_control[1]=0; f.toc_start[1]=12000;
    d=&f.disk.channel.devices[1]; capture_logs=true; captured_logs[0]=0;
    CHECK(!atapi_ready(d) && d->media==ATAPI_READY && d->toc.first_audio && d->toc.audio_only && d->audio_only && f.packets==4);
    CHECK(strstr(captured_logs,"stage=audio result=0"));
    gen=d->block.media_generation;
    CHECK(!atapi_cd_read_audio(d,gen,0,27,audio) && f.read_cd_lba==0);
    CHECK(!atapi_cd_read_audio(d,gen,19999,1,audio) && atapi_cd_read_audio(d,gen,19999,2,audio)==-FS_EINVAL);
    CHECK(!storage_setup(&optical_store,32768));
    CHECK(!optical_bind(&optical_store,3,d));
    issued=f.packets; captured_logs[0]=0;
    unsigned notifications=media_notifications;
    uint32_t namespace_before[26];memcpy(namespace_before,optical_store.vfs.generation,sizeof(namespace_before));
    optical_service(&optical_store,0);
    CHECK(media_notifications==notifications+1);
    CHECK(!optical_store.optical[3].mounted && !vfs_drive_present(&optical_store.vfs,3) && !optical_store.optical[3].state);
    CHECK(f.packets==issued+1 && strstr(captured_logs,"reason=audio_cd") && strstr(captured_logs,"stage=audio generation="));
    issued=f.packets; captured_logs[0]=0; optical_service(&optical_store,1000);
    CHECK(f.packets==issued+1 && !strstr(captured_logs,"stage=iso")); /* readiness only, once per second */
    CHECK(media_notifications==notifications+1); /* unchanged CD: no extra wake */
    /* The disc goes away and comes back: a new generation, still no mount. */
    f.no_medium=true; optical_service(&optical_store,2000);
    CHECK(media_notifications==notifications+2);
    CHECK(d->media==ATAPI_NO_MEDIUM && d->block.media_generation>gen && !d->toc.valid);
    f.no_medium=false; optical_service(&optical_store,3000);
    CHECK(media_notifications==notifications+3);
    CHECK(!memcmp(namespace_before,optical_store.vfs.generation,sizeof(namespace_before)));
    CHECK(d->media==ATAPI_READY && d->toc.first_audio && !optical_store.optical[3].mounted && !vfs_drive_present(&optical_store.vfs,3));
    CHECK(!atapi_eject(d) && d->media==ATAPI_NO_MEDIUM);
    CHECK(!d->media_writes && !d->block.quarantined && !f.disk.channel.stopped);
    capture_logs=false; storage_destroy(&optical_store);
    /* Both packet lengths, read/streaming errors, unit attention, timeout.
     * No failed READ CD is retried; REQUEST SENSE is the only extra packet. */
    for (unsigned format=0;format<2;format++) {
        packet_fresh(&f,format ? 16 : 12); f.responses=true; cd_caps_all(&f); cd_mixed_toc(&f);
        d=&f.disk.channel.devices[1]; CHECK(!atapi_ready(d)); gen=d->block.media_generation;
        CHECK(!atapi_cd_read_audio(d,gen,1000,1,audio) && f.packet_length==(format ? 16u : 12u));
        for (unsigned fault=0;fault<2;fault++) {
            f.read_cd_fail=true; f.sense_key=fault ? 0xb : 3; f.asc=0x11; f.ascq=fault ? 5 : 0;
            issued=f.packets; CHECK(atapi_cd_read_audio(d,gen,1000,1,audio)==-FS_EIO && f.packets==issued+2);
            CHECK(!d->block.quarantined && d->block.media_generation==gen);
        }
        f.sense_key=6; f.asc=0x28; f.ascq=0; issued=f.packets;
        CHECK(atapi_cd_read_audio(d,gen,1000,1,audio)==-FS_EIO && f.packets==issued+2 && !d->toc.valid);
        CHECK(d->block.media_generation!=gen && !d->block.quarantined);
        f.read_cd_fail=false; CHECK(!atapi_ready(d)); gen=d->block.media_generation;
        f.timeout=true;
        CHECK(atapi_cd_read_audio(d,gen,1000,1,audio)<0 && d->block.quarantined && f.disk.channel.stopped);
    }
    puts("PASS CD audio packets: TOC cache/types, page 2Ah bits, READ CD CDB/stream (27 sectors), refusals before any packet, PLAY AUDIO MSF/PAUSE/SUB-CHANNEL, injected sense, capability refusals, audio-only readiness without a mount");
}

/* f5-12B2: the /dev/media CD-audio devctls (media_cd.c) over the same packet
 * fake: record validation, STATE/INFO/TOC, READ_AUDIO with the transient
 * bounce run and user-copy faults, the analog commands, tray, errno mapping. */
static int cd_ctl(struct process *proc, uint32_t op, void *record, uint32_t bytes)
{
    memcpy(cd_user + 256, record, bytes);
    cd_user_calls = 0;
    int e = media_cd_devctl(proc, &optical_store, op, CD_USER_BASE + 256, bytes);
    cd_user_fail_at = 0;
    if (!e) memcpy(record, cd_user + 256, bytes);
    return e;
}
static void test_media_cd(void)
{
    struct packet_fake f; struct process proc; memset(&proc, 0, sizeof(proc));
    packet_fresh(&f, 12); f.responses = true; cd_caps_all(&f); cd_mixed_toc(&f);
    struct ata_device *d = &f.disk.channel.devices[1];
    CHECK(!storage_setup(&optical_store, 32768) && !optical_bind(&optical_store, 1, d));
    CHECK(!atapi_ready(d)); uint32_t gen = d->block.media_generation;
    struct ciuki_media_cd_state st = { .version = 1, .size = sizeof(st), .slot = 1 };
    /* Record validation: version, size, slot; unknown operation. */
    CHECK(cd_ctl(&proc, MEDIA_CD_STATE, &st, sizeof(st)) == 0 && st.present && st.state == CIUKI_CD_READY && st.generation == gen);
    CHECK(!st.audio_ready && !st.mounted && st.drive == 26); /* mixed-mode: first track is data */
    st = (struct ciuki_media_cd_state){ .version = 2, .size = sizeof(st), .slot = 1 };
    CHECK(cd_ctl(&proc, MEDIA_CD_STATE, &st, sizeof(st)) == -EOPNOTSUPP);
    st = (struct ciuki_media_cd_state){ .version = 1, .size = sizeof(st) - 4, .slot = 1 };
    CHECK(cd_ctl(&proc, MEDIA_CD_STATE, &st, sizeof(st)) == -EINVAL);
    st = (struct ciuki_media_cd_state){ .version = 1, .size = sizeof(st), .slot = STORAGE_DISKS };
    CHECK(cd_ctl(&proc, MEDIA_CD_STATE, &st, sizeof(st)) == -EINVAL);
    st = (struct ciuki_media_cd_state){ .version = 1, .size = sizeof(st), .slot = 0 };
    CHECK(cd_ctl(&proc, MEDIA_CD_STATE, &st, sizeof(st)) == 0 && !st.present && st.drive == 26);
    CHECK(media_cd_devctl(&proc, &optical_store, 0x4d1fu, CD_USER_BASE, 4) == -ENOTTY);
    struct ciuki_media_cd_info in = { .version = 1, .size = sizeof(in), .slot = 0 };
    CHECK(cd_ctl(&proc, MEDIA_CD_INFO, &in, sizeof(in)) == -ENODEV);
    in.slot = 1; unsigned issued = f.packets;
    CHECK(cd_ctl(&proc, MEDIA_CD_INFO, &in, sizeof(in)) == 0 && f.caps_packets == 1 && f.packets == issued + 1);
    CHECK(in.capabilities == (CIUKI_CD_CAP_REPORTED | CIUKI_CD_CAP_AUDIO_PLAY | CIUKI_CD_CAP_CDDA_READ | CIUKI_CD_CAP_CDDA_ACCURATE |
                              CIUKI_CD_CAP_EJECT | CIUKI_CD_CAP_SEPARATE_VOLUME));
    CHECK(in.volume_levels == 256 && in.packet_bytes == 12 && in.max_read_sectors == 27 && in.generation == gen && in.model[0]);
    CHECK(cd_ctl(&proc, MEDIA_CD_INFO, &in, sizeof(in)) == 0 && f.caps_packets == 1); /* cached per drive */
    /* TOC through the cache: no packet; a stale generation is EAGAIN. */
    static struct ciuki_media_cd_toc toc;
    toc = (struct ciuki_media_cd_toc){ .version = 1, .size = sizeof(toc), .slot = 1 };
    issued = f.packets; CHECK(cd_ctl(&proc, MEDIA_CD_TOC, &toc, sizeof(toc)) == 0 && f.packets == issued);
    CHECK(toc.count == 3 && toc.first == 1 && toc.last == 3 && toc.leadout_lba == 9000 && toc.generation == gen);
    CHECK(toc.tracks[0].number == 1 && toc.tracks[0].control == 4 && toc.tracks[0].start_lba == 0);
    CHECK(toc.tracks[1].number == 2 && toc.tracks[1].control == 0 && toc.tracks[1].adr == 1 && toc.tracks[1].start_lba == 1000);
    CHECK(toc.tracks[2].control == 2 && toc.tracks[2].start_lba == 5000);
    toc = (struct ciuki_media_cd_toc){ .version = 1, .size = sizeof(toc), .slot = 1, .generation = gen + 1 };
    CHECK(cd_ctl(&proc, MEDIA_CD_TOC, &toc, sizeof(toc)) == -EAGAIN);

    /* READ_AUDIO: one bounce run per call, freed on every path. */
    cd_bounce_mode = true;
    struct ciuki_media_cd_read rd = { .version = 1, .size = sizeof(rd), .slot = 1, .generation = gen, .lba = 1000, .sectors = 27,
                                      .buffer = CD_USER_BASE + 4096 };
    memset(cd_user + 4096, 0xee, 27 * 2352);
    CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == 0 && rd.done == 27 * 2352);
    bool same = true;
    for (uint32_t i = 0; i < 27 * 2352; i++) if (cd_user[4096 + i] != packet_cdda_byte(1000 + i / 2352, i % 2352)) same = false;
    CHECK(same && cd_bounce_allocs == 1 && cd_bounce_frees == 1 && !cd_bounce_pages_out && f.read_cd_packets == 1);
    rd.lba = 8999; rd.sectors = 1; rd.done = 0; /* the last audio sector: one 2,352-byte run of 1 page */
    CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == 0 && rd.done == 2352 && f.read_cd_lba == 8999 &&
          cd_bounce_allocs == 2 && cd_bounce_frees == 2);
    /* Refused before any packet and any allocation. */
    issued = f.packets; unsigned allocs = cd_bounce_allocs;
    rd.lba = 999; rd.sectors = 2; CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -EINVAL);   /* data track */
    rd.lba = 1000; rd.sectors = 28; CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -EINVAL); /* too many */
    rd.sectors = 0; CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -EINVAL);
    rd.sectors = 1; rd.buffer = 0; CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -EINVAL);
    rd.buffer = 16; CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -EFAULT);              /* outside user space */
    rd.buffer = CD_USER_BASE + sizeof(cd_user) - 100; CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -EFAULT);
    rd.buffer = CD_USER_BASE + 4096; rd.generation = gen + 1;
    CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -EAGAIN);
    CHECK(f.packets == issued && cd_bounce_allocs == allocs);
    rd.generation = gen; cd_bounce_fail = true;
    CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -ENOMEM && f.packets == issued);       /* no bounce run: no packet */
    cd_bounce_fail = false;
    /* A user-copy fault after the transfer (bounce copy, then record copy) frees the run. */
    memcpy(cd_user + 256, &rd, sizeof(rd)); cd_user_calls = 0; cd_user_fail_at = 1; /* the record copy-in itself */
    CHECK(media_cd_devctl(&proc, &optical_store, MEDIA_CD_READ_AUDIO, CD_USER_BASE + 256, sizeof(rd)) == -EFAULT);
    cd_user_fail_at = 0;
    CHECK(f.packets == issued && cd_bounce_pages_out == 0);
    for (unsigned at = 2; at <= 3; at++) {
        unsigned frees = cd_bounce_frees; issued = f.packets;
        memcpy(cd_user + 256, &rd, sizeof(rd)); cd_user_calls = 0; cd_user_fail_at = at;
        CHECK(media_cd_devctl(&proc, &optical_store, MEDIA_CD_READ_AUDIO, CD_USER_BASE + 256, sizeof(rd)) == -EFAULT);
        cd_user_fail_at = 0;
        CHECK(cd_bounce_frees == frees + 1 && !cd_bounce_pages_out && f.packets == issued + 1);
    }
    /* Packet failures: EIO for the request only; MEDIUM NOT PRESENT -> ENXIO. */
    f.read_cd_fail = true; f.sense_key = 5; f.asc = 0x64; f.ascq = 0; rd.sectors = 4; rd.lba = 1000;
    allocs = cd_bounce_frees;
    CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -EINVAL && cd_bounce_pages_out == 0 && cd_bounce_frees == allocs + 1);
    CHECK(d->media == ATAPI_READY && d->block.media_generation == gen);
    f.sense_key = 2; f.asc = 0x3a;
    CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -ENXIO && d->media == ATAPI_NO_MEDIUM);
    rd.generation = d->block.media_generation;
    CHECK(cd_ctl(&proc, MEDIA_CD_READ_AUDIO, &rd, sizeof(rd)) == -ENXIO);
    st = (struct ciuki_media_cd_state){ .version = 1, .size = sizeof(st), .slot = 1 };
    CHECK(cd_ctl(&proc, MEDIA_CD_STATE, &st, sizeof(st)) == 0 && st.state == CIUKI_CD_NO_MEDIUM && st.generation > gen);
    toc = (struct ciuki_media_cd_toc){ .version = 1, .size = sizeof(toc), .slot = 1 };
    CHECK(cd_ctl(&proc, MEDIA_CD_TOC, &toc, sizeof(toc)) == -ENXIO);
    f.read_cd_fail = false; cd_bounce_mode = false;

    /* Analog commands and the tray on a fresh disc. */
    packet_fresh(&f, 12); f.responses = true; cd_caps_all(&f); cd_mixed_toc(&f);
    d = &f.disk.channel.devices[1];
    optical_store.optical_bindings[1].device = d;
    CHECK(!atapi_ready(d)); gen = d->block.media_generation;
    struct ciuki_media_cd_play pl = { .version = 1, .size = sizeof(pl), .slot = 1, .generation = gen, .start_lba = 1000, .end_lba = 5150 };
    CHECK(cd_ctl(&proc, MEDIA_CD_PLAY, &pl, sizeof(pl)) == 0 && f.play_packets == 1 && f.play_start[1] == 15 && f.play_end[1] == 10);
    struct ciuki_media_cd_tray busy_tray = {.version=1,.size=sizeof(busy_tray),.slot=1,.generation=gen,.action=CIUKI_CD_TRAY_OPEN};
    issued=f.packets;
    CHECK(cd_ctl(&proc,MEDIA_CD_TRAY,&busy_tray,sizeof(busy_tray))==-EBUSY && f.packets==issued);
    pl.start_lba = 900; CHECK(cd_ctl(&proc, MEDIA_CD_PLAY, &pl, sizeof(pl)) == -EINVAL && f.play_packets == 1);
    pl.start_lba = 1000; pl.generation = gen + 1; CHECK(cd_ctl(&proc, MEDIA_CD_PLAY, &pl, sizeof(pl)) == -EAGAIN);
    struct ciuki_media_cd_pause pa = { .version = 1, .size = sizeof(pa), .slot = 1, .generation = gen, .action = CIUKI_CD_ACTION_PAUSE };
    CHECK(cd_ctl(&proc, MEDIA_CD_PAUSE, &pa, sizeof(pa)) == 0 && f.pause_packets == 1 && f.pause_resume == 0);
    pa.action = CIUKI_CD_ACTION_RESUME; CHECK(cd_ctl(&proc, MEDIA_CD_PAUSE, &pa, sizeof(pa)) == 0 && f.pause_packets == 2 && f.pause_resume == 1);
    pa.action = CIUKI_CD_ACTION_STOP; CHECK(cd_ctl(&proc, MEDIA_CD_PAUSE, &pa, sizeof(pa)) == 0 && f.stop_packets == 1);
    pa.action = 3; CHECK(cd_ctl(&proc, MEDIA_CD_PAUSE, &pa, sizeof(pa)) == -EINVAL && f.stop_packets == 1);
    pa.action = 0; pa.reserved = 1; CHECK(cd_ctl(&proc, MEDIA_CD_PAUSE, &pa, sizeof(pa)) == -EINVAL);
    f.sub_track = 2; f.sub_index = 1; f.sub_abs = 1300; f.sub_rel = 300; f.audio_status = 0x11;
    struct ciuki_media_cd_position po = { .version = 1, .size = sizeof(po), .slot = 1, .generation = gen };
    CHECK(cd_ctl(&proc, MEDIA_CD_POSITION, &po, sizeof(po)) == 0 && po.audio_status == 0x11 && po.track == 2 && po.index == 1 &&
          po.absolute_lba == 1300 && po.relative_lba == 300);
    po.generation = gen + 1; CHECK(cd_ctl(&proc, MEDIA_CD_POSITION, &po, sizeof(po)) == -EAGAIN);
    f.play_fail = true; f.sense_key = 5; f.asc = 0x21; pl.generation = gen; pl.start_lba = 1000;
    CHECK(cd_ctl(&proc, MEDIA_CD_PLAY, &pl, sizeof(pl)) == -EINVAL && d->media == ATAPI_READY);
    f.play_fail = false;
    pa.action = CIUKI_CD_ACTION_STOP; pa.reserved = 0;
    CHECK(cd_ctl(&proc, MEDIA_CD_PAUSE, &pa, sizeof(pa)) == 0 && !d->cd_playing);
    struct ciuki_media_cd_tray tr = { .version = 1, .size = sizeof(tr), .slot = 1, .action = 9 };
    CHECK(cd_ctl(&proc, MEDIA_CD_TRAY, &tr, sizeof(tr)) == -EINVAL);
    tr.action = CIUKI_CD_TRAY_CLOSE; CHECK(cd_ctl(&proc, MEDIA_CD_TRAY, &tr, sizeof(tr)) == 0 && f.cdb[0] == 0x1b && f.cdb[4] == 3);
    tr.action = CIUKI_CD_TRAY_OPEN; CHECK(cd_ctl(&proc, MEDIA_CD_TRAY, &tr, sizeof(tr)) == 0 && f.cdb[0] == 0x1b && f.cdb[4] == 2);
    CHECK(d->media == ATAPI_NO_MEDIUM && d->block.media_generation > gen);
    st = (struct ciuki_media_cd_state){ .version = 1, .size = sizeof(st), .slot = 1 };
    CHECK(cd_ctl(&proc, MEDIA_CD_STATE, &st, sizeof(st)) == 0 && st.state == CIUKI_CD_NO_MEDIUM && !st.audio_ready);
    CHECK(!d->media_writes && !d->block.quarantined && !f.disk.channel.stopped && !cd_bounce_pages_out);
    storage_destroy(&optical_store);
    puts("PASS /dev/media CD audio devctls: record validation, STATE/INFO/TOC, READ_AUDIO bounce run and user faults, refusals before any packet, errno mapping, PLAY/PAUSE/STOP/POSITION, tray");
}
