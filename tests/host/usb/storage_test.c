/* Production BOT/block/mount/cache/VFS over private mkfs FAT mappings.
 * No copied image, QEMU or controller substitute in usb_storage.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#include <ciuki/storage.h>
#include "uhci_model.h"
struct ciuki_boot_info g_boot;
static gen_t generation;
gen_t gen_alloc(void) { return ++generation; }
static char records[512*1024]; /* retain the bounded becoming-ready replay */
static size_t records_at;
static void (*log_hook)(const char *, size_t);
static void (*sleep_hook)(void);
static void (*hid_poll_hook)(struct usb_device *,struct usb_hid *);
static bool (*private_transaction_hook)(uint32_t,uint32_t,volatile uint32_t *);
void klog_periodic(const char *fmt, ...) __attribute__((alias("klog")));
void klog(const char *fmt,...)
{
    char record[256];va_list ap;va_start(ap,fmt);int n=kvsnprintf(record,sizeof(record),fmt,ap);va_end(ap);
    assert(n>=0 && n<(int)sizeof(record));
    if(records_at+(unsigned)n+2<sizeof(records)) {
        memcpy(records+records_at,record,(unsigned)n);records_at+=(unsigned)n;
        records[records_at++]='\n';records[records_at]=0;
    }
    if(getenv("USB_TEST_LOG")) fprintf(stderr,"%s\n",record);
    if(log_hook) { log_hook(record,(size_t)n); log_hook("\n",1); }
}
/* usb_attach is intentionally not used by this packet-level peripheral model;
 * the register/DMA engines have their own production suite. */
