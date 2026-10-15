/* USB MSC 1.0 BOT, transparent SCSI LUN zero. No issued write is retried.
 * See docs/design/usb-contract.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/hardware_check.h>
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
static unsigned raw_port(struct usb_device *d)
{
    struct usb_host *h=d->host;
    bool uhci=h->pci.prog_if==0;
    return usb_read(h,uhci ? 0x10+2*d->root_port : 0x54+4*d->root_port,uhci ? 2 : 4);
}
static uint64_t bulk_end(struct usb_device *d)
{
    struct usb_storage *s=&d->storage;
    uint64_t end=d->host->io.now(d->host->io.cookie)+
        (s->binding ? USB_BIND_BULK_MS : USB_BULK_TIMEOUT_MS);
    if (s->binding && (int64_t)(end-(s->bind_deadline-USB_BIND_FINISH_MS))>0)
        end=s->bind_deadline-USB_BIND_FINISH_MS;
    uint64_t check=hardware_check_io_deadline ? hardware_check_io_deadline() : 0;
    if (check && check<end) end=check;
    return end;
}
static void bulk_packet_record(struct usb_device *d)
{
    struct usb_storage_trace *t=&d->storage.trace;
    if (t->packet_count<USB_BULK_TRACE_LIMIT) t->packets[t->packet_count]=d->packet_trace;
    t->packet_count++;
    if (d->storage.binding) {
        d->storage.bind_packets++;
        d->storage.bind_poll_frames+=d->packet_trace.poll_frames;
        d->storage.bind_retire_frames+=d->packet_trace.frames-d->packet_trace.poll_frames;
    }
}
static int bulk_result(struct usb_device *d, int result, const char *stop)
{
    struct usb_storage_trace *t=&d->storage.trace;
    uint64_t now=d->host->io.now(d->host->io.cookie);
    t->bulk_stop=stop; t->bulk_elapsed_ms=now-t->bulk_started_ms;
    int64_t left=d->storage.binding ? (int64_t)(d->storage.bind_deadline-now) : 0;
    t->bind_remaining_ms=left>0 ? (uint64_t)left : 0;
    return result;
}
/* Caller owns the host lock. ACK alone advances toggle, including a ZLP.
 * One retired TD per packet uses the existing controller bounce buffer. */
