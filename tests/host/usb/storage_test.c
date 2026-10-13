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
struct ciuki_boot_info g_boot;
static gen_t generation;
gen_t gen_alloc(void) { return ++generation; }
void klog(const char *fmt,...) { if (!getenv("USB_TEST_LOG")) return; va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);fputc(10,stderr); }
/* usb_attach is intentionally not used by this packet-level peripheral model;
 * the register/DMA engines have their own production suite. */
void usb_hid_release(struct usb_device *d,struct usb_hid *h) { (void)d;(void)h; }
const struct usb_hc_ops usb_uhci_ops={0},usb_ohci_ops={0};
static struct model {
    struct usb_host h;
    uint8_t *image, *second_image;
    unsigned second;
    size_t bytes;
    uint8_t setup[8], cbw[31], response[36], sense[18], mbr[512];
    unsigned cbw_at, at, remaining, total, offset, status, in_toggle, out_toggle;
    unsigned commands, writes, flushes, resets, clear_in, clear_out, packets;
    unsigned stage; /* 0 CBW, 1 data, 2 CSW */
    uint64_t now;
    bool connected, max_lun_stall, no_flush, wp, phase_error, bad_tag, bad_signature, bad_residue, short_csw;
    bool data_stall, csw_stall, cbw_stall, timeout, remove_data, ua, no_medium, fail_flush;
    unsigned sector_size, initial_ua;
} m;
static uint32_t be32(const uint8_t *b) {return (uint32_t)b[0]<<24|(uint32_t)b[1]<<16|(uint32_t)b[2]<<8|b[3];}
static void put32(uint8_t *b,uint32_t v) {b[0]=v>>24;b[1]=v>>16;b[2]=v>>8;b[3]=v;}
static uint64_t now(void *c) { (void)c; return m.now; }
static void sleep_ms(void *c,unsigned ms) { (void)c; m.now+=ms; }
static unsigned port_status(struct usb_host *h,unsigned p)
{ (void)h; assert(p==0); return m.connected ? 3 : 0; }
static void failed(unsigned key,unsigned asc)
{
    memset(m.sense,0,sizeof(m.sense)); m.sense[0]=0x70; m.sense[2]=(uint8_t)key;
    m.sense[7]=10; m.sense[12]=(uint8_t)asc; m.status=1;
}
static void start_command(void)
{
    assert(fs_rd32(m.cbw)==0x43425355 && m.cbw[13]==0 && m.cbw[14]>=6 && m.cbw[14]<=10);
    m.commands++; m.status=0; m.total=fs_rd32(m.cbw+8); m.remaining=m.total; m.at=0;
    m.stage=m.remaining ? 1 : 2;
    const uint8_t *c=m.cbw+15; memset(m.response,0,sizeof(m.response));
    switch(c[0]) {
    case 0x12: assert(m.total==36);m.response[1]=0x80;m.response[2]=2;m.response[4]=31;break;
    case 0x03: assert(m.total==18);memcpy(m.response,m.sense,18);break;
    case 0x00:
        assert(!m.total);
        if(m.ua || m.initial_ua) {m.ua=false;if(m.initial_ua)m.initial_ua--;failed(6,0x28);}
        else if(m.no_medium) failed(2,0x3a);
        break;
    case 0x25: assert(m.total==8);put32(m.response,(uint32_t)(m.bytes/512+(m.second ? m.second : m.offset)-1));put32(m.response+4,m.sector_size);break;
    case 0x35:
        assert(!m.total);m.flushes++;
        if(m.no_flush) failed(5,0x20); else if(m.fail_flush) failed(4,0x44);
        break;
    case 0x1a: assert(m.total==4); m.response[0]=3;m.response[2]=m.wp ? 0x80:0;break;
    case 0x28: case 0x2a:
        assert(m.total==((unsigned)c[7]*256+c[8])*512);
        if(c[0]==0x2a) m.writes++;
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
    (void)d; assert(!periodic && n<=64); m.packets++;m.now++;
    uint8_t *b=buffer;
    if(!ep) {
        if(pid==USB_PID_SETUP) {assert(n==8);memcpy(m.setup,b,8);return 8;}
        unsigned req=m.setup[1];
        if(req==0xfe && pid==USB_PID_IN && n) {if(m.max_lun_stall)return -USB_EPIPE;b[0]=0;return 1;}
        if(!n) {
            if(req==0xff) {m.resets++;m.stage=0;m.cbw_at=0;}
            if(req==1) {if(m.setup[4]&0x80){m.clear_in++;m.in_toggle=0;}else{m.clear_out++;m.out_toggle=0;}}
            return 0;
        }
        assert(!"unexpected control");
    }
    assert(ep==(pid==USB_PID_IN ? 1u : 2u));
    unsigned *expected=pid==USB_PID_IN ? &m.in_toggle : &m.out_toggle;
    assert(toggle==*expected);
    if(m.timeout) {m.timeout=false;return -USB_ETIMEDOUT;}
    if(m.stage==0) {
        assert(pid==USB_PID_OUT);
        if(m.cbw_stall) {m.cbw_stall=false;return -USB_EPIPE;}
        assert(n<=31-m.cbw_at);memcpy(m.cbw+m.cbw_at,b,n);m.cbw_at+=n;
        if(m.cbw_at==31){m.cbw_at=0;start_command();}
    } else if(m.stage==1) {
        if(m.remove_data) {m.remove_data=false;m.connected=false;return -USB_EIO;}
        if(m.data_stall) {m.data_stall=false;failed(3,0x11);m.stage=2;return -USB_EPIPE;}
        assert(n<=m.remaining && (pid==USB_PID_IN)==!!(m.cbw[12]&0x80));
        if(m.cbw[15]==0x28 || m.cbw[15]==0x2a) medium_bytes(b,n,pid==USB_PID_OUT);
        else {assert(pid==USB_PID_IN && m.at+n<=sizeof(m.response));memcpy(b,m.response+m.at,n);}
        m.at+=n;m.remaining-=n;if(!m.remaining){m.stage=2;m.at=0;}
    } else {
        assert(pid==USB_PID_IN);
        if(m.csw_stall){m.csw_stall=false;return -USB_EPIPE;}
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
static struct usb_device *device(void) {return &m.h.devices[0];}
static struct blkdev *block(void) {return &device()->storage.block;}
static void setup(const char *path,bool partitioned)
{
    memset(&m,0,sizeof(m));m.connected=true;m.sector_size=512;m.offset=partitioned?8:0;
    int fd=open(path,O_RDONLY);assert(fd>=0);struct stat st;assert(!fstat(fd,&st));m.bytes=(size_t)st.st_size;
    m.image=mmap(NULL,m.bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);assert(m.image!=MAP_FAILED);close(fd);
    if(partitioned) {fs_wr16(m.mbr+510,0xaa55);m.mbr[450]=fs_rd16(m.image+22) ? (fs_rd16(m.image+19) ? 1 : 6) : 0x0c;
        fs_wr32(m.mbr+454,m.offset);fs_wr32(m.mbr+458,(uint32_t)(m.bytes/512));}
    m.h.ready=true;m.h.device_count=1;m.h.ops=&ops;m.h.io.now=now;m.h.io.sleep=sleep_ms;fs_lock_init(&m.h.transfer_lock);
    struct usb_device *d=device();d->host=&m.h;d->present=d->configured=true;d->ep0=64;d->address=1;
    d->storage=(struct usb_storage){.bound=true,.in=1,.out=2,.in_packet=64,.out_packet=64};
    assert(!storage_setup(storage_get(),CACHE_DEFAULT_BYTES));
}
static void cleanup(void)
{storage_destroy(storage_get());fs_lock_destroy(&m.h.transfer_lock);assert(!munmap(m.image,m.bytes));if(m.second_image)assert(!munmap(m.second_image,m.bytes));}
static void attach(void)
{fs_lock_take(&m.h.transfer_lock);assert(!usb_storage_attach(device()));fs_lock_drop(&m.h.transfer_lock);}
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
    mount_device();assert(storage_get()->volumes[4].fat.readonly && !m.writes);
    assert(storage_enable_write(storage_get(),4)==-FS_EROFS && !m.writes);cleanup();
    setup(path,false);m.sector_size=4096;fs_lock_take(&m.h.transfer_lock);assert(usb_storage_attach(device())<0);fs_lock_drop(&m.h.transfer_lock);
    assert(!device()->storage.online && !m.writes);cleanup();
    puts("PASS USB write admission: unsupported flush, protected/dirty FAT without writes/recovery, non-512 sectors rejected");
}
int main(int argc,char **argv)
{
    assert(argc==4);
    io_mount(argv[1],false,12);io_mount(argv[2],true,16);io_mount(argv[3],true,32);
    errors(argv[2]);readonly(argv[2]);partitions(argv[2]);
    puts("PASS USB storage production suite (ASan/UBSan)");return 0;
}
