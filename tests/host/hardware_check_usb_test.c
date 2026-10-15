/* Execute the page's production USB wait/read logic against asynchronous
 * worker milestones, including the supplied no-first-scan failure. */
#include <assert.h>
#include <stdio.h>
#include <ciuki/kernel.h>
#include <ciuki/storage.h>
#include <ciuki/usb.h>
#include <ciuki/hardware_check.h>
static struct storage s;
static struct usb_host h;
static uint64_t clock_ms, scan_at, mount_at;
static unsigned count=1, reads;
static bool no_scan, no_mount, read_error;
unsigned usb_count(void) { return count; }
const struct usb_host *usb_get(unsigned i) { return i<count ? &h : NULL; }
struct storage *storage_get(void) { return &s; }
static uint64_t now(void *ctx) { (void)ctx;return clock_ms; }
static int read_blocks(struct blkdev *d,uint64_t lba,uint32_t n,void *buf) {
    assert(!blkdev_range(d,lba,n) && n==128 && lba==reads*128);
    reads++;clock_ms+=20;memset(buf,0x5a,n*512);
    return read_error ? -FS_EIO : 0;
}
void task_sleep_ms(uint32_t ms) {
    assert(ms>0 && ms<=10);clock_ms+=ms;
    if (!no_scan && clock_ms>=scan_at) h.scanned=true;
    if (!no_mount && clock_ms>=mount_at) {
        s.volumes[4].present=true;s.volumes[4].fat.mounted=true;
    }
}
static void reset(void) {
    memset(&s,0,sizeof(s));memset(&h,0,sizeof(h));fs_lock_init(&s.vfs.lock);
    s.ready=true;count=1;clock_ms=reads=0;scan_at=9000;mount_at=11000;
    no_scan=no_mount=read_error=false;h.ready=true;
    struct storage_volume *v=&s.volumes[4];
    v->drive=4;v->removable=true;v->fat.mount_ms=1200;
    v->io=(struct blkdev){.read=read_blocks,.capacity=8388608,.sector_size=512};
}
static void done(void) { fs_lock_destroy(&s.vfs.lock);assert(!fs_host_contig_live); }
int main(void) {
    struct hw_check_result r;
    reset();hardware_check_usb_storage(15000,&r,now);
    assert(r.status==HW_PASS && clock_ms==11320 && reads==16);
    assert(strstr(r.detail,"capacity=4294967296 bytes=1048576 mount_ms=1200 wait_ms=11000 read_ms=320 error=0"));
    done();puts("PASS USB page: first scan at 9s, publication at 11s, writable 4 GiB mount, 1 MiB read in 320 ms within 15s item");
    reset();h.ready=false;h.quarantined=true;h.reason="resource_conflict";
    hardware_check_usb_storage(15000,&r,now);
    assert(r.status==HW_FAIL && !clock_ms && !reads && strstr(r.detail,"reason=resource_conflict"));done();
    puts("PASS USB page: unavailable controller with no first scan reports resource_conflict immediately");
    reset();no_scan=no_mount=true;hardware_check_usb_storage(15000,&r,now);
    assert(r.status==HW_FAIL && clock_ms==13000 && strstr(r.detail,"reason=scan_pending"));done();
    reset();no_mount=true;hardware_check_usb_storage(15000,&r,now);
    assert(r.status==HW_SKIP && clock_ms==13000 && strstr(r.detail,"reason=no_volume"));done();
    reset();no_mount=true;h.device_count=1;h.devices[0].present=true;h.devices[0].class_code=8;
    h.devices[0].storage.binding=true;hardware_check_usb_storage(15000,&r,now);
    assert(r.status==HW_FAIL && clock_ms==13000 && strstr(r.detail,"binding=1"));done();
    puts("PASS USB page: pending first scan/bind fail at budget; completed scan with no storage skips");
    reset();scan_at=mount_at=0;h.scanned=true;s.volumes[4].present=true;s.volumes[4].fat.mounted=true;
    read_error=true;hardware_check_usb_storage(15000,&r,now);
    assert(r.status==HW_FAIL && reads==1 && strstr(r.detail,"error=-5"));done();
    reset();count=0;hardware_check_usb_storage(15000,&r,now);
    assert(r.status==HW_SKIP && !clock_ms && !reads);done();
    puts("PASS USB page: read failure stops without retry, absent host skips, temporary read buffer released");
    reset();no_scan=no_mount=true;clock_ms=100;
    hardware_check_usb_storage(2095,&r,now);
    assert(r.status==HW_FAIL && clock_ms==100 && !reads && strstr(r.detail,"reason=scan_pending"));done();
    reset();no_scan=no_mount=true;
    hardware_check_usb_storage(2095,&r,now);
    assert(r.status==HW_FAIL && clock_ms==95 && !reads && strstr(r.detail,"reason=scan_pending"));done();
    reset();no_mount=true;h.scanned=true;clock_ms=100;
    hardware_check_usb_storage(2095,&r,now);
    assert(r.status==HW_SKIP && clock_ms==100 && !reads);done();
    puts("PASS merged USB storage: first-scan deadline and publication wait preserve absolute reserves without oversleep");
    return 0;
}