void usb_hid_poll(struct usb_device *d,struct usb_hid *h) { if(hid_poll_hook)hid_poll_hook(d,h); }
void usb_hid_release(struct usb_device *d,struct usb_hid *h) { (void)d;(void)h; }
int registry_quarantine(int h,gen_t g) { (void)h;(void)g;assert(!"unexpected host quarantine");return 0; }
const struct usb_hc_ops usb_ohci_ops={0};
static struct model {
    struct usb_host h;
    _Alignas(4096) uint8_t dma[USB_DMA_BYTES];
    uint8_t *image, *second_image;
    unsigned second;
    unsigned storage_slot;
    size_t bytes;
    uint8_t setup[8], cbw[31], response[36], sense[18], mbr[512];
    unsigned cbw_at, at, remaining, total, offset, status, in_toggle, out_toggle;
    unsigned commands, writes, flushes, resets, clear_in, clear_out, packets;
    unsigned stage; /* 0 CBW, 1 data, 2 CSW */
    uint64_t now;
    bool connected, max_lun_stall, no_flush, wp, phase_error, bad_tag, bad_signature, bad_residue, short_csw;
    bool data_stall, csw_stall, cbw_stall, timeout, remove_data, ua, no_medium, fail_flush;
    bool forever_data, forever_csw, forever_stall, nak_capacity, nak_mount, slow_all;
    bool nak_prepare, nak_eighth;
    unsigned read_commands;
    unsigned sector_size, initial_ua;
    bool never_ready, frozen_clock;
    unsigned not_ready_ascq, ready_commands, flush_delay, first_read_delay;
    uint64_t ready_at;
    bool uhci, data_babble, data_crc, clear_fail, reset_fail, late_ack, latched, mode_stall;
    unsigned inquiry_bytes, capacity_bytes, inquiry_delay, inquiry_delays, inquiries, modes, naks, reset_order, frame_offset, late_acks;
    unsigned mode_csw_stalls;
    uint64_t command_at;
    uint32_t injected_status;
    uint32_t latched_qh, hc_status;
} m;
static int packet(struct usb_device *,unsigned,unsigned,unsigned,void *,unsigned,bool);
static struct usb_device *device(void);
static uint32_t be32(const uint8_t *b) {return (uint32_t)b[0]<<24|(uint32_t)b[1]<<16|(uint32_t)b[2]<<8|b[3];}
static void put32(uint8_t *b,uint32_t v) {b[0]=v>>24;b[1]=v>>16;b[2]=v>>8;b[3]=v;}
static uint64_t now(void *c) { (void)c; return m.frozen_clock ? 0 : m.now; }
static volatile uint32_t *uhci_dma(void *c,uint32_t address,unsigned bytes)
{
    (void)c;assert(address>=m.h.dma_phys && address-m.h.dma_phys<=USB_DMA_BYTES-bytes);
    return (volatile uint32_t *)(m.dma+(address-m.h.dma_phys));
}
static bool uhci_transaction(void *c,uint32_t address,uint32_t qh,volatile uint32_t *td)
{
    (void)c;
    if(address!=m.h.dma_phys+USB_TD && !(address>=m.h.dma_phys+USB_BULK_TD && address<m.h.dma_phys+USB_BULK_TD+USB_BULK_BATCH*16)) {
        assert(private_transaction_hook);return private_transaction_hook(address,qh,td);
    }
    assert(qh==m.h.dma_phys+(((td[2]>>15)&15) ? USB_UHCI_BULK_QH:USB_UHCI_CONTROL_QH));
    unsigned other=((td[2]>>15)&15) ? USB_UHCI_CONTROL_QH:USB_UHCI_BULK_QH;
    assert(uhci_dma(NULL,m.h.dma_phys+other,8)[1]&1); /* shared TD has one owner */
    assert(address==m.h.dma_phys+USB_TD ? td[0]==1 && td[3]==m.h.dma_phys+USB_BUFFER :
           td[3]==m.h.dma_phys+USB_BULK_BUFFER+(address-m.h.dma_phys-USB_BULK_TD)*4);
    unsigned n=(((td[2]>>21)&0x7ff)+1)&0x7ff;
    assert(n<=64 && ((td[2]>>8)&127)==device()->address);
    int got=packet(device(),(td[2]>>15)&15,td[2]&255,(td[2]>>19)&1,m.dma+(td[3]-m.h.dma_phys),n,false);
    if(got==-USB_EAGAIN) {
        td[1]|=1u<<19;m.naks++;
        if(m.late_ack) {m.latched=true;m.latched_qh=qh;}
        return false;
    }
    td[1]&=~(0xffu<<16|0x7ffu);
    if(got<0) td[1]|=m.injected_status ? m.injected_status : got==-USB_EPIPE ? 1u<<22 : 1u<<18;
    else td[1]|=((unsigned)got-1)&0x7ff;
    m.injected_status=0;
    return got>=0;
}
static void sleep_ms(void *c,unsigned ms)
{
    (void)c;
    while(ms--) {
        m.now++;
        if(!m.uhci) { if(sleep_hook)sleep_hook();continue; }
        unsigned frame=(unsigned)(m.now+m.frame_offset)&1023;
        struct uhci_model_io io={NULL,uhci_dma,uhci_transaction,&m.hc_status};
        assert(!uhci_model_frame(&io,((uint32_t *)m.dma)[frame]));
        if(m.latched && (uhci_dma(NULL,m.latched_qh,8)[1]&1)) {
            /* The HC fetched this TD before unlink and completes it while
             * the driver is waiting for retirement. No new TD is queued. */
            volatile uint32_t *td=(volatile uint32_t *)(m.dma+USB_TD);
            assert(td[1]&(1u<<23));m.latched=false;m.late_ack=false;
            m.inquiry_delay=0;m.late_acks++;
            assert(uhci_transaction(NULL,m.h.dma_phys+USB_TD,m.latched_qh,td));
        }
        if(sleep_hook)sleep_hook();
    }
}
static uint32_t read_reg(void *c,unsigned off,unsigned width)
{
    (void)c;assert(width==2);
    if(off==0) return 0xc1;
    if(off==2) return m.hc_status;
    if(off==6) return (unsigned)(m.now+m.frame_offset)&0x7ff;
    if(off==0x10 || off==0x12) return m.connected ? 0x85 : 0x80;
    assert(!"unexpected UHCI register");return 0;
}
static void write_reg(void *c,unsigned off,uint32_t value,unsigned width)
{ (void)c;(void)off;(void)value;(void)width;assert(!"unexpected UHCI register write"); }
static unsigned port_status(struct usb_host *h,unsigned p)
{ (void)h; assert(p==device()->root_port); return m.connected ? 3 : 0; }
static void failed(unsigned key,unsigned asc)
{
    memset(m.sense,0,sizeof(m.sense)); m.sense[0]=0x70; m.sense[2]=(uint8_t)key;
    m.sense[7]=10; m.sense[12]=(uint8_t)asc; m.status=1;
}
static void start_command(void)
{
    assert(fs_rd32(m.cbw)==0x43425355 && m.cbw[13]==0 && m.cbw[14]>=6 && m.cbw[14]<=10);
    m.commands++; m.status=0; m.total=fs_rd32(m.cbw+8); m.remaining=m.total; m.at=0;
    m.command_at=m.now;
    m.stage=m.remaining ? 1 : 2;
    const uint8_t *c=m.cbw+15; memset(m.response,0,sizeof(m.response));
    switch(c[0]) {
    case 0x12: assert(m.total==36);m.inquiries++;m.response[1]=0x80;m.response[2]=2;m.response[4]=31;break;
    case 0x03: assert(m.total==18);memcpy(m.response,m.sense,18);break;
    case 0x00:
        assert(!m.total);m.ready_commands++;
        if(m.ua || m.initial_ua) {m.ua=false;if(m.initial_ua)m.initial_ua--;failed(6,0x28);}
        else if(m.no_medium) failed(2,0x3a);
        else if(m.never_ready || m.now<m.ready_at) {failed(2,4);m.sense[13]=(uint8_t)m.not_ready_ascq;}
        break;
    case 0x25: assert(m.total==8);put32(m.response,(uint32_t)(m.bytes/512+(m.second ? m.second : m.offset)-1));put32(m.response+4,m.sector_size);break;
    case 0x35:
        assert(!m.total);m.flushes++;
        if(m.no_flush) failed(5,0x20); else if(m.fail_flush) failed(4,0x44);
        break;
    case 0x1a: assert(m.total==4);m.modes++;m.response[0]=3;m.response[2]=m.wp ? 0x80:0;break;
    case 0x28: case 0x2a:
        assert(m.total==((unsigned)c[7]*256+c[8])*512);
        if(c[0]==0x2a) m.writes++;
        else m.read_commands++;
        break;
    default: assert(!"unexpected SCSI command");
    }
}
static void medium_bytes(void *buffer,unsigned n,bool write)
{
    uint64_t pos=(uint64_t)be32(m.cbw+17)*512+m.at;
    uint8_t *b=buffer;
    while(n) {
        unsigned take=n;
        if(pos<(uint64_t)m.offset*512) {
            assert(!write); if(take>512-pos%512)take=512-pos%512;
            if(pos<512)memcpy(b,m.mbr+(unsigned)pos,take);else memset(b,0,take);
        } else {
            bool second=m.second && pos>=(uint64_t)m.second*512;
            uint64_t at=pos-(uint64_t)(second ? m.second : m.offset)*512;
            uint8_t *image=second ? m.second_image : m.image;
            assert(at<m.bytes && take<=m.bytes-at);
            if(write) memcpy(image+at,b,take); else memcpy(b,image+at,take);
        }
        n-=take;b+=take;pos+=take;
    }
}
static int packet(struct usb_device *d,unsigned ep,unsigned pid,unsigned toggle,void *buffer,unsigned n,bool periodic)
{
    (void)d; assert(!periodic && n<=64); m.packets++;
    uint8_t *b=buffer;
    if(!ep) {
        if(pid==USB_PID_SETUP) {assert(n==8);memcpy(m.setup,b,8);return 8;}
        unsigned req=m.setup[1];
        if(req==0xfe && pid==USB_PID_IN && n) {if(m.max_lun_stall)return -USB_EPIPE;b[0]=0;return 1;}
        if(!n) {
            if(req==0xff) {m.resets++;if(m.reset_fail)return -USB_EPIPE;m.stage=0;m.cbw_at=0;m.reset_order=1;}
            if(req==1) {
                if(m.clear_fail) {m.clear_fail=false;return -USB_EPIPE;}
                if(m.setup[4]&0x80){m.clear_in++;m.in_toggle=0;if(m.reset_order){assert(m.reset_order==1);m.reset_order=2;}}
                else{m.clear_out++;m.out_toggle=0;if(m.reset_order){assert(m.reset_order==2);m.reset_order=0;}}
            }
            return 0;
        }
        assert(!"unexpected control");
    }
    assert(ep==(pid==USB_PID_IN ? 1u : 2u));
    if(m.slow_all && m.now-m.command_at<700 && m.stage!=0) return -USB_EAGAIN;
    unsigned *expected=pid==USB_PID_IN ? &m.in_toggle : &m.out_toggle;
    assert(toggle==*expected);
    if(m.timeout) {m.timeout=false;return -USB_ETIMEDOUT;}
    if(m.stage==0) {
        assert(pid==USB_PID_OUT);
        if(m.cbw_stall) {m.cbw_stall=false;return -USB_EPIPE;}
        assert(n<=31-m.cbw_at);memcpy(m.cbw+m.cbw_at,b,n);m.cbw_at+=n;
        if(m.cbw_at==31){m.cbw_at=0;start_command();}
    } else if(m.stage==1) {
        if(m.cbw[15]==0x28 && m.read_commands==1 &&
           m.now-m.command_at<m.first_read_delay) return -USB_EAGAIN;
        if(m.forever_data || (m.nak_capacity && m.cbw[15]==0x25) ||
           (m.nak_mount && m.cbw[15]==0x28) ||
           (m.nak_prepare && m.cbw[15]==0x28 && m.read_commands>=3) ||
           (m.nak_eighth && m.cbw[15]==0x28 && m.read_commands>=3 && m.at==448)) return -USB_EAGAIN;
        if(m.forever_stall && m.cbw[15]==0x12) {failed(3,0x11);m.stage=2;return -USB_EPIPE;}
        if(m.cbw[15]==0x12 && (!m.inquiry_delays || m.inquiries<=m.inquiry_delays) &&
           m.now-m.command_at<m.inquiry_delay) return -USB_EAGAIN;
        if(m.data_babble) {m.injected_status=(1u<<22)|(1u<<20);m.data_babble=false;return -USB_EIO;}
        if(m.data_crc) {m.injected_status=(1u<<22)|(1u<<18);m.data_crc=false;return -USB_EIO;}
        if(m.remove_data) {m.remove_data=false;m.connected=false;return -USB_EIO;}
        if(m.data_stall) {m.data_stall=false;failed(3,0x11);m.stage=2;return -USB_EPIPE;}
        if(m.cbw[15]==0x1a && m.mode_stall) {failed(5,0x20);m.stage=2;return -USB_EPIPE;}
        assert(n<=m.remaining && (pid==USB_PID_IN)==!!(m.cbw[12]&0x80));
        if(m.cbw[15]==0x12 && n>m.inquiry_bytes-m.at) n=m.inquiry_bytes-m.at;
        if(m.cbw[15]==0x25 && n>m.capacity_bytes-m.at) n=m.capacity_bytes-m.at;
        if(m.cbw[15]==0x28 || m.cbw[15]==0x2a) medium_bytes(b,n,pid==USB_PID_OUT);
        else {assert(pid==USB_PID_IN && m.at+n<=sizeof(m.response));memcpy(b,m.response+m.at,n);}
        m.at+=n;m.remaining-=n;if(!m.remaining || (m.cbw[15]==0x12 && m.at==m.inquiry_bytes) ||
            (m.cbw[15]==0x25 && m.at==m.capacity_bytes)){m.stage=2;m.at=0;}
    } else {
        assert(pid==USB_PID_IN);
        if(m.cbw[15]==0x35 && m.now-m.command_at<m.flush_delay) return -USB_EAGAIN;
        if(m.forever_csw) return -USB_EAGAIN;
        if(m.csw_stall){m.csw_stall=false;return -USB_EPIPE;}
        if(m.cbw[15]==0x1a && m.mode_csw_stalls){m.mode_csw_stalls--;return -USB_EPIPE;}
        uint8_t csw[13]={0};fs_wr32(csw,m.bad_signature ? 0 : 0x53425355);
        fs_wr32(csw+4,fs_rd32(m.cbw+4)+(m.bad_tag?1:0));fs_wr32(csw+8,m.bad_residue ? m.total+1 : m.remaining);
        csw[12]=m.phase_error ? 2 : (uint8_t)m.status;
        unsigned take=n;if(m.short_csw && take)take--;
        assert(m.at+take<=13);memcpy(b,csw+m.at,take);m.at+=take;
        if(m.at==13 || m.short_csw){m.stage=0;m.at=0;}
        *expected^=1;return (int)take;
    }
    *expected^=1;return (int)n;
}
static const struct usb_hc_ops ops={.port_status=port_status,.packet=packet};
static struct usb_device *device(void) {return &m.h.devices[m.storage_slot];}
static struct blkdev *block(void) {return &device()->storage.block;}
static void setup(const char *path,bool partitioned)
{
    memset(&m,0,sizeof(m));records_at=0;records[0]=0;m.inquiry_bytes=36;m.capacity_bytes=8;
    m.connected=true;m.sector_size=512;m.offset=partitioned?8:0;m.not_ready_ascq=1;
    int fd=open(path,O_RDONLY);assert(fd>=0);struct stat st;assert(!fstat(fd,&st));m.bytes=(size_t)st.st_size;
    m.image=mmap(NULL,m.bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);assert(m.image!=MAP_FAILED);close(fd);
    if(partitioned) {fs_wr16(m.mbr+510,0xaa55);m.mbr[450]=fs_rd16(m.image+22) ? (fs_rd16(m.image+19) ? 1 : 6) : 0x0c;
        fs_wr32(m.mbr+454,m.offset);fs_wr32(m.mbr+458,(uint32_t)(m.bytes/512));}
    m.h.ready=true;m.h.device_count=1;m.h.ops=&ops;m.h.io.now=now;m.h.io.sleep=sleep_ms;m.h.io.read=read_reg;
    fs_lock_init(&m.h.transfer_lock);
    struct usb_device *d=device();d->host=&m.h;d->present=d->configured=true;d->ep0=64;d->address=1;
    d->storage=(struct usb_storage){.bound=true,.in=1,.out=2,.in_packet=64,.out_packet=64};
    assert(!storage_setup(storage_get(),CACHE_DEFAULT_BYTES));
}
static void use_uhci(void)
{
    m.uhci=true;m.h.ops=&usb_uhci_ops;m.h.dma=m.dma;m.h.dma_phys=0x100000;m.frame_offset=1990;
    m.h.io.read=read_reg;m.h.io.write=write_reg;
    volatile uint32_t *qh=(volatile uint32_t *)(m.dma+USB_UHCI_CONTROL_QH);
    qh[0]=(m.h.dma_phys+USB_UHCI_BULK_QH)|2;qh[1]=1;
    qh=(volatile uint32_t *)(m.dma+USB_UHCI_BULK_QH);qh[0]=qh[1]=1;
    for(unsigned i=0;i<1024;i++) ((uint32_t *)m.dma)[i]=(m.h.dma_phys+USB_ED)|2;
}
static void cleanup(void)
{storage_destroy(storage_get());assert(!fs_host_contig_live);fs_lock_destroy(&m.h.transfer_lock);assert(!munmap(m.image,m.bytes));if(m.second_image)assert(!munmap(m.second_image,m.bytes));}
static void attach(void)
{assert(!usb_storage_attach(device()));}
static void mount_device(void) {attach();usb_storage_service(&m.h);assert(storage_get()->vfs.volumes[4]);}
static unsigned clean_flag(void)
{
    struct fat_volume *f=&storage_get()->volumes[4].fat;
    if(f->type==12)return 1;
    unsigned at=f->reserved*512+f->type/8;
    return f->type==16 ? !!(fs_rd16(m.image+at)&0x8000) : !!(fs_rd32(m.image+at)&0x08000000);
}
static void io_mount(const char *path,bool partitioned,unsigned bits)
{
    setup(path,partitioned);m.max_lun_stall=true;m.initial_ua=1;mount_device();
    struct storage *s=storage_get();assert(s->volumes[4].fat.type==bits && !s->volumes[4].fat.readonly);
    assert(s->volumes[4].part.start==m.offset && s->vfs.drive_flags[4]==VFS_DRIVE_REMOVABLE);
    assert(s->volumes[4].io.media_generation==block()->media_generation);
    struct vfs_table *t=calloc(1,sizeof(*t));assert(t && !vfs_table_init(&s->vfs,t,4));
    int h=vfs_open(t,"E:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0);assert(h>=0);
    uint8_t readback[512],payload[512];size_t done;
    assert(!vfs_read(t,h,readback,sizeof(readback),&done) && done==sizeof(readback));
    unsigned writes=m.writes,flushes=m.flushes;
    assert(usb_storage_eject(s,4,block()->media_generation)==-FS_EBUSY);
    assert(m.writes==writes && m.flushes==flushes && s->vfs.volumes[4]);
    assert(!vfs_close(t,h));
    h=vfs_open(t,"E:/USB copy.bin",VFS_READ|VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,VFS_DENY_NONE,0);assert(h>=0);
    memset(payload,0xa7,sizeof(payload));assert(!vfs_write(t,h,payload,sizeof(payload),&done)&&done==sizeof(payload));
    uint64_t pos;assert(!vfs_seek(t,h,0,VFS_SEEK_SET,&pos));
    assert(!vfs_read(t,h,readback,sizeof(readback),&done)&&done==sizeof(readback)&&!memcmp(payload,readback,done));
    assert(!vfs_commit(t,h) && !vfs_close(t,h));
    uint32_t gen=block()->media_generation;
    assert(usb_storage_eject(s,4,gen+1)==-FS_EIO && s->vfs.volumes[4]);
    assert(!usb_storage_eject(s,4,gen));assert(!s->vfs.volumes[4] && clean_flag() && device()->storage.ejected);
    unsigned commands=m.commands;assert(block()->read(block(),0,1,readback)<0 && m.commands==commands);
    vfs_table_destroy(t);free(t);cleanup();
    printf("PASS USB FAT%u %s: read/copy/write/flush, E:, busy/stale eject, clean detach, disabled I/O\n",bits,partitioned?"MBR":"whole-disk");
}
static void errors(const char *path)
{
    for(unsigned mode=0;mode<10;mode++) {
        setup(path,false);mount_device();uint8_t b[512];unsigned before=m.commands;
        switch(mode) {
        case 0:m.cbw_stall=true;break;case 1:m.data_stall=true;break;case 2:m.phase_error=true;break;
        case 3:m.bad_tag=true;break;case 4:m.bad_signature=true;break;case 5:m.bad_residue=true;break;
        case 6:m.short_csw=true;break;case 7:m.timeout=true;break;case 8:m.remove_data=true;break;case 9:m.ua=true;break;
        }
        assert(block()->read(block(),0,1,b)<0 && block()->quarantined);
        unsigned stopped=m.commands;assert(stopped>=before && block()->write(block(),1,1,b)<0 && m.commands==stopped);
        usb_storage_service(&m.h);assert(!storage_get()->vfs.volumes[4]);cleanup();
    }
    setup(path,false);mount_device();m.remove_data=true;uint8_t write_buffer[512]={0};unsigned issued=m.writes;
    assert(block()->write(block(),1,1,write_buffer)<0 && m.writes==issued+1 && block()->quarantined);
    unsigned commands=m.commands;assert(block()->write(block(),1,1,write_buffer)<0 && m.commands==commands);cleanup();
    setup(path,false);attach();m.csw_stall=true;uint8_t b[512];assert(!block()->read(block(),0,1,b)&&m.clear_in==1&&!block()->quarantined);cleanup();
    setup(path,false);mount_device();m.fail_flush=true;
    assert(usb_storage_eject(storage_get(),4,block()->media_generation)<0);
    assert(!device()->storage.ejected && block()->quarantined && !clean_flag());
    usb_storage_service(&m.h);assert(!storage_get()->vfs.volumes[4]);cleanup();
    setup(path,false);mount_device();m.no_medium=true;m.now+=2000;usb_storage_service(&m.h);
    assert(!storage_get()->vfs.volumes[4] && block()->quarantined);cleanup();
    puts("PASS USB BOT errors: CBW/data/CSW stalls, phase/tag/signature/residue/length/timeout, I/O removal, media change/absence, failed flush, no write retry");
}
static void partitions(const char *path)
{
    setup(path,true);m.second=m.offset+(unsigned)(m.bytes/512)+8;
    int fd=open(path,O_RDONLY);assert(fd>=0);
    m.second_image=mmap(NULL,m.bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);assert(m.second_image!=MAP_FAILED);close(fd);
    m.mbr[466]=m.mbr[450];fs_wr32(m.mbr+470,m.second);fs_wr32(m.mbr+474,(uint32_t)(m.bytes/512));
    mount_device();struct storage *s=storage_get();assert(s->vfs.volumes[4] && s->vfs.volumes[5]);
    assert(storage_add_removable(s,block())==-FS_EBUSY);
    struct vfs_table *t=calloc(1,sizeof(*t));assert(t && !vfs_table_init(&s->vfs,t,5));
    int h=vfs_open(t,"F:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0);assert(h>=0);
    unsigned writes=m.writes,flushes=m.flushes;
    assert(usb_storage_eject(s,4,block()->media_generation)==-FS_EBUSY && s->vfs.volumes[4] && s->vfs.volumes[5]);
    assert(m.writes==writes && m.flushes==flushes);assert(!vfs_close(t,h));
    uint8_t b[512];unsigned commands=m.commands;
    assert(block()->read(block(),block()->capacity,1,b)==-FS_EINVAL);
    assert(block()->read(block(),0,0,b)==-FS_EINVAL && block()->read(block(),0,129,b)==-FS_EINVAL && m.commands==commands);
    assert(!usb_storage_eject(s,5,block()->media_generation) && !s->vfs.volumes[4] && !s->vfs.volumes[5]);
    vfs_table_destroy(t);free(t);cleanup();
    puts("PASS USB multiple partitions: E:/F: offset views, busy peer preflight without writes/flush, whole-device removal, duplicate/range/request bounds");
}
static void readonly(const char *path)
{
    setup(path,false);m.no_flush=true;mount_device();assert(storage_get()->volumes[4].fat.readonly && !m.writes);cleanup();
    setup(path,false);m.wp=true;mount_device();assert(storage_get()->volumes[4].fat.readonly && !m.writes);cleanup();
    setup(path,false);unsigned reserved=fs_rd16(m.image+14),fats=m.image[16],fsz=fs_rd16(m.image+22);
    assert(fsz);
    for(unsigned i=0;i<fats;i++) {uint8_t *flag=m.image+(reserved+i*fsz)*512+2;fs_wr16(flag,fs_rd16(flag)&~0x8000u);}
    mount_device();assert(!storage_get()->volumes[4].fat.readonly && m.writes);
    assert(!storage_enable_write(storage_get(),4));cleanup();
    setup(path,false);m.sector_size=4096;assert(usb_storage_attach(device())<0);
    assert(!device()->storage.online && !m.writes);cleanup();
    puts("PASS USB write admission: unsupported flush, protected FAT without writes, dirty FAT repaired, non-512 sectors rejected");
}
static void uhci_transport(const char *path)
{
    /* Real frame-paced TD processing, descriptor MPS=64, initial DATA0.
     * The device NAKs INQUIRY while preparing its response, with no ACK or
     * toggle change. This exposed the old 500 ms packet timeout. */
    setup(path,false);use_uhci();m.inquiry_delay=750;attach();
    assert(m.naks>USB_TIMEOUT_MS && m.commands==5 && device()->storage.online);
    assert(!m.resets && !m.clear_in && !m.clear_out && !m.writes);
    cleanup();
    setup(path,false);use_uhci();m.inquiry_delay=USB_BIND_BULK_MS+500;m.late_ack=true;attach();
    assert(m.late_acks==1 && m.commands==5 && !m.resets && device()->storage.online);cleanup();
    /* A short standard INQUIRY is accepted only with consistent residue.
     * 36/64 is the usual short USB packet; these are genuinely short CDB
     * responses to the existing 36-byte allocation. */
    for(unsigned bytes=5;bytes<=35;bytes+=30) {
        setup(path,false);use_uhci();m.inquiry_bytes=bytes;attach();
        assert(m.commands==5 && device()->storage.online && !m.resets);cleanup();
    }
    setup(path,false);use_uhci();m.inquiry_bytes=4;
    assert(usb_storage_attach(device())<0);
    assert(block()->quarantined && m.commands==1 && !m.writes);
    assert(strstr(records,"reason=short_inquiry") && strstr(records,"status=0 residue=32"));cleanup();
    setup(path,false);use_uhci();m.inquiry_delay=USB_BIND_BULK_MS+500;
    assert(usb_storage_attach(device())<0);
    struct usb_storage_trace *t=&device()->storage.trace;
    assert(t->tag==2 && !strcmp(t->stage,"data") && t->expected==36 && !t->actual);
    assert(t->result==-USB_ETIMEDOUT && t->packet.result==-USB_ETIMEDOUT && t->packet.actual==-1);
    assert(t->packet.valid && (t->packet.status&(1u<<23)) && (t->packet.status&(1u<<19)));
    assert(t->packet.endpoint==1 && t->packet.pid==USB_PID_IN && !t->packet.toggle);
    assert(t->packet.frames==USB_BIND_BULK_MS+2 && t->packet.elapsed_ms==USB_BIND_BULK_MS+2 && !t->packet.retired);
    char timing[96];snprintf(timing,sizeof(timing),"frames=%u elapsed_ms=%u retired=0",USB_BIND_BULK_MS+2,USB_BIND_BULK_MS+2);
    assert(strstr(records,"stage=data expected=36 actual=0 result=-110") && strstr(records,timing));
    assert(m.commands==2 && m.resets==2 && m.clear_in==2 && m.clear_out==2 && !m.writes && !m.reset_order);
    assert(!device()->storage.bulk_deadline && !device()->storage.in_toggle && !device()->storage.out_toggle);
    cleanup();
    setup(path,false);use_uhci();m.data_stall=true;
    attach();
    assert(device()->storage.online && m.commands==7 && m.clear_in==2 && m.clear_out==1 && m.resets==1 && !m.writes);
    assert(strstr(records,"stall addr=1 reason=data tag=1") && strstr(records,"stage=data expected=36 actual=0 result=-32"));
    assert(strstr(records,"csw_tag=1 status=1 residue=36"));cleanup();
    setup(path,false);use_uhci();m.data_babble=true;
    assert(usb_storage_attach(device())<0);
    assert(block()->quarantined && m.commands==1 && m.resets==1 && m.clear_in==1 && m.clear_out==1);
    assert(device()->storage.trace.packet.status&1u<<20);
    assert(device()->storage.trace.result==-USB_EIO && !strstr(records,"stall addr="));cleanup();
    setup(path,false);use_uhci();m.data_stall=true;m.clear_fail=true;
    assert(usb_storage_attach(device())<0);
    assert(block()->quarantined && m.commands==1 && m.resets==1);
    assert(strstr(records,"reason=data_clear_halt") && device()->storage.trace.packet.status&1u<<22);cleanup();
    setup(path,false);use_uhci();m.bad_tag=true;
    assert(usb_storage_attach(device())<0);
    assert(strstr(records,"stage=csw expected=13 actual=13") && strstr(records,"csw_tag=2 status=0 residue=0"));
    assert(device()->storage.trace.packet.toggle==1 && m.commands==1 && m.resets==1);cleanup();
    setup(path,false);use_uhci();attach();m.csw_stall=true;uint8_t readback[512];
    assert(!block()->read(block(),0,1,readback) && m.clear_in==1 && !m.resets);
    assert(strstr(records,"stall addr=1 reason=csw") && !block()->quarantined);cleanup();
    setup(path,false);use_uhci();attach();m.data_stall=true;uint8_t buffer[512]={0};
    assert(block()->write(block(),1,1,buffer)<0 && m.writes==1 && block()->quarantined);
    unsigned commands=m.commands;
    assert(block()->write(block(),1,1,buffer)<0 && m.commands==commands && m.writes==1);cleanup();
    puts("PASS USB UHCI/BOT: 1 ms frames, 64-byte MPS, delayed/short INQUIRY, late ACK retirement, first data STALL, CSW stall/tag diagnostics, babble, preserved fault snapshots, wrapped frame count, reset ordering, no write resend");
}
static void complete_fault_record(void)
{
    const char *line=strstr(records,"[usb-storage] fault ");assert(line);
    const char *end=strchr(line,'\n');assert(end && end-line<=254);
    const char *fields[]={"stage=","tag=","expected=","actual=","td=","toggle=","csw=","status=","residue=","port="};
    for(unsigned i=0;i<ARRAY_SIZE(fields);i++) {const char *field=strstr(line,fields[i]);assert(field && field<end);}
    assert(!strstr(end,"[usb-storage] fault "));
}
static void attach_tolerance(const char *path)
{
    setup(path,false);use_uhci();m.inquiry_delay=USB_BIND_BULK_MS+500;m.inquiry_delays=1;attach();
    assert(m.inquiries==2 && m.resets==1 && m.clear_in==1 && m.clear_out==1 && !m.writes);
    assert(device()->storage.online && !block()->quarantined && !device()->storage.bulk_deadline);
    complete_fault_record();assert(strstr(records,"reason=data_transfer tag=1 cmd=12 stage=data expected=36 actual=0 result=-110"));
    assert(!strstr(records,"quarantine addr="));cleanup();
    /* A persistent slow first response has one retry, one first-fault
     * snapshot and terminal quarantine naming the failing stage. */
    setup(path,false);use_uhci();m.inquiry_delay=USB_BIND_BULK_MS+500;
    assert(usb_storage_attach(device())<0);
    assert(m.inquiries==2 && block()->quarantined && !m.writes);complete_fault_record();
    assert(strstr(records,"quarantine addr=1 reason=data_transfer stage=inquiry transfer=data"));cleanup();
    setup(path,false);use_uhci();m.inquiry_delay=USB_BIND_BULK_MS+500;m.reset_fail=true;
    assert(usb_storage_attach(device())<0);
    assert(m.inquiries==1 && m.resets==1 && !m.clear_in && !m.clear_out && block()->quarantined);
    complete_fault_record();assert(strstr(records,"quarantine addr=1 reason=reset_recovery stage=inquiry transfer=data"));cleanup();
    setup(path,false);use_uhci();m.data_crc=true;
    assert(usb_storage_attach(device())<0);
    assert(m.inquiries==1 && block()->quarantined && (device()->storage.trace.packet.status&(1u<<18)));
    complete_fault_record();assert(!strstr(records,"stall addr="));cleanup();
    for(unsigned mode=0;mode<3;mode++) {
        setup(path,false);use_uhci();m.mode_stall=true;
        if(mode==1)m.mode_csw_stalls=2;
        if(mode==2)m.no_flush=true;
        attach();assert(m.inquiries==1 && m.modes==1 && !block()->quarantined && !m.writes);
        assert(device()->storage.online && !device()->storage.write_protected);
        assert(blkdev_durable(block())==(mode!=2));
        assert(strstr(records,"mode_sense addr=1 reason=stalled assume_writable=1"));
        assert(m.resets==(mode==1 ? 1u:0u));
        uint8_t b[512];assert(!block()->read(block(),0,1,b));
        if(mode!=2)assert(!block()->write(block(),1,1,b) && m.writes==1);
        else assert(block()->write(block(),1,1,b)==-FS_EROFS && !m.writes);
        cleanup();
    }
    for(unsigned bytes=0;bytes<8;bytes++) {
        setup(path,false);use_uhci();m.capacity_bytes=bytes;
        assert(usb_storage_attach(device())<0);
        assert(block()->quarantined && !device()->storage.online && !block()->capacity && !m.modes && !m.writes);
        complete_fault_record();cleanup();
    }
    setup(path,false);use_uhci();m.no_medium=true;m.mode_stall=true;
    assert(usb_storage_attach(device())<0);
    assert(block()->quarantined && !device()->storage.online && !m.modes && !m.writes);cleanup();
    puts("PASS USB F1-53 attach: one reset-qualified INQUIRY retry, persistent timeout/reset failure/CRC quarantine, optional MODE SENSE STALL, DATA0 recovery, short capacity rejection and readiness/write/flush admission");
}
static void failed_mount(const char *path)
{
    for(unsigned mode=0;mode<4;mode++) {
        setup(path,true);use_uhci();
        if(mode==3) {m.inquiry_delay=USB_BIND_BULK_MS+500;m.inquiry_delays=1;}
        attach();
        if(mode==0)m.nak_prepare=true;
        if(mode==1 || mode==3)m.nak_eighth=true;
        if(mode==2)memset(m.image,0,512); /* Recognized FAT partition, invalid BPB. */
        usb_storage_service(&m.h);
        struct storage *s=storage_get();struct usb_storage *u=&device()->storage;
        assert(strstr(records,"[usb-storage] volume drive=E part=1 "));
        assert(s->volumes[4].error && !s->volumes[4].fat.mounted);
        if(u->published)fprintf(stderr,"F1-60 failed mount: published=%u volume_error=%d VFS_E=%u\n",
            u->published,s->volumes[4].error,vfs_drive_present(&s->vfs,4));
        assert(!u->published && !u->online && !u->binding && device()->quarantined);
        if(mode!=2)assert(strstr(records,"quarantine addr=1 reason=data_transfer stage=mount transfer=data"));
        else assert(strstr(records,"quarantine addr=1 reason=mount_failed stage=mount"));
        if(mode==1 || mode==3) {
            struct usb_storage_trace *t=&u->trace;
            assert(t->expected==512 && t->actual==448 && t->packet_count==8 && t->packet_index==7);
            assert(t->packet_offset==448 && t->packet.result==-USB_ETIMEDOUT);
            unsigned initial=t->packets[0].toggle;
            for(unsigned i=0;i<8;i++) {
                const struct usb_packet_trace *p=&t->packets[i];
                assert(p->valid && p->toggle==(initial^(i&1)) && !p->retired);
                assert(p->actual==(i<7 ? 64:-1) && p->result==(i<7 ? 64:-USB_ETIMEDOUT));
                char needle[96];snprintf(needle,sizeof(needle),"[usb-storage] packet addr=1 tag=%u index=%u toggle=%u",t->tag,i,p->toggle);
                assert(strstr(records,needle));
            }
            assert(strstr(records,"packets=8 index=7 offset=448 stop=packet"));
            assert(strstr(records,"expected=64 actual=-1 result=-110 td=188807ff"));
            /* Recovery control packets reuse DMA, but not the data-stage history. */
            assert(device()->packet_trace.pid!=USB_PID_IN || device()->packet_trace.expected!=64);
            if(mode==3)assert(strstr(records,"[usb-storage] terminal_fault addr=1 reason=data_transfer"));
        }
        for(unsigned d=4;d<26;d++)
            assert(!vfs_drive_present(&s->vfs,d) && !s->volumes[d].present && !s->vfs.drive_flags[d]);
        struct vfs_table table;assert(!vfs_table_init(&s->vfs,&table,2));
        assert(vfs_open(&table,"E:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0)<0);
        vfs_table_destroy(&table);
        unsigned commands=m.commands;size_t logged=records_at;
        for(unsigned i=0;i<3;i++)usb_storage_service(&m.h);
        assert(m.commands==commands && records_at==logged && !m.writes);
        cleanup();
    }
    puts("PASS F1-60 failed FAT preparation: volume error, mount quarantine, unpublished USB/VFS/drive flags, E: refused, no rebind, eighth-packet trace survives reset, invalid BPB");
}
static void readiness_bounds(const char *path)
{
    for(unsigned kind=0;kind<3;kind++) {
        setup(path,false);
        if(kind==0) {m.never_ready=true;m.not_ready_ascq=2;} /* initialization required */
        if(kind==1)m.initial_ua=3;
        if(kind==2) {m.never_ready=true;m.frozen_clock=true;}
        assert(usb_storage_attach(device())<0);
        assert(block()->quarantined && device()->quarantined && !m.writes);
        assert(m.ready_commands==(kind==1 ? 3u : kind==0 ? 1u : USB_BIND_TIMEOUT_MS/100u+1));
        assert(!device()->storage.bind_reserved && !m.h.busy);
        assert(strstr(records,"reason=not_ready stage=ready"));
        assert(strstr(records,"stage=ready addr=1 elapsed_ms="));
        cleanup();
    }
    puts("PASS F1-62 readiness: initialization-required fails immediately, UNIT ATTENTION limited to three probes, frozen clock has finite polling");
}
static void prepare_validation(const char *path)
{
    for(unsigned kind=0;kind<4;kind++) {
        setup(path,true);use_uhci();
        unsigned reserved=fs_rd16(m.image+14), sectors=fs_rd32(m.image+36);
        if(kind==0) {
            fs_wr32(m.image+(reserved+sectors)*512+127*4,0x0fffffff);
        } else if(kind==1) {
            for(unsigned copy=0;copy<2;copy++)fs_wr32(m.image+(reserved+copy*sectors)*512+127*4,0x0fffffff);
        } else if(kind==2)fs_host_contig_fail=1;
        else {
            unsigned total=fs_rd32(m.image+32), clusters=(total-reserved-2*sectors)/m.image[13];
            m.image[(reserved+sectors)*512+(clusters+2)*4]^=0xa5;
        }
        /* Agreeing clean copies take the F1-70 fast path. These repair
         * fixtures model interrupted allocation, with the dirty flag set. */
        if(kind<2) for(unsigned copy=0;copy<2;copy++)
            fs_wr32(m.image+(reserved+copy*sectors)*512+4,0x07ffffff);
        attach();usb_storage_service(&m.h);
        if(kind<2) {
            struct storage_volume *v=&storage_get()->volumes[4];
            assert(device()->storage.published && !v->fat.readonly && m.writes);
            assert(!v->fat.ro_reasons && v->fat.repaired_lost==1);
            if (!kind) assert(v->fat.repaired_copies==1);
            assert(!v->fat.prepare_cache);
        } else if(kind==2)assert(!device()->storage.published && block()->quarantined && !m.writes);
        else assert(device()->storage.published && !storage_get()->volumes[4].fat.readonly);
        cleanup();
    }
    puts("PASS F1-66 private mount windows repair unowned copy/lost-cluster allocation, fail closed on allocation failure and release all pages");
}
static bool budget_log_delay;
static void delay_after_publication(const char *line,size_t n)
{
    (void)n;
    if(budget_log_delay && strstr(line,"[usb-storage] volume drive=E")) {
        assert(device()->storage.published && !device()->storage.binding);
        budget_log_delay=false;m.now+=USB_BIND_TIMEOUT_MS;
    }
}
static void fat32_budget(const char *path)
{
    for(unsigned gib=4;gib<=32;gib*=8) {
    setup(path,true);use_uhci();
    size_t mapped=m.bytes;
    unsigned total=gib*2097152u,fat_sectors=gib*256u,data_start=32+2*fat_sectors;
    memset(m.image,0,(data_start+128)*512);
    uint8_t *b=m.image;
    b[0]=0xeb;b[1]=0x58;b[2]=0x90;fs_wr16(b+11,512);b[13]=64;
    fs_wr16(b+14,32);b[16]=2;b[21]=0xf8;
    fs_wr32(b+32,total);fs_wr32(b+36,fat_sectors);fs_wr32(b+44,2);
    fs_wr16(b+48,1);fs_wr16(b+50,6);fs_wr16(b+510,0xaa55);
    memcpy(b+6*512,b,512);
    fs_wr32(b+512,0x41615252);fs_wr32(b+512+484,0x61417272);fs_wr32(b+512+508,0xaa550000);
    fs_wr32(b+512+488,UINT32_MAX);fs_wr32(b+512+492,UINT32_MAX);
    for(unsigned copy=0;copy<2;copy++) {
        uint8_t *fat=b+(32+copy*fat_sectors)*512;
        fs_wr32(fat,0x0ffffff8);fs_wr32(fat+4,0x0fffffff);
        fs_wr32(fat+8,0x0fffffff);fs_wr32(fat+12,0x0fffffff);
    }
    uint8_t *root=b+data_start*512;memcpy(root,"SHORT   BIN",11);root[11]=FAT_ATTR_ARCHIVE;
    fs_wr16(root+26,3);fs_wr32(root+28,512);memset(root+64*512,0x5a,512);
    m.bytes=(uint64_t)gib*1024*1024*1024;
    m.mbr[450]=0x0c;fs_wr32(m.mbr+458,total);
    uint64_t begin=m.now;
    mount_device();
    uint64_t bind_ms=m.now-begin;
    struct usb_storage *s=&device()->storage;
    assert(bind_ms<(gib==4 ? 5000u : USB_BIND_TIMEOUT_MS) && s->published && !block()->quarantined && !storage_get()->volumes[4].fat.readonly);
    assert(s->bind_read_max==128 && s->bind_read_requests<gib*16 && s->bind_read_sectors<gib*1024);
    struct vfs_table table;assert(!vfs_table_init(&storage_get()->vfs,&table,4));
    int fd=vfs_open(&table,"E:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0);assert(fd>=0);
    uint8_t data[512];size_t got;assert(!vfs_read(&table,fd,data,sizeof(data),&got) && got==512);
    for(unsigned i=0;i<512;i++)assert(data[i]==0x5a);
    assert(!vfs_close(&table,fd));vfs_table_destroy(&table);
    printf("PASS F1-70 %u GiB FAT32, 32 KiB clusters: bind_ms=%llu requests=%u sectors=%u max_sectors=%u poll_frames=%u retire_frames=%u, E: read/write gate retained\n",
           gib,(unsigned long long)bind_ms,s->bind_read_requests,s->bind_read_sectors,s->bind_read_max,s->bind_poll_frames,s->bind_retire_frames);
    m.bytes=mapped;cleanup();
    }
    setup(path,true);use_uhci();budget_log_delay=true;log_hook=delay_after_publication;
    mount_device();log_hook=NULL;
    assert(!budget_log_delay && device()->storage.published && !device()->storage.binding && !block()->quarantined);
    assert(strstr(records,"published=1 overrun=1") && !strstr(records,"quarantine"));
    uint8_t data[512];
    assert(!block()->read(block(),m.offset,1,data));cleanup();
    puts("PASS F1-66 post-publication log delay exceeds 20 s: overrun recorded, volume retained and readable, no deadline quarantine");
}
int main(int argc,char **argv)
{
    assert(argc==4);
    if(getenv("USB_TEST_F1_60_MOUNT_ONLY")) {failed_mount(argv[2]);return 0;}
    prepare_validation(argv[3]);fat32_budget(argv[3]);failed_mount(argv[2]);
    if(getenv("USB_TEST_F1_47_ONLY")) {uhci_transport(argv[2]);return 0;}
    uhci_transport(argv[2]);attach_tolerance(argv[2]);readiness_bounds(argv[2]);
    io_mount(argv[1],false,12);io_mount(argv[2],true,16);io_mount(argv[3],true,32);
    errors(argv[2]);readonly(argv[2]);partitions(argv[2]);
    puts("PASS USB storage production suite (ASan/UBSan)");return 0;
}
