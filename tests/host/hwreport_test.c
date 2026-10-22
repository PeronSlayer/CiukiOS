/* SPDX-License-Identifier: MIT */
#include "../../apps/hwreport/report.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fw_error,fw_bad,fw_calls;
static int firmware(void *ctx,struct ciuki_firmware_page *p)
{
    (void)ctx; fw_calls++;
    if (fw_error) return -EAGAIN;
    assert(p->version==2 && p->size==sizeof(*p) && (!p->cursor || p->generation==7));
    p->generation=7; p->total=2; p->count=1;
    p->records[0]=(struct ciuki_firmware_record){.id=p->cursor+1,.parent=1,.origin=1,.state=1,.capabilities=CIUKI_FW_CAP_BATTERY,.resource_count=1,.cid_count=1};
    strcpy(p->records[0].hid,"PNP0C0A"); strcpy(p->records[0].path,"\\_SB_.BAT0");
    strcpy(p->records[0].cid[0],"PNP0303"); strcpy(p->records[0].reason,"Firmware description only");
    p->records[0].resources[0]=(struct ciuki_firmware_resource){.type=1,.start=9,.end=10};
    p->next=p->cursor ? 0 : 1;
    if (fw_bad) p->count=5;
    return 0;
}
static int names(void *ctx,unsigned bus,unsigned vendor,unsigned device,char *out,size_t n)
{ (void)ctx; (void)vendor; (void)device; snprintf(out,n,"Fixture %s product",bus==1 ? "PCI" : "USB"); return 0; }
struct disk { char temp[HR_TEXT_MAX],final[HR_TEXT_MAX]; size_t used,n; int fail,closed,removed,published,presents; };
static int op_open(void *c,const char *p) { struct disk *d=c; assert(strstr(p,"CIUKIREP.TMP")); return d->fail==1 ? -ENOSPC : 7; }
static ssize_t op_write(void *c,int fd,const void *p,size_t n)
{
    struct disk *d=c; assert(fd==7);
    if (d->fail==2) return -ENOSPC;
    if (d->fail==3 && d->used) return -ENODEV;
    if (d->fail==4) return 0;
    if (d->fail==10 && !d->removed++) return -EINTR;
    if (n>7) n=7;
    memcpy(d->temp+d->used,p,n); d->used+=n; return (ssize_t)n;
}
static int op_sync(void *c,int fd) { assert(fd==7); return ((struct disk *)c)->fail==5 ? -EIO : 0; }
static int op_close(void *c,int fd) { struct disk *d=c; assert(fd==7); d->closed++; return d->fail==6 ? -EIO : 0; }
static int op_rename(void *c,const char *a,const char *b)
{ struct disk *d=c; assert(strstr(a,"CIUKIREP.TMP") && strstr(b,"CIUKIREP.TXT")); if(d->fail==7) return -EIO; memcpy(d->final,d->temp,d->used); d->n=d->used; d->published++; return 0; }
static int op_remove(void *c,const char *p) { assert(strstr(p,"CIUKIREP.TMP")); ((struct disk *)c)->removed++; return 0; }
static int op_present(void *c) { struct disk *d=c; d->presents++; return d->fail==8 || (d->fail==9 && d->presents>1) ? -ENODEV : 0; }
int main(int argc,char **argv)
{
    assert(argc==2);
    char *text=malloc(HR_TEXT_MAX); struct disk *disk=calloc(1,sizeof(*disk)); assert(text && disk);
    struct ciuki_devices_info d={.version=1,.size=sizeof(d),.count=4,.generation=7,.cpu_family=6,.cpu_model=11,.cpu_stepping=1};
    strcpy(d.cpu_vendor,"GenuineIntel"); strcpy(d.cpu_brand,"Pentium III"); strcpy(d.boot_options,"private-host=DO-NOT-EXPORT");
    for (unsigned i=0;i<4;i++) {
        d.devices[i]=(struct ciuki_device_record){.id=i,.bus_type=i==1 ? 3 : 1,.vendor=0x8086,.device=0x7010,.pci_class=1,.subclass=1,.prog_if=0x85,.class_id=i+4,.resource_count=1};
        strcpy(d.devices[i].name,"Synthetic adapter"); strcpy(d.devices[i].driver,"ata-pio"); strcpy(d.devices[i].reason,"ready");
        d.devices[i].resources[0]=(struct ciuki_device_resource){.type=0,.start=0x1f0,.end=0x1f8};
    }
    d.devices[2].class_id=CIUKI_DEVICE_DISK; strcpy(d.devices[2].driver,"fat"); strcpy(d.devices[2].reason,"file=secret-user.txt");
    struct ciuki_hardware_info h={.version=3,.size=sizeof(h),.smbios_valid=1,.cpuid_edx=0x8001,.memmap_count=3,.memory_bytes={0,67108864,4096,8192,16384,0}};
    strcpy(h.vendor,"IBM"); strcpy(h.product,"2647"); strcpy(h.bios_vendor,"IBM"); strcpy(h.bios_version,"1.20"); strcpy(h.bios_date,"10/22/1999"); strcpy(h.build_id,"fixture-build");
    strcpy(h.log_pending,"[ata] count=9 user=secret-user ip=203.0.113.78\n"); h.log_pending_bytes=(uint32_t)strlen(h.log_pending);
    struct ciuki_display_info v={.mode_count=1,.width=640,.height=480,.bpp=16};
    strcpy(v.product,"Synthetic VGA"); v.modes[0]=(struct ciuki_display_mode){.id=0x111,.width=640,.height=480,.bpp=16,.current=1};
    struct ciuki_accel_info e={.driver=CIUKI_ACCEL_DRIVER_SAVAGE,.capabilities=3};
    struct ciuki_net_state net={.present=1,.link=2,.state=1,.mac={0xde,0xad,0xbe,0xef,0x12,0x34},.ipv4={203,0,113,78}};
    strcpy(net.driver,"e100"); strcpy(net.reason,"DO-NOT-EXPORT");
    const char log[]="[ata] count=3 error=-5 path=/private/secret.txt\n[net] hostname=secret-host mac=de:ad:be:ef:12:34 ipv4=203.0.113.78\n"
        "[cpu] bytes=17 status=123.456 host=secret-host status=-123 count=3:4 free=9999999999999999999\n"
        "[kernel] pages=42 user=secret-user file=secret.txt\n";
    struct hr_source s={.devices=&d,.hardware=&h,.display=&v,.engine=&e,.network=&net,.firmware=firmware,.device_name=names,
        .boot_log=log,.latest_log=log,.boot_length=sizeof(log)-1,.latest_length=sizeof(log)-1};
    size_t n=0; assert(!hr_format(&s,text,HR_TEXT_MAX,&n) && n==strlen(text) && fw_calls==2);
    const char *required[]={"[CPU]","GenuineIntel","Family 6 model 11 stepping 1","EDX=0x00008001","[Memory map]","ACPI NVS bytes=16384",
        "[SMBIOS]","BIOS date: 10/22/1999","[PCI devices]","Fixture PCI product","[USB devices]","Fixture USB product",
        "end-exclusive=0x1f8","primary=native secondary=native bus-master-capable=1","[ACPI/PnP inventory]","PNP0C0A","CID: PNP0303",
        "Device 2 parent=1","[Display]","Mode 0111 640x480x16","2D engine driver=1","[Audio, network and storage bindings]",
        "Network present=1 link=2 state=1","fixture-build","Image SHA-256: unavailable","Build manifest: unavailable","[Boot log","[Latest kernel records",
        "error=-5","count=3","count=9","[Pending kernel records","pages=42","other log records redacted"};
    for (unsigned i=0;i<sizeof(required)/sizeof(*required);i++) { if (!strstr(text,required[i])) fprintf(stderr,"missing: %s\n",required[i]); assert(strstr(text,required[i])); }
    const char *excluded[]={"DO-NOT-EXPORT","secret-host","secret-user","secret.txt","de:ad:be:ef:12:34","203.0.113.78","123.456","3:4","9999999999999999999","/private"};
    for (unsigned i=0;i<sizeof(excluded)/sizeof(*excluded);i++) assert(!strstr(text,excluded[i]));
    assert(hr_format(&s,text,32,&n)==-ENOSPC && !n && !text[0]);
    fw_bad=1; assert(!hr_format(&s,text,HR_TEXT_MAX,&n) && strstr(text,"Invalid/stale firmware page")); fw_bad=0;
    fw_error=1; assert(!hr_format(&s,text,HR_TEXT_MAX,&n) && strstr(text,"error -11")); fw_error=0;
    d.count=65; assert(hr_format(&s,text,HR_TEXT_MAX,&n)==-EINVAL); d.count=4;
    h.version=2; assert(hr_format(&s,text,HR_TEXT_MAX,&n)==-EINVAL); h.version=3;
    memset(d.devices[0].name,'X',sizeof(d.devices[0].name)); s.device_name=NULL;
    assert(!hr_format(&s,text,HR_TEXT_MAX,&n));
    struct hr_source empty={.boot_error=-ENOENT,.latest_error=-EIO};
    assert(!hr_format(&empty,text,HR_TEXT_MAX,&n) && strstr(text,"Unavailable (error"));
    assert(!hr_format(&s,text,HR_TEXT_MAX,&n));
    struct hr_io io={disk,op_open,op_write,op_sync,op_close,op_rename,op_remove,op_present};
    for (int fail=0;fail<=10;fail++) {
        memset(disk,0,sizeof(*disk)); disk->fail=fail; strcpy(disk->final,"previous report");
        int result=hr_save(&io,"/mnt/d",text,n);
        if (!fail || fail==10) assert(!result && disk->n==n && !memcmp(disk->final,text,n) && disk->closed==1 && disk->published==1);
        else { assert(result<0 && !strcmp(disk->final,"previous report") && !disk->published); if (fail!=1 && fail!=8) assert(disk->closed==1 && disk->removed); }
    }
    assert(hr_default_drive(0x1d,0x18,0)==3);
    assert(hr_default_drive(0x5,0,0)==0);
    assert(hr_default_drive(0x4,0,0)==2);
    assert(hr_default_drive(0x5,0,1)==26);
    assert(hr_default_drive(0x1c,0x18,1)==3);
    /* Arbitrary bytes and unterminated records stay bounded under sanitizers. */
    char noise[1024]; for (unsigned i=0;i<sizeof(noise);i++) noise[i]=(char)(i*73);
    s.boot_log=noise; s.boot_length=sizeof(noise); assert(!hr_format(&s,text,HR_TEXT_MAX,&n));
    size_t filtered=0; assert(!hr_filter_log(log,sizeof(log)-1,text,HR_TEXT_MAX,&filtered));
    unsigned char *gz=NULL; size_t gzlen=0;
    assert(!hr_gzip(text,filtered,&gz,&gzlen) && gzlen>18 && gz[0]==0x1f && gz[1]==0x8b);
    FILE *f=fopen(argv[1],"wb"); assert(f && fwrite(gz,1,gzlen,f)==gzlen && !fclose(f)); free(gz);
    char expected[4096]; snprintf(expected,sizeof(expected),"%s.txt",argv[1]);
    f=fopen(expected,"wb"); assert(f && fwrite(text,1,filtered,f)==filtered && !fclose(f));
    free(text); free(disk);
    puts("PASS hardware report: every section, firmware paging, exclusions, bounds, short/EINTR writes, full/removed media, fsync/close/rename failures, drive policy");
}