int usb_bulk(struct usb_device *d, bool in, void *data, unsigned bytes, unsigned *actual)
{
    if (actual) *actual=0;
    if (!d || !d->host || !actual || (bytes && !data) || bytes>BLKDEV_READ_BYTES) return -USB_EPROTO;
    d->packet_trace=(struct usb_packet_trace){.actual=-1};
    struct usb_storage *s=&d->storage;
    unsigned packet=in ? s->in_packet : s->out_packet, ep=in ? s->in : s->out;
    if (!s->bound || (packet!=8 && packet!=16 && packet!=32 && packet!=64) ||
        !ep || ep>15 || d->low_speed) return -USB_EPROTO;
    uint8_t *toggle=in ? &s->in_toggle : &s->out_toggle;
    struct usb_storage_trace *t=&s->trace;
    uint64_t end=bulk_end(d);
    t->packet_count=0; t->packet_index=0; t->packet_offset=0;
    t->bulk_started_ms=d->host->io.now(d->host->io.cookie); t->bulk_deadline=end;
    if (bytes>512 && d->host->ops->bulk) {
        s->bulk_deadline=end;
        int e=d->host->ops->bulk(d,in,data,bytes,actual);
        s->bulk_deadline=0;
        return bulk_result(d,e,e ? "packet" : *actual<bytes ? "short" : "complete");
    }
    while (*actual<bytes) {
        unsigned n=bytes-*actual; if (n>packet) n=packet;
        t->packet_index=t->packet_count; t->packet_offset=*actual;
        /* A refusal before submission must name the next packet, not retain
         * the previous packet's successful TD/toggle (the T23 448-byte trace). */
        d->packet_trace=(struct usb_packet_trace){.expected=n,.actual=-1,
            .endpoint=ep,.pid=in ? USB_PID_IN : USB_PID_OUT,.toggle=*toggle};
        bool connected=live(d);
        if (!connected || usb_expired(d->host,end)) {
            int e=connected ? -USB_ETIMEDOUT : -USB_EIO;
            d->packet_trace.result=e; bulk_packet_record(d);
            const char *stop=!connected ? "disconnected" :
                s->binding && usb_expired(d->host,s->bind_deadline-USB_BIND_FINISH_MS) ?
                "bind_deadline" : "bulk_deadline";
            return bulk_result(d,e,stop);
        }
        s->bulk_deadline=end;
        int e=d->host->ops->packet(d,ep,in ? USB_PID_IN : USB_PID_OUT,*toggle,
                                  (uint8_t *)data+*actual,n,false);
        s->bulk_deadline=0;
        if (!d->packet_trace.valid) {
            d->packet_trace.expected=n; d->packet_trace.endpoint=ep;
            d->packet_trace.pid=in ? USB_PID_IN : USB_PID_OUT; d->packet_trace.toggle=*toggle;
        }
        bulk_packet_record(d);
        if (e<0) return bulk_result(d,e,"packet");
        if ((unsigned)e>n || (!in && (unsigned)e!=n)) return bulk_result(d,-USB_EPROTO,"length");
        *toggle^=1; *actual+=(unsigned)e;
        if (!live(d)) return bulk_result(d,-USB_EIO,"disconnected");
        if ((unsigned)e<n) return bulk_result(d,0,"short");
        /* Full ACK progress earns another bounded wait, not another bind.
         * Retirement/scheduler time cannot consume packet 8's entire chance
         * after seven successful packets. NAK never reaches this renewal. */
        if (s->binding && *actual<bytes) { end=bulk_end(d); t->bulk_deadline=end; }
    }
    return bulk_result(d,0,"complete");
}
/* Keep transport context separate from packet_trace: halt clear/reset and
 * REQUEST SENSE reuse the same TD and must not erase the failing stage. */
