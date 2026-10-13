/* USB MSC 1.0 BOT, transparent SCSI LUN zero. No issued write is retried.
 * See docs/design/usb-contract.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#include <ciuki/storage.h>

static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void put32(uint8_t *p, uint32_t n)
{ p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n; }
static bool live(struct usb_device *d)
{
    struct usb_host *h=d->host;
    if (!d->present || d->quarantined || !h->ready || h->quarantined) return false;
    unsigned status=h->ops->port_status(h,d->root_port);
    return (status & (USB_PORT_CONNECTED|USB_PORT_ENABLED|USB_PORT_CHANGED|USB_PORT_OVERCURRENT)) ==
           (USB_PORT_CONNECTED|USB_PORT_ENABLED);
}
/* Caller owns the host lock. ACK alone advances toggle, including a ZLP.
 * One retired TD per packet uses the existing controller bounce buffer. */
int usb_bulk(struct usb_device *d, bool in, void *data, unsigned bytes, unsigned *actual)
{
    if (!d || !actual || (bytes && !data) || bytes>BLKDEV_READ_BYTES) return -USB_EPROTO;
    struct usb_storage *s=&d->storage;
    unsigned packet=in ? s->in_packet : s->out_packet, ep=in ? s->in : s->out;
    if (!s->bound || !packet || !ep || d->low_speed) return -USB_EPROTO;
    uint8_t *toggle=in ? &s->in_toggle : &s->out_toggle;
    uint64_t end=d->host->io.now(d->host->io.cookie)+10000;
    *actual=0;
    while (*actual<bytes) {
        if (!live(d)) return -USB_EIO;
        if (usb_expired(d->host,end)) return -USB_ETIMEDOUT;
        unsigned n=bytes-*actual; if (n>packet) n=packet;
        int e=d->host->ops->packet(d,ep,in ? USB_PID_IN : USB_PID_OUT,*toggle,
                                  (uint8_t *)data+*actual,n,false);
        if (e<0) return e;
        if ((unsigned)e>n || (!in && (unsigned)e!=n)) return -USB_EPROTO;
        *toggle^=1; *actual+=(unsigned)e;
        if (!live(d)) return -USB_EIO;
        if ((unsigned)e<n) break;
    }
    return 0;
}
static int clear_halt(struct usb_device *d, bool in)
{
    struct usb_storage *s=&d->storage;
    int e=usb_control(d,2,1,0,(uint16_t)(in ? 0x80|s->in : s->out),0,0);
    if (!e) { if (in) s->in_toggle=0; else s->out_toggle=0; }
    return e;
}
static int fault(struct usb_device *d, const char *reason, bool reset)
{
    struct usb_storage *s=&d->storage;
    s->block.quarantined=true; s->online=false;
    if (reset && live(d)) {
        /* BOT 5.3.4 ordering. Recovery restores framing only; it never
         * authorizes a second CBW or reuse of an uncertain writable mount. */
        int e=usb_control(d,0x21,0xff,0,s->interface,0,0);
        if (!e) e=clear_halt(d,true);
        if (!e) e=clear_halt(d,false);
        klog("[usb-storage] reset addr=%u result=%d",d->address,e);
    }
    d->reason=reason;
    klog("[usb-storage] fault addr=%u reason=%s tag=%u",d->address,reason,s->tag);
    return -FS_EIO;
}
/* 0 passed, 1 SCSI command failed, negative transport failure. */
static int bot(struct usb_device *d, const uint8_t *cdb, unsigned clen, bool in,
               void *data, unsigned bytes, unsigned *transferred)
{
    struct usb_storage *s=&d->storage;
    if (!live(d) || s->ejected || s->block.quarantined) return -FS_EIO;
    uint8_t cbw[31]={0}, csw[13]; unsigned n=0, got=0;
    fs_wr32(cbw,0x43425355); fs_wr32(cbw+4,++s->tag); fs_wr32(cbw+8,bytes);
    cbw[12]=in ? 0x80 : 0; cbw[14]=(uint8_t)clen; memcpy(cbw+15,cdb,clen);
    int e=usb_bulk(d,false,cbw,sizeof(cbw),&n);
    if (e || n!=sizeof(cbw)) return fault(d,"cbw",true);
    if (bytes) {
        e=usb_bulk(d,in,data,bytes,&got);
        if (e==-USB_EPIPE) {
            e=clear_halt(d,in);
            if (e) return fault(d,"data_clear_halt",true);
        } else if (e) return fault(d,"data_transfer",true);
    }
    e=usb_bulk(d,true,csw,sizeof(csw),&n);
    if (e==-USB_EPIPE) {
        e=clear_halt(d,true);
        if (!e) e=usb_bulk(d,true,csw,sizeof(csw),&n);
    }
    if (e || n!=sizeof(csw) || fs_rd32(csw)!=0x53425355 ||
        fs_rd32(csw+4)!=s->tag || csw[12]>2 || fs_rd32(csw+8)>bytes)
        return fault(d,"invalid_csw",true);
    if (csw[12]==2) return fault(d,"phase_error",true);
    if (transferred) *transferred=got;
    if (csw[12]==1) return 1;
    if ((transferred ? got+fs_rd32(csw+8)!=bytes : got!=bytes || fs_rd32(csw+8))) return fault(d,"short_command",true);
    return 0;
}
static int command(struct usb_device *d, const uint8_t *cdb, unsigned len,
                   bool in, void *data, unsigned bytes)
{
    int e=bot(d,cdb,len,in,data,bytes,0);
    if (e!=1) return e;
    uint8_t sense[18]={0}, req[6]={3,0,0,0,18,0}; unsigned got=0;
    /* REQUEST SENSE is diagnostic, never a retry of the failed command. */
    e=bot(d,req,6,true,sense,sizeof(sense),&got);
    if (e || got<14 || ((sense[0]&0x7f)!=0x70 && (sense[0]&0x7f)!=0x71))
        return fault(d,"sense",false);
    struct usb_storage *s=&d->storage;
    s->sense_key=sense[2]&15; s->asc=sense[12]; s->ascq=sense[13];
    klog("[usb-storage] scsi addr=%u cmd=%02x sense=%02x/%02x/%02x",
         d->address,cdb[0],s->sense_key,s->asc,s->ascq);
    if (s->published && (s->sense_key==6 || (s->sense_key==2 && s->asc==0x3a)))
        return fault(d,"media_changed",false);
    return -FS_EIO;
}
static int ready(struct usb_device *d)
{ uint8_t cdb[6]={0}; return command(d,cdb,6,false,0,0); }
static int flush_locked(struct usb_device *d)
{ uint8_t cdb[10]={0x35}; return command(d,cdb,10,false,0,0); }
static int block_media(struct blkdev *b)
{
    struct usb_device *d=b->ctx; fs_lock_take(&d->host->transfer_lock);
    int e=ready(d);
    if (e && d->storage.published && !b->quarantined) e=fault(d,"readiness_failed",false);
    fs_lock_drop(&d->host->transfer_lock); return e;
}
static int block_flush(struct blkdev *b)
{
    struct usb_device *d=b->ctx; fs_lock_take(&d->host->transfer_lock);
    int e=flush_locked(d);
    if (e && !b->quarantined) e=fault(d,"flush_failed",false);
    fs_lock_drop(&d->host->transfer_lock); return e;
}
static int block_io(struct blkdev *b, uint64_t lba, uint32_t count, void *buf, bool write)
{
    int e=blkdev_range(b,lba,count);
    if (e || !buf || count>BLKDEV_READ_SECTORS || lba>UINT32_MAX || count>UINT32_MAX-lba+1ull)
        return e ? e : -FS_EINVAL;
    struct usb_device *d=b->ctx; struct usb_storage *s=&d->storage;
    fs_lock_take(&d->host->transfer_lock);
    e=blkdev_range(b,lba,count);
    if (!e && (!s->online || s->ejected)) e=-FS_EIO;
    if (!e && write && (!blkdev_durable(b) || s->write_protected)) e=-FS_EROFS;
    if (!e) {
        e=ready(d); /* observe unit attention before issuing READ/WRITE */
        if (e && s->published && !b->quarantined) e=fault(d,"readiness_failed",false);
    }
    if (!e) {
        uint8_t cdb[10]={0}; cdb[0]=write ? 0x2a : 0x28;
        put32(cdb+2,(uint32_t)lba); cdb[7]=(uint8_t)(count>>8); cdb[8]=(uint8_t)count;
        e=command(d,cdb,10,!write,buf,count*512u);
        if (e && !b->quarantined) e=fault(d,write ? "write_failed" : "read_failed",false);
    }
    fs_lock_drop(&d->host->transfer_lock); return e;
}
static int block_read(struct blkdev *b,uint64_t lba,uint32_t n,void *buf)
{ return block_io(b,lba,n,buf,false); }
static int block_write(struct blkdev *b,uint64_t lba,uint32_t n,const void *buf)
{ return block_io(b,lba,n,(void *)buf,true); }
int usb_storage_attach(struct usb_device *d)
{
    struct usb_storage *s=&d->storage;
    if (!s->bound || !s->in || !s->out || d->low_speed) return -FS_EINVAL;
    s->block=(struct blkdev){.read=block_read,.write=block_write,.media_check=block_media,
        .sector_size=512,.ctx=d,.media_generation=gen_alloc()};
    uint8_t lun=0;
    int e=usb_control(d,0xa1,0xfe,0,s->interface,&lun,1);
    if (e!=-USB_EPIPE && (e!=1 || lun>15)) return fault(d,"max_lun",false);
    klog("[usb-storage] interface=%u addr=%u max_lun=%u selected=0",s->interface,d->address,lun);
    uint8_t inquiry[36]={0}, cdb[10]={0x12,0,0,0,36,0};
    e=command(d,cdb,6,true,inquiry,sizeof(inquiry));
    if (e) return e;
    if ((inquiry[0]&0x1f)!=0 || (inquiry[0]&0xe0)) return fault(d,"unsupported_lun",false);
    /* Clear initial power-on unit attention, bounded before any mount/write. */
    for (unsigned i=0;i<3;i++) {
        e=ready(d); if (!e || s->sense_key!=6 || s->block.quarantined) break;
    }
    if (e) return fault(d,"not_ready",false);
    uint8_t capacity[8]; memset(cdb,0,sizeof(cdb)); cdb[0]=0x25;
    e=command(d,cdb,10,true,capacity,sizeof(capacity)); if (e) return e;
    if (be32(capacity+4)!=512 || be32(capacity)==UINT32_MAX) return fault(d,"sector_size_or_capacity",false);
    s->block.capacity=(uint64_t)be32(capacity)+1;
    /* A successful SYNCHRONIZE CACHE qualifies the durability callback.
     * Unknown/unsupported flush keeps the FAT mount read-only. */
    e=flush_locked(d);
    if (!e) { s->block.write_cache_state=BLKDEV_CACHE_ENABLED; s->block.flush=block_flush; }
    else if (s->block.quarantined) return e;
    /* MODE SENSE(6) header reports write protection for transparent SCSI.
     * Its absence does not invent a cache policy; failed writes fail closed. */
    uint8_t mode[4]; uint8_t mode_cdb[6]={0x1a,8,0x3f,0,4,0};
    e=command(d,mode_cdb,6,true,mode,sizeof(mode));
    if (!e) s->write_protected=!!(mode[2]&0x80);
    else if (s->block.quarantined) return e;
    if (s->write_protected) { s->block.write=0; s->block.flush=0; s->block.write_cache_state=BLKDEV_CACHE_UNKNOWN; }
    s->online=true; d->reason="storage_ready";
    klog("[usb-storage] ready addr=%u sectors=%llu generation=%u mode=%s",
         d->address,(unsigned long long)s->block.capacity,s->block.media_generation,
         blkdev_durable(&s->block) ? "rw_capable" : "ro_no_flush");
    return 0;
}
void usb_storage_detach(struct usb_device *d)
{
    if (!d->storage.bound) return;
    d->storage.block.quarantined=true; d->storage.online=false;
}
void usb_storage_service(struct usb_host *h)
{
    struct storage *store=storage_get();
    if (!store || !store->ready || store->stopped) return;
    for (unsigned i=0;i<h->device_count;i++) {
        struct usb_device *d=&h->devices[i]; struct usb_storage *s=&d->storage;
        if (!s->bound) continue;
        if (s->published && s->online && !s->ejected && usb_expired(h,s->next_check)) {
            s->next_check=h->io.now(h->io.cookie)+1000;
            (void)block_media(&s->block);
        }
        if (s->published && (!s->online || s->block.quarantined || !d->present)) {
            storage_remove_removable(store,&s->block,true);
            s->published=false;
            klog("[usb-storage] detached addr=%u reason=%s",d->address,d->reason);
        } else if (s->online && !s->published && !s->ejected) {
            s->published=true;
            int e=storage_add_removable(store,&s->block);
            /* Retain the attempt, including refusal; no recovery loop. */
            klog("[usb-storage] mount addr=%u result=%d",d->address,e);
        }
    }
}
int usb_storage_eject(struct storage *store, unsigned drive, uint32_t generation)
{
    if (!store || !store->ready || drive<4 || drive>=26) return -FS_EINVAL;
    fs_lock_take(&store->vfs.lock);
    struct storage_volume *v=&store->volumes[drive];
    int e=-FS_ENOENT;
    if (v->present && v->removable && store->vfs.volumes[drive]==&v->fat) {
        struct blkdev *b=v->part.parent;
        if (!b || b->media_generation!=generation) e=-FS_EIO;
        else {
            e=storage_remove_removable_locked(store,b,false);
            if (!e) {
                struct usb_device *d=b->ctx;
                fs_lock_take(&d->host->transfer_lock);
                d->storage.ejected=true; d->storage.online=false;
                b->quarantined=true; d->reason="safely_removed";
                fs_lock_drop(&d->host->transfer_lock);
                klog("[usb-storage] safely_removed addr=%u drive=%c generation=%u",
                     d->address,'A'+drive,generation);
            }
        }
    }
    fs_lock_drop(&store->vfs.lock); return e;
}
