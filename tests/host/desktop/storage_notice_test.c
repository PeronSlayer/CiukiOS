/* Production desktop poll against retained inventory, no display or disk. */
#include <ciuki/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"notice:%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#define __i386__
#define DU_DLG_ICON_ERROR 2
#define SH_NOTIFY_DETAILS 0
struct desktop { struct { int gate; } sh; };
static struct ciuki_devices_info inventory;
static unsigned queries, notices;
static int query_error;
static char last[320];
static int ciuki_devices(struct ciuki_devices_info *out)
{
    CHECK(out->version==CIUKI_DEVICES_VERSION && out->size==sizeof(*out));queries++;
    if (query_error) return -1;
    *out=inventory;return 0;
}
static void desk_shell_notify(struct desktop *d,const char *title,const char *text,int icon,int action)
{
    CHECK(!d->sh.gate && !strcmp(title,"Storage is read-only") && icon==2 && action==0);
    snprintf(last,sizeof(last),"%s",text);notices++;
}
#include "latency_storage_notice.inc"
int main(void)
{
    struct desktop d={0};
    d.sh.gate=1;storage_poll(&d,0);CHECK(!queries);d.sh.gate=0;
    query_error=1;storage_poll(&d,0);CHECK(queries==1 && !notices);
    storage_poll(&d,1999999999);CHECK(queries==1);
    query_error=0;inventory.count=2;
    inventory.devices[0]=(struct ciuki_device_record){.id=0x41002};
    strcpy(inventory.devices[0].driver,"fat");strcpy(inventory.devices[0].name,"C: read-only volume");
    strcpy(inventory.devices[0].reason,"chain_bounds; cluster=8192; file=BOOT.LOG");
    inventory.devices[1]=inventory.devices[0];inventory.devices[1].id=0x41004;
    strcpy(inventory.devices[1].name,"E: read-only volume");
    storage_poll(&d,2000000000);CHECK(queries==2 && notices==1);
    CHECK(strstr(last,"C:") && strstr(last,"chain_bounds; cluster=8192; file=BOOT.LOG"));
    storage_poll(&d,4000000000);CHECK(notices==2 && strstr(last,"E:"));
    storage_poll(&d,6000000000);CHECK(notices==2);
    strcpy(inventory.devices[0].reason,"device_quarantined; cluster=8192; file=BOOT.LOG");
    storage_poll(&d,8000000000);CHECK(notices==3 && strstr(last,"device_quarantined"));
    inventory.count=0;storage_poll(&d,10000000000);CHECK(notices==3);
    inventory.count=1;storage_poll(&d,12000000000);CHECK(notices==4);
    puts("desktop storage: live reason/file/cluster notification, gate silence, two-second polling, query failure recovery, deduplication and a new cause/session PASS");
    return 0;
}