static int transfer(struct usb_device *d, const char *stage, bool in, void *data,
                    unsigned bytes, unsigned *actual)
{
    struct usb_storage_trace *t=&d->storage.trace;
    t->stage=stage; t->expected=bytes;
    int e=usb_bulk(d,in,data,bytes,actual);
    t->actual=*actual; t->result=e; t->packet=d->packet_trace; t->port_status=raw_port(d);
    return e;
}
static void trace_record(struct usb_device *d, const char *event, const char *reason)
{
    struct usb_storage_trace *t=&d->storage.trace;
    struct usb_packet_trace *p=&t->packet;
    /* The essential snapshot must fit klog's 254-byte payload on one line.
     * Additional packet/timing records below retain the detailed F1-47 trace. */
    klog("[usb-storage] %s addr=%u reason=%s tag=%u cmd=%02x stage=%s expected=%u actual=%u result=%d td=%08x toggle=%u csw=%u status=%u residue=%u port=%08x",
         event,d->address,reason,t->tag,t->command,t->stage ? t->stage : "none",
         t->expected,t->actual,t->result,p->status,p->toggle,t->csw_available,t->csw_status,t->residue,t->port_status);
    klog("[usb-storage] td addr=%u tag=%u valid=%u ep=%u pid=%02x toggle=%u expected=%u actual=%d td=%08x token=%08x hc_status=%08x result=%d",
         d->address,t->tag,p->valid,p->endpoint,p->pid,p->toggle,p->expected,p->actual,
         p->status,p->token,p->hc_status,p->result);
    klog("[usb-storage] timing addr=%u tag=%u frames=%u elapsed_ms=%llu retired=%d",
         d->address,t->tag,p->frames,p->elapsed_ms,p->retired);
    if (t->bulk_stop) {
        klog("[usb-storage] bulk addr=%u tag=%u packets=%u index=%u offset=%u stop=%s elapsed_ms=%llu deadline_ms=%llu bind_left_ms=%llu",
             d->address,t->tag,t->packet_count,t->packet_index,t->packet_offset,t->bulk_stop,
             t->bulk_elapsed_ms,t->bulk_deadline,t->bind_remaining_ms);
        for (unsigned i=0;i<t->packet_count && i<USB_BULK_TRACE_LIMIT;i++) {
            p=&t->packets[i];
            klog("[usb-storage] packet addr=%u tag=%u index=%u toggle=%u expected=%u actual=%d result=%d td=%08x token=%08x frames=%u elapsed_ms=%llu retired=%d valid=%u",
                 d->address,t->tag,i,p->toggle,p->expected,p->actual,p->result,p->status,p->token,
                 p->frames,p->elapsed_ms,p->retired,p->valid);
        }
    }
    klog("[usb-storage] csw addr=%u tag=%u available=%u signature=%08x csw_tag=%u status=%u residue=%u",
         d->address,t->tag,t->csw_available,t->csw_signature,t->csw_tag,t->csw_status,t->residue);
}
static void first_fault(struct usb_device *d, const char *reason)
{
    if (d->storage.fault_logged) return;
    d->storage.fault_logged=true;
    trace_record(d,"fault",reason);
}
static int clear_halt(struct usb_device *d, bool in)
{
    struct usb_storage *s=&d->storage;
    int e=usb_control(d,2,1,0,(uint16_t)(in ? 0x80|s->in : s->out),0,0);
    if (!e) { if (in) s->in_toggle=0; else s->out_toggle=0; }
    return e;
}
static int reset_recovery(struct usb_device *d)
{
    /* Class reset preserves toggles; only successful halt clear sets DATA0. */
    int e=usb_control(d,0x21,0xff,0,d->storage.interface,0,0);
    if (!e) e=clear_halt(d,true);
    if (!e) e=clear_halt(d,false);
    klog("[usb-storage] reset addr=%u result=%d",d->address,e);
    return e;
}
static int fault(struct usb_device *d, const char *reason, bool reset)
{
    struct usb_storage *s=&d->storage;
    /* A tolerated INQUIRY/MODE fault must not hide the packet that later
     * quarantines the mount. Retain one initial and one terminal snapshot. */
    if (s->fault_logged && !s->quarantine_logged) trace_record(d,"terminal_fault",reason);
    else first_fault(d,reason);
    if (!s->quarantine_logged) {
        klog("[usb-storage] quarantine addr=%u reason=%s stage=%s transfer=%s",d->address,reason,
             s->binding && s->bind_stage ? s->bind_stage : s->trace.stage ? s->trace.stage : "none",
             s->trace.stage ? s->trace.stage : "none");
        s->quarantine_logged=true;
    }
    s->block.quarantined=true; s->online=false;
    d->reason=reason;
    if (reset && live(d) && (!s->binding || !usb_expired(d->host,s->bind_deadline-USB_PERIODIC_WATCHDOG_MS))) {
        /* BOT 5.3.4 ordering. Recovery restores framing only; it never
         * authorizes a second CBW or reuse of an uncertain writable mount. */
        (void)reset_recovery(d);
    }
    return -FS_EIO;
}
enum bot_policy { BOT_STRICT, BOT_FIRST_INQUIRY, BOT_OPTIONAL_MODE };
enum { BOT_RETRY_INQUIRY=2, BOT_MODE_UNAVAILABLE=3 };
static int transport_fault(struct usb_device *d, const char *reason, enum bot_policy policy)
{
    struct usb_storage *s=&d->storage;
    struct usb_storage_trace *t=&s->trace;
    /* The sole read-only attach retry cannot revive a published medium or
     * hardware/DMA failure. A malformed received CSW has result=0, not STALL. */
    bool retry=policy==BOT_FIRST_INQUIRY &&
        (t->result==-USB_ETIMEDOUT || t->result==-USB_EPIPE);
    bool skip=policy==BOT_OPTIONAL_MODE && t->result==-USB_EPIPE;
    if ((retry || skip) && !s->published && !s->online && !s->block.quarantined &&
        !t->packet.retired && live(d)) {
        first_fault(d,reason);
        if (!reset_recovery(d) && live(d)) return retry ? BOT_RETRY_INQUIRY : BOT_MODE_UNAVAILABLE;
        return fault(d,"reset_recovery",false);
    }
    return fault(d,reason,true);
}
/* 0 passed, 1 SCSI command failed, negative transport failure; the two
 * positive policy outcomes are confined to unpublished attach commands. */
