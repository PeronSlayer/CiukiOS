/* f1-97: the desktop's one-line 2D acceleration notice (production
 * accel_notice in apps/desktop/desktop.c), no display or disk.
 * SPDX-License-Identifier: MIT */
#include <ciuki/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"accel-notice:%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#define __i386__
#define DU_DLG_ICON_INFO 0
#define SH_NOTIFY_DETAILS 0
struct desktop { uint64_t now_ns; struct { struct { uint64_t expires; } notification; } sh; };
static struct ciuki_devices_info inventory;
static unsigned queries, notices;
static int query_error;
static char last_title[64], last_text[320];
static int ciuki_devices(struct ciuki_devices_info *out)
{
    CHECK(out->version==CIUKI_DEVICES_VERSION && out->size==sizeof(*out)); queries++;
    if (query_error) return -1;
    *out=inventory; return 0;
}
static void desk_shell_notify(struct desktop *d,const char *title,const char *text,int icon,int action)
{
    (void)d; CHECK(icon==DU_DLG_ICON_INFO && action==SH_NOTIFY_DETAILS);   /* a card, never an error dialog */
    snprintf(last_title,sizeof(last_title),"%s",title); snprintf(last_text,sizeof(last_text),"%s",text); notices++;
}
#include "latency_accel_notice.inc"
static void record(unsigned state,const char *reason)
{
    memset(&inventory,0,sizeof(inventory));
    struct ciuki_device_record *v=&inventory.devices[0];
    snprintf(v->driver,sizeof(v->driver),"fat"); snprintf(v->reason,sizeof(v->reason),"%s","unrelated");
    v=&inventory.devices[1]; v->state=state;
    snprintf(v->driver,sizeof(v->driver),"savage2d"); snprintf(v->reason,sizeof(v->reason),"%s",reason);
    inventory.count=2;
}
int main(void)
{
    struct desktop d={.now_ns=5000000000ull};
    char text[160];
    accel_notice_text(text,sizeof(text),1,"none");
    CHECK(!strcmp(text,"2D acceleration: YES \xE2\x80\x94 Savage"));
    accel_notice_text(text,sizeof(text),0,"bci_test_readback_failed");
    CHECK(!strcmp(text,"2D acceleration: NO \xE2\x80\x94 software rendering (bci_test_readback_failed)"));
    accel_notice_text(text,sizeof(text),0,"");
    CHECK(strstr(text,"(unknown)"));
    record(CIUKI_DEVICE_READY,"accelerated");
    CHECK(accel_notice(&d)==1 && notices==1);
    CHECK(!strcmp(last_text,"2D acceleration: YES \xE2\x80\x94 Savage") && d.sh.notification.expires==15000000000ull);
    record(CIUKI_DEVICE_QUARANTINED,"unsupported_device");
    CHECK(accel_notice(&d)==1 && notices==2);
    CHECK(!strcmp(last_text,"2D acceleration: NO \xE2\x80\x94 software rendering (unsupported_device)"));
    inventory.count=1;                              /* no record: nothing to say */
    CHECK(accel_notice(&d)==1 && notices==2);
    query_error=1;                                  /* retried twice, then given up */
    unsigned before=queries;
    CHECK(accel_notice(&d)==0 && accel_notice(&d)==0 && accel_notice(&d)==1 && notices==2 && queries==before+3);
    printf("desktop accel notice: YES/NO text, 10 s expiry, device record PASS\n");
    return 0;
}
