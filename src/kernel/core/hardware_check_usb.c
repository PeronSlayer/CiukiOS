/* USB diagnostic shares the asynchronous worker's lifecycle; no polling or
 * controller takeover on the page task. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/hardware_check.h>
#include <ciuki/storage.h>
#include <ciuki/usb.h>
bool hardware_check_usb_wait_first_scan(uint64_t until,struct hw_check_result *out,uint64_t (*now)(void *))
{
    uint64_t start=now(0);
    for (;;) {
        unsigned hosts=usb_count(), viable=0, scanned=0;
        const char *reason="unavailable";
        if (!hosts) {
            out->status=HW_SKIP; ksnprintf(out->detail,sizeof(out->detail),"USB host absent"); return false;
        }
        for (unsigned i=0;i<hosts;i++) {
            const struct usb_host *h=usb_get(i);
            if (!h) continue;
            if (h->ready && !h->quarantined) { viable++; scanned+=h->scanned; }
            else if (h->reason) reason=h->reason;
        }
        if (viable && scanned==viable) return true;
        uint64_t tick=now(0);
        if (!viable || tick>=until) {
            out->status=HW_FAIL;
            ksnprintf(out->detail,sizeof(out->detail),"hosts=%u scanned=%u reason=%s ms=%llu",
                viable,scanned,viable ? "scan_pending" : reason,tick-start);
            return false;
        }
        uint64_t left=until-tick;
        task_sleep_ms(left<10 ? (uint32_t)left : 10);
    }
}
void hardware_check_usb_storage(uint64_t until,struct hw_check_result *out, uint64_t (*now)(void *))
{
    uint64_t start=now(0), wait=until>2000 ? until-2000 : 0;
    if (!hardware_check_usb_wait_first_scan(wait,out,now)) return;
    struct storage *s=storage_get(); struct storage_volume *vol=0;
    unsigned present=0, quarantined=0, binding=0, viable=0, scanned=0;
    const char *reason="none";
    do {
        present=quarantined=binding=viable=scanned=0;
        for (unsigned i=0;i<usb_count();i++) {
            const struct usb_host *h=usb_get(i);
            if (!h) continue;
            if (h->ready && !h->quarantined) { viable++;scanned+=h->scanned; }
            else if (h->reason) reason=h->reason;
            for (unsigned j=0;j<h->device_count;j++) {
                const struct usb_device *d=&h->devices[j];
                if (!d->present || (!d->storage.bound && d->class_code!=8)) continue;
                present++; quarantined+=d->quarantined||d->storage.block.quarantined;
                binding+=d->storage.binding;
            }
        }
        if (s && s->ready) {
            fs_lock_take(&s->vfs.lock);
            for (unsigned i=4;i<26;i++) if (s->volumes[i].present && s->volumes[i].removable &&
                s->volumes[i].fat.mounted && !s->volumes[i].io.quarantined) { vol=&s->volumes[i]; break; }
            fs_lock_drop(&s->vfs.lock);
        }
        if (vol || !viable || (present && quarantined==present && !binding)) break;
        uint64_t tick=now(0);
        if (tick>=wait) break;
        uint64_t left=wait-tick;
        task_sleep_ms(left<10 ? (uint32_t)left : 10);
    } while (true);
    if (!vol) {
        out->status=present || !viable || scanned<viable ? HW_FAIL : HW_SKIP;
        ksnprintf(out->detail,sizeof(out->detail),"enumerated=%u mounted=0 binding=%u quarantine=%u hosts=%u scanned=%u reason=%s ms=%llu",
            present,binding,quarantined,viable,scanned,!viable ? reason : scanned<viable ? "scan_pending" : "no_volume",now(0)-start);
        return;
    }
    uint8_t *buf=fs_contig_alloc(BLKDEV_READ_BYTES);
    uint32_t bytes=0; int e=buf ? 0 : -ENOMEM;
    uint64_t mounted=now(0);
    while (!e && bytes<1024u*1024u) {
        if (until<1000 || now(0)>=until-1000) { e=-USB_ETIMEDOUT; break; }
        e=vol->io.read(&vol->io,bytes/512,BLKDEV_READ_SECTORS,buf);
        if (!e) bytes+=BLKDEV_READ_BYTES;
    }
    fs_contig_free(buf,BLKDEV_READ_BYTES);
    out->status=e ? HW_FAIL : HW_PASS;
    ksnprintf(out->detail,sizeof(out->detail),"drive=%c mounted=1 mode=%s capacity=%llu bytes=%u mount_ms=%u wait_ms=%llu read_ms=%llu error=%d",
        'A'+vol->drive,vol->fat.readonly ? "ro" : "rw",(unsigned long long)vol->io.capacity*512,
        bytes,vol->fat.mount_ms,mounted-start,now(0)-mounted,e);
}