static int bot(struct usb_device *d, const uint8_t *cdb, unsigned clen, bool in,
               void *data, unsigned bytes, unsigned *transferred, enum bot_policy policy)
{
    struct usb_storage *s=&d->storage;
    if (!live(d) || s->ejected || s->block.quarantined) return -FS_EIO;
    uint8_t cbw[31]={0}, csw[13]={0}; unsigned n=0, got=0;
    s->trace=(struct usb_storage_trace){.command=cdb[0],.data_expected=bytes};
    fs_wr32(cbw,0x43425355); fs_wr32(cbw+4,++s->tag); fs_wr32(cbw+8,bytes);
    s->trace.tag=s->tag;
    cbw[12]=in ? 0x80 : 0; cbw[14]=(uint8_t)clen; memcpy(cbw+15,cdb,clen);
    int e=transfer(d,"cbw",false,cbw,sizeof(cbw),&n);
    if (e || n!=sizeof(cbw)) return transport_fault(d,"cbw",policy);
    if (bytes) {
        e=transfer(d,"data",in,data,bytes,&got);
        s->trace.data_actual=got;
        if (e==-USB_EPIPE) {
            s->trace.data_stalled=true;
            first_fault(d,"data_stall");
            trace_record(d,"stall","data");
            e=clear_halt(d,in);
            if (e) { s->trace.result=e; return fault(d,"data_clear_halt",true); }
        } else if (e) return transport_fault(d,"data_transfer",policy);
    }
    e=transfer(d,"csw",true,csw,sizeof(csw),&n);
    if (e==-USB_EPIPE) {
        first_fault(d,"csw_stall");
        trace_record(d,"stall","csw");
        e=clear_halt(d,true);
        if (!e) e=transfer(d,"csw",true,csw,sizeof(csw),&n);
        else s->trace.result=e;
    }
    if (!e && n==sizeof(csw)) {
        s->trace.csw_available=true; s->trace.csw_signature=fs_rd32(csw);
        s->trace.csw_tag=fs_rd32(csw+4); s->trace.csw_status=csw[12]; s->trace.residue=fs_rd32(csw+8);
    }
    if (e || n!=sizeof(csw) || fs_rd32(csw)!=0x53425355 ||
        fs_rd32(csw+4)!=s->tag || csw[12]>2 || fs_rd32(csw+8)>bytes)
        return transport_fault(d,"invalid_csw",policy);
    if (csw[12]==2) return fault(d,"phase_error",true);
    if (transferred) *transferred=got;
    if (policy==BOT_OPTIONAL_MODE && s->trace.data_stalled) return BOT_MODE_UNAVAILABLE;
    if (csw[12]==1) return 1;
    if ((transferred ? got+fs_rd32(csw+8)!=bytes : got!=bytes || fs_rd32(csw+8))) return fault(d,"short_command",true);
    return 0;
}
static int command_policy(struct usb_device *d, const uint8_t *cdb, unsigned len,
                          bool in, void *data, unsigned bytes, unsigned *transferred, enum bot_policy policy)
{
    int e=bot(d,cdb,len,in,data,bytes,transferred,policy);
    if (e!=1) return e;
    struct usb_storage_trace failed=d->storage.trace;
    trace_record(d,"command_failed","csw_status");
    uint8_t sense[18]={0}, req[6]={3,0,0,0,18,0}; unsigned got=0;
    /* REQUEST SENSE is diagnostic, never a retry of the failed command. */
    e=bot(d,req,6,true,sense,sizeof(sense),&got,BOT_STRICT);
    if (e || got<14 || ((sense[0]&0x7f)!=0x70 && (sense[0]&0x7f)!=0x71))
        return fault(d,"sense",false);
    struct usb_storage *s=&d->storage;
    s->trace=failed;
    s->sense_key=sense[2]&15; s->asc=sense[12]; s->ascq=sense[13];
    klog("[usb-storage] scsi addr=%u cmd=%02x sense=%02x/%02x/%02x",
         d->address,cdb[0],s->sense_key,s->asc,s->ascq);
    if (s->published && (s->sense_key==6 || (s->sense_key==2 && s->asc==0x3a)))
        return fault(d,"media_changed",false);
    if (policy==BOT_FIRST_INQUIRY && live(d)) {
        first_fault(d,"inquiry_failed");
        if (!reset_recovery(d) && live(d)) return BOT_RETRY_INQUIRY;
        return fault(d,"reset_recovery",false);
    }
    return -FS_EIO;
}
static int command_result(struct usb_device *d, const uint8_t *cdb, unsigned len,
                          bool in, void *data, unsigned bytes, unsigned *transferred)
{ return command_policy(d,cdb,len,in,data,bytes,transferred,BOT_STRICT); }
static int command(struct usb_device *d, const uint8_t *cdb, unsigned len,
                   bool in, void *data, unsigned bytes)
{ return command_result(d,cdb,len,in,data,bytes,0); }
static int ready(struct usb_device *d)
{ uint8_t cdb[6]={0}; return command(d,cdb,6,false,0,0); }
static int flush_locked(struct usb_device *d)
{ uint8_t cdb[10]={0x35}; return command(d,cdb,10,false,0,0); }
static bool block_take(struct usb_device *d)
{
    uint64_t check=hardware_check_io_deadline ? hardware_check_io_deadline() : 0;
    if (check) return usb_host_take_until(d->host,check);
    if (!d->storage.binding) { usb_host_take(d->host); return true; }
    return usb_host_take_until(d->host,d->storage.bind_deadline-USB_BIND_FINISH_MS);
}
static int block_media(struct blkdev *b)
{
    struct usb_device *d=b->ctx;
    if (!block_take(d)) return fault(d,"bind_deadline",false);
    int e=ready(d);
    if (e && d->storage.published && !b->quarantined) e=fault(d,"readiness_failed",false);
    usb_host_drop(d->host); return e;
}
static int block_flush(struct blkdev *b)
{
    struct usb_device *d=b->ctx;
    if (!block_take(d)) return fault(d,"bind_deadline",false);
    int e=flush_locked(d);
    if (e && !b->quarantined) e=fault(d,"flush_failed",false);
    usb_host_drop(d->host); return e;
}
static int block_io(struct blkdev *b, uint64_t lba, uint32_t count, void *buf, bool write)
{
    int e=blkdev_range(b,lba,count);
    if (e || !buf || count>BLKDEV_READ_SECTORS || lba>UINT32_MAX || count>UINT32_MAX-lba+1ull)
        return e ? e : -FS_EINVAL;
    struct usb_device *d=b->ctx; struct usb_storage *s=&d->storage;
    if (!block_take(d)) return fault(d,"bind_deadline",false);
    e=blkdev_range(b,lba,count);
    if (!e && (!s->online || s->ejected)) e=-FS_EIO;
    if (!e && write && (!blkdev_durable(b) || s->write_protected)) e=-FS_EROFS;
    if (!e) {
        e=ready(d); /* observe unit attention before issuing READ/WRITE */
        if (e && s->published && !b->quarantined) e=fault(d,"readiness_failed",false);
    }
    if (!e) {
        if (!write && s->binding) {
            s->bind_read_requests++; s->bind_read_sectors+=count;
            if (count>s->bind_read_max) s->bind_read_max=count;
        }
        uint8_t cdb[10]={0}; cdb[0]=write ? 0x2a : 0x28;
        put32(cdb+2,(uint32_t)lba); cdb[7]=(uint8_t)(count>>8); cdb[8]=(uint8_t)count;
        e=command(d,cdb,10,!write,buf,count*512u);
        if (e && !b->quarantined) e=fault(d,write ? "write_failed" : "read_failed",false);
    }
    usb_host_drop(d->host); return e;
}
static int block_read(struct blkdev *b,uint64_t lba,uint32_t n,void *buf)
{ return block_io(b,lba,n,buf,false); }
static int block_write(struct blkdev *b,uint64_t lba,uint32_t n,const void *buf)
{ return block_io(b,lba,n,(void *)buf,true); }
static bool bind_cancel(void *ctx)
{
    struct usb_device *d=ctx;
    return usb_expired(d->host,d->storage.bind_deadline-USB_BIND_FINISH_MS) ||
           !d->host->ready || d->host->quarantined;
}
static void bind_pause(void *ctx)
{ struct usb_device *d=ctx; d->host->io.sleep(d->host->io.cookie,1); }
static void bind_stage_finish(struct usb_device *d, int result)
{
    struct usb_storage *s=&d->storage;
    if (!s->bind_stage_active) return;
    klog("[usb-storage] stage=%s addr=%u elapsed_ms=%llu result=%d",s->bind_stage,d->address,
         d->host->io.now(d->host->io.cookie)-s->bind_stage_started_ms,result);
    s->bind_stage_active=false;
}
static void bind_stage_begin(struct usb_device *d, const char *name)
{
    bind_stage_finish(d,0);
    struct usb_storage *s=&d->storage;
    s->bind_stage=name; s->bind_stage_started_ms=d->host->io.now(d->host->io.cookie);
    s->bind_stage_active=true;
    klog("[usb-storage] stage=%s addr=%u",name,d->address);
}
/* Each stage enters without a reservation and leaves owning one. The caller
 * checks the return and owns nothing on failure. No log/VFS -> host cycle. */
