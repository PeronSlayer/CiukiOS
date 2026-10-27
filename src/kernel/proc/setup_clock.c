/* Setup's checked UTC clock/RTC update and boot convention reader.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/desktop.h>
#include <ciuki/clock.h>
#include <ciuki/setup_mode.h>
#include <ciuki/storage.h>
#include <ciuki/systime.h>
extern bool fs_rtc_store(uint16_t,uint16_t,uint8_t) __attribute__((weak));
static bool leap_year(unsigned y) { return !(y%4) && (y%100 || !(y%400)); }
int file_setup_clock(struct process *p,const struct ciuki_setup_clock *q)
{
    if (!p || !p->setup_capability || !setup_mode()) return -EPERM;
    if (!q || q->version!=CIUKI_SETUP_FAT_VERSION || q->size!=sizeof(*q) || q->rtc_local>1 ||
        q->offset_minutes < -720 || q->offset_minutes>840 || q->seconds<0 || q->seconds>3471292799LL) return -EINVAL;
    int64_t rtc=q->seconds+(q->rtc_local ? q->offset_minutes*60 : 0);
    if (rtc<315532800LL || rtc>3471292799LL) return -EINVAL; /* shared reader's 1980..2079 pivot */
    uint64_t days=(uint64_t)rtc/86400,rem=(uint64_t)rtc%86400;
    unsigned year=1970,month=1;
    while (days>=365u+leap_year(year)) { days-=365u+leap_year(year); year++; }
    static const uint8_t months[]={31,28,31,30,31,30,31,31,30,31,30,31};
    while (days>=(unsigned)months[month-1]+(month==2 && leap_year(year))) {
        days-=(unsigned)months[month-1]+(month==2 && leap_year(year)); month++;
    }
    uint16_t date=(uint16_t)((year-1980)<<9|month<<5|(unsigned)(days+1));
    uint16_t time=(uint16_t)((rem/3600)<<11|((rem/60)%60)<<5|(rem%60)/2);
    if (!fs_rtc_store || !fs_rtc_store(date,time,(uint8_t)((rem%2)*100))) return -EIO;
    file_clock_set_utc(q->seconds);
    return 0;
}

/* G1: system/settings/clock.cfg store of the time-zone service
 * (proc/timezone.c, codec in lib/tzrule.c). Clock start runs after
 * storage_init but before native process bootstrap; a missing or malformed
 * file keeps the historical UTC convention. */
#define CLOCK_CFG "C:\\system\\settings\\clock.cfg"
static int clock_table(struct vfs_table **out)
{
    struct storage *s=storage_get();
    if (!s || !s->vfs.volumes[2]) return -ENODEV;
    struct vfs_table *t=desktop_buffer_alloc(sizeof(*t),MM_IO_REQUEST);
    if (!t) return -ENOMEM;
    if (vfs_table_init(&s->vfs,t,2)) { desktop_buffer_free(t,sizeof(*t),MM_IO_REQUEST); return -EIO; }
    *out=t; return 0;
}
int timezone_store_read(char *text,size_t capacity,size_t *length)
{
    struct vfs_table *t; int e=clock_table(&t); if (e) return e;
    int h=vfs_open(t,CLOCK_CFG,VFS_READ,VFS_DENY_NONE,0);
    if (h<0) e=-ENOENT;
    else {
        size_t n=0; e=vfs_read(t,h,text,capacity,&n); vfs_close(t,h);
        if (!e && n==capacity) e=-E2BIG;
        if (!e) *length=n; else e=-EIO;
    }
    vfs_table_destroy(t); desktop_buffer_free(t,sizeof(*t),MM_IO_REQUEST); return e;
}
int timezone_store_write(const char *text,size_t length)
{
    struct vfs_table *t; int e=clock_table(&t); if (e) return e;
    e=vfs_mkdir(t,"C:\\system\\settings",0);
    if (e && e!=-FS_EEXIST) e=-EIO;
    else {
        int h=vfs_open(t,CLOCK_CFG,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,0);
        if (h<0) e=-EIO;
        else {
            size_t n=0; e=vfs_write(t,h,text,length,&n);
            if (!e && n!=length) e=-FS_EIO;
            if (!e) e=vfs_commit(t,h);
            int closed=vfs_close(t,h);
            e=e || closed ? -EIO : 0;
        }
    }
    vfs_table_destroy(t); desktop_buffer_free(t,sizeof(*t),MM_IO_REQUEST); return e;
}
