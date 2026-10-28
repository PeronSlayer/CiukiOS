/* SPDX-License-Identifier: MIT -- private action shared with first boot.
 * DM8 (lead decision 2026-10-23): a retry is a request keyed to the bound
 * driver and PCI function from the registry's engine record. On the next
 * boot the kernel registry truncates that driver's keyed marker and every
 * attempt file the driver declares, then consumes the request. Activation
 * remains a boot-time kernel operation; success requires a restart. No
 * driver-specific path is known here. */
#ifndef CIUKI_ACCEL_RETRY_H
#define CIUKI_ACCEL_RETRY_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ciuki/abi.h>
#define CP_ACCEL_RETRY_PATH "/system/settings/accel-retry.cfg"
/* The request text for this engine record: its length, or -errno. sec-sysvc2:
 * Control Panel hands it to the SYSTEM setting service (the file is sealed). */
static inline int cp_accel_retry_text(const struct ciuki_display_engine_info *e,char *text,size_t cap)
{
    if(!e||e->version!=CIUKI_DISPLAY_ENGINE_VERSION||e->size!=sizeof(*e))return -EINVAL;
    size_t n=strnlen(e->driver_id,sizeof(e->driver_id));
    if(!n||n>=sizeof(e->driver_id))return -EINVAL;
    for(size_t i=0;i<n;i++){char c=e->driver_id[i];if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return -EINVAL;}
    /* No PCI function is bound (software renderer): nothing to re-arm. */
    if(!e->pci_vendor)return -ENODEV;
    int length=snprintf(text,cap,"driver=%s\npci=%04x:%04x\n",e->driver_id,e->pci_vendor,e->pci_device);
    if(length<0||(size_t)length>=cap)return -EINVAL;
    return length;
}
static inline int cp_accel_retry_request(const char *path,const struct ciuki_display_engine_info *e)
{
    if(!path)return -EINVAL;
    char text[48];
    int length=cp_accel_retry_text(e,text,sizeof(text));
    if(length<0)return length;
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);if(fd<0)return -errno;
    int r=0;
    for(const char *at=text;!r&&length;){
        ssize_t k=write(fd,at,(size_t)length);
        if(k<0&&errno==EINTR)continue;
        if(k<=0){r=k<0?-errno:-EIO;break;}
        at+=k;length-=(int)k;
    }
    if(!r&&fsync(fd))r=-errno;
    if(close(fd)&&!r)r=-errno;
    return r;
}
#endif