static int bind_stage(struct usb_device *d, const char *name)
{
    struct usb_host *h=d->host;
    if (d->storage.bind_reserved) { usb_host_drop(h); d->storage.bind_reserved=false; } /* only the current attach owner calls here */
    bind_stage_begin(d,name);
    if (bind_cancel(d) || !usb_host_take_until(h,d->storage.bind_deadline-USB_BIND_FINISH_MS)) {
        d->storage.trace.result=-USB_ETIMEDOUT;
        return fault(d,"bind_deadline",false);
    }
    d->storage.bind_reserved=true; return 0;
}
static int probe_storage(struct usb_device *d)
{
    struct usb_storage *s=&d->storage;
    if (!s->bound || !s->in || !s->out || d->low_speed) return -FS_EINVAL;
    s->block=(struct blkdev){.read=block_read,.write=block_write,.media_check=block_media,
        .sector_size=512,.ctx=d,.media_generation=gen_alloc()};
    if (bind_stage(d,"max_lun")) return -FS_EIO;
    uint8_t lun=0;
    s->trace=(struct usb_storage_trace){.stage="max_lun",.expected=1};
    int e=usb_control(d,0xa1,0xfe,0,s->interface,&lun,1);
    s->trace.result=e; s->trace.actual=e>0 ? (unsigned)e : 0; s->trace.packet=d->packet_trace;
    s->trace.port_status=raw_port(d);
    if (e!=-USB_EPIPE && (e!=1 || lun>15)) return fault(d,"max_lun",false);
    klog("[usb-storage] interface=%u addr=%u max_lun=%u selected=0",s->interface,d->address,lun);
    uint8_t inquiry[36]={0}, cdb[10]={0x12,0,0,0,36,0};
    unsigned got=0;
    if (bind_stage(d,"inquiry")) return -FS_EIO;
    e=command_policy(d,cdb,6,true,inquiry,sizeof(inquiry),&got,BOT_FIRST_INQUIRY);
    if (e==BOT_RETRY_INQUIRY) {
        bind_stage_finish(d,e);
        klog("[usb-storage] retry addr=%u cmd=12 attempt=2",d->address);
        if (bind_stage(d,"inquiry")) return -FS_EIO;
        e=command_result(d,cdb,6,true,inquiry,sizeof(inquiry),&got);
    }
    if (e) return s->block.quarantined ? e : fault(d,"inquiry_failed",false);
    /* Only the fixed header is consumed; short IN is legal with a passed
     * CSW and consistent residue. Never infer missing capability bytes. */
    if (got<5) return fault(d,"short_inquiry",false);
    if ((inquiry[0]&0x1f)!=0 || (inquiry[0]&0xe0)) return fault(d,"unsupported_lun",false);
    /* Clear initial power-on unit attention. A successful sense response
     * saying 'becoming ready' may outlast one BOT wait, but not the bind.
     * Transport failures and other NOT READY conditions are not retried. */
    if (bind_stage(d,"ready")) return -FS_EIO;
    unsigned attention=0, polls=0;
    for (;;) {
        if (bind_cancel(d)) return fault(d,"bind_deadline",false);
        e=ready(d);
        if (!e || s->block.quarantined) break;
        if (s->sense_key==6 && ++attention<3) continue;
        if (s->sense_key!=2 || s->asc!=4 || s->ascq!=1 ||
            polls++>=USB_BIND_TIMEOUT_MS/100u) break;
        if (bind_cancel(d)) return fault(d,"bind_deadline",false);
        uint64_t now=d->host->io.now(d->host->io.cookie);
        int64_t left=(int64_t)(s->bind_deadline-USB_BIND_FINISH_MS-now);
        if (left<=0) return fault(d,"bind_deadline",false);
        usb_sleep(d->host,left<100u ? (unsigned)left : 100u);
    }
    if (e) return fault(d,"not_ready",false);
    if (bind_stage(d,"capacity")) return -FS_EIO;
    uint8_t capacity[8]; memset(cdb,0,sizeof(cdb)); cdb[0]=0x25;
    e=command(d,cdb,10,true,capacity,sizeof(capacity)); if (e) return e;
    if (be32(capacity+4)!=512 || be32(capacity)==UINT32_MAX) return fault(d,"sector_size_or_capacity",false);
    s->block.capacity=(uint64_t)be32(capacity)+1;
    /* A successful SYNCHRONIZE CACHE qualifies the durability callback.
     * Unknown/unsupported flush keeps the FAT mount read-only. */
    if (bind_stage(d,"flush")) return -FS_EIO;
    e=flush_locked(d);
    if (!e) { s->block.write_cache_state=BLKDEV_CACHE_ENABLED; s->block.flush=block_flush; }
    else if (s->block.quarantined) return e;
    bind_stage_finish(d,e);
    /* MODE SENSE(6) header reports write protection for transparent SCSI.
     * Its absence does not invent a cache policy; failed writes fail closed. */
    if (bind_stage(d,"mode")) return -FS_EIO;
    uint8_t mode[4]={0}; uint8_t mode_cdb[6]={0x1a,8,0x3f,0,4,0};
    e=command_policy(d,mode_cdb,6,true,mode,sizeof(mode),0,BOT_OPTIONAL_MODE);
    if (!e) s->write_protected=!!(mode[2]&0x80);
    else if (e==BOT_MODE_UNAVAILABLE)
        klog("[usb-storage] mode_sense addr=%u reason=stalled assume_writable=1",d->address);
    else if (s->block.quarantined) return e;
    if (s->write_protected) { s->block.write=0; s->block.flush=0; s->block.write_cache_state=BLKDEV_CACHE_UNKNOWN; }
    bind_stage_finish(d,e);
    s->online=true; d->reason="storage_ready";
    klog("[usb-storage] ready addr=%u sectors=%llu generation=%u mode=%s elapsed_ms=%llu",
         d->address,(unsigned long long)s->block.capacity,s->block.media_generation,
         blkdev_durable(&s->block) ? "rw_capable" : "ro_no_flush",
         d->host->io.now(d->host->io.cookie)-s->bind_started_ms);
    return 0;
}
int usb_storage_attach(struct usb_device *d)
{
    if (!d || !d->host) return -FS_EINVAL;
    struct usb_storage *s=&d->storage;
    s->binding=true; s->bind_started_ms=d->host->io.now(d->host->io.cookie);
    s->bind_deadline=s->bind_started_ms+USB_BIND_TIMEOUT_MS; s->bind_stage_active=false;
    int e=probe_storage(d);
    bind_stage_finish(d,e);
    if (s->bind_reserved) { usb_host_drop(d->host); s->bind_reserved=false; }
    /* Keep the same deadline until usb_storage_service publishes the mount. */
    if (e) {
        s->block.quarantined=true; s->online=false; d->quarantined=true;
        s->binding=false;
    }
    return e;
}
void usb_storage_detach(struct usb_device *d)
{
    if (!d->storage.bound) return;
    d->storage.block.quarantined=true; d->storage.online=false;
}
void usb_storage_service(struct usb_host *h)
{
    struct storage *store=storage_get();
    if (!store || !store->ready || store->stopped) {
        for (unsigned i=0;i<h->device_count;i++) {
            struct usb_device *d=&h->devices[i];struct usb_storage *s=&d->storage;
            if (!s->binding || !s->online) continue;
            bind_stage_begin(d,"mount");
            int e=fault(d,"mount_unavailable",false);
            bind_stage_finish(d,e);d->quarantined=true;s->binding=false;
        }
        return;
    }
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
            bind_stage_begin(d,"mount");
            int e=bind_cancel(d) ? -FS_EIO :
                storage_add_removable_bounded(store,&s->block,bind_cancel,bind_pause,d);
            if (e) {
                (void)fault(d,bind_cancel(d) ? "bind_deadline" : "mount_failed",false);
                d->quarantined=true;
            } else {
                s->published=true;
                /* Publication is the commit point. Diagnostic output may
                 * exceed the budget but cannot revoke a completed mount. */
                s->binding=false;
                for (unsigned drive=4;drive<26;drive++) {
                    struct storage_volume *v=&store->volumes[drive];
                    if (!v->present || !v->removable || v->part.parent!=&s->block) continue;
                    klog("[usb-storage] volume drive=%c part=%u fat=%u mode=%s dirty=%u error=%d mount_ms=%u repair_ms=%u clusters=%u spc=%u",
                         'A'+drive,v->partition,v->fat.type,v->fat.readonly ? "ro" : "rw",
                         !!(v->fat.ro_reasons & FAT_RO_DIRTY),v->error,v->fat.mount_ms,v->fat.repair_ms,v->fat.clusters,v->fat.spc);
                }
            }
            bind_stage_finish(d,e);
            s->binding=false;
            /* Retain the attempt, including refusal; no recovery loop. */
            klog("[usb-storage] mount addr=%u result=%d",d->address,e);
            uint64_t elapsed=h->io.now(h->io.cookie)-s->bind_started_ms;
            klog("[usb-storage] bind addr=%u elapsed_ms=%llu result=%d published=%u overrun=%u reads=%u sectors=%u max_sectors=%u",
                 d->address,elapsed,e,s->published,elapsed>USB_BIND_TIMEOUT_MS,
                 s->bind_read_requests,s->bind_read_sectors,s->bind_read_max);
            klog("[usb-storage] cadence addr=%u packets=%u poll_frames=%u retire_frames=%u batch=%u",
                 d->address,s->bind_packets,s->bind_poll_frames,s->bind_retire_frames,h->ops->bulk ? USB_BULK_BATCH : 1);
        }
    }
}
int usb_storage_eject(struct storage *store, unsigned drive, uint32_t generation)
{
    if (!store || !store->ready || store->stopped || drive<4 || drive>=26 || !generation) return -FS_EINVAL;
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
                usb_host_take(d->host);
                d->storage.ejected=true; d->storage.online=false;
                d->storage.published=false; d->storage.binding=false;
                b->quarantined=true; d->reason="safely_removed";
                usb_host_drop(d->host);
                klog("[usb-storage] safely_removed addr=%u drive=%c generation=%u",
                     d->address,'A'+drive,generation);
            }
        }
    }
    klog("[usb-storage] removal drive=%c generation=%u result=%d reason=%s",
         'A'+drive,generation,e,!e ? "safely_removed" : e==-FS_EBUSY ? "open_files" : "removal_failed");
    fs_lock_drop(&store->vfs.lock); return e;
}
