/* Setup's checked UTC clock/RTC update and boot convention reader.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/clock.h>
#include <ciuki/setup_mode.h>
#include <ciuki/storage.h>
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

/* Clock starts after storage_init but before native process bootstrap. Read
 * only; malformed/missing settings retain the historical UTC convention. */
int file_clock_rtc_offset(void)
{
    struct storage *s=storage_get();
    if (!s || !s->vfs.volumes[2]) return 0;
    struct vfs_table *t=kzalloc(sizeof(*t));
    if (!t) return 0;
    int offset=0;
    if (!vfs_table_init(&s->vfs,t,2)) {
        int h=vfs_open(t,"C:\\system\\settings\\clock.cfg",VFS_READ,VFS_DENY_NONE,0);
        char text[96]={0}; size_t n=0;
        if (h>=0) {
            int e=vfs_read(t,h,text,sizeof(text)-1,&n); vfs_close(t,h);
            const char prefix[]="version=1\nrtc_local=1\noffset_minutes=";
            if (!e && n>sizeof(prefix)-1 && !memcmp(text,prefix,sizeof(prefix)-1)) {
                size_t i=sizeof(prefix)-1; int sign=1,value=0;
                if (text[i]=='-') { sign=-1; i++; }
                size_t start=i;
                while (i<n && text[i]>='0' && text[i]<='9' && i-start<4) value=value*10+text[i++]-'0';
                value*=sign;
                if (i>start && i+1==n && text[i]=='\n' && value>=-720 && value<=840) offset=value*60;
            }
        }
        vfs_table_destroy(t);
    }
    kfree(t); return offset;
}
