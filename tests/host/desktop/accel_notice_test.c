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
static int notice_at(struct desktop *d,uint64_t ns) { d->now_ns=ns; return accel_notice(d); }
int main(void)
{
    struct desktop d={.now_ns=5000000000ull};
    char text[160];
    /* f2-92: YES only once the engine has drawn; OK (drawing: software) before. */
    accel_notice_text(text,sizeof(text),1,1,"accelerated");
    CHECK(!strcmp(text,"2D acceleration: YES \xE2\x80\x94 Savage"));
    accel_notice_text(text,sizeof(text),1,0,"engine_ok_drawing_software");
    CHECK(!strcmp(text,"2D engine: OK (drawing: software)"));
    accel_notice_text(text,sizeof(text),0,0,"bci_test_readback_failed");
    CHECK(!strcmp(text,"2D engine: NO (bci_test_readback_failed)"));
    accel_notice_text(text,sizeof(text),0,0,"");
    CHECK(strstr(text,"(unknown)"));
    record(CIUKI_DEVICE_READY,"accelerated");
    CHECK(notice_at(&d,5000000000ull)==1 && notices==1);
    CHECK(!strcmp(last_text,"2D acceleration: YES \xE2\x80\x94 Savage") && d.sh.notification.expires==15000000000ull);
    /* Ready but unused: 3 s for the first engine draw, polled twice a second. */
    memset(&accel_notice_state,0,sizeof(accel_notice_state));
    record(CIUKI_DEVICE_READY,"engine_ok_drawing_software");
    unsigned before=queries;
    CHECK(notice_at(&d,20000000000ull)==0 && notice_at(&d,20100000000ull)==0 && queries==before+1);
    CHECK(notice_at(&d,20600000000ull)==0 && notices==1);
    record(CIUKI_DEVICE_READY,"accelerated");             /* drew meanwhile */
    CHECK(notice_at(&d,21200000000ull)==1 && notices==2 && strstr(last_text,"YES"));
    memset(&accel_notice_state,0,sizeof(accel_notice_state));
    record(CIUKI_DEVICE_READY,"engine_ok_drawing_software");
    CHECK(notice_at(&d,30000000000ull)==0 && notice_at(&d,31000000000ull)==0 && notice_at(&d,33100000000ull)==1);
    CHECK(notices==3 && !strcmp(last_text,"2D engine: OK (drawing: software)"));
    memset(&accel_notice_state,0,sizeof(accel_notice_state));
    record(CIUKI_DEVICE_QUARANTINED,"unsupported_device");
    CHECK(notice_at(&d,40000000000ull)==1 && notices==4);
    CHECK(!strcmp(last_text,"2D engine: NO (unsupported_device)"));
    /* No record yet: the activation decides later; give up after 60 s. */
    memset(&accel_notice_state,0,sizeof(accel_notice_state));
    inventory.count=1;
    CHECK(notice_at(&d,50000000000ull)==0 && notice_at(&d,80000000000ull)==0 && notice_at(&d,110000000000ull)==1 && notices==4);
    memset(&accel_notice_state,0,sizeof(accel_notice_state));
    query_error=1;                                  /* retried twice, then given up */
    before=queries;
    CHECK(notice_at(&d,120000000000ull)==0 && notice_at(&d,121000000000ull)==0 && notice_at(&d,122000000000ull)==1 &&
          notices==4 && queries==before+3);
    printf("desktop accel notice: YES only after an engine draw, OK (drawing: software), NO (<reason>), late decision, 10 s expiry PASS\n");
    return 0;
}
