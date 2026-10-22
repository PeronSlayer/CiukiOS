/* Production ACPICA/OSPM over modeled ports, EC and IRQ routing.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include <ciuki/registry.h>
static uint8_t rom[0x100000], io[65536];
static struct resource leases[16];
static unsigned lease_count, enables, disables, writes, maps, allocation_count, callbacks;
static size_t live;
static uint64_t now;
static const char *scenario;
static ACPI_OSD_HANDLER irq_fn;
static void *irq_context;
static bool finish_safe, finish_called, driving_off, off_driven;
static bool repeat_more = true;
static unsigned ec_step;
static uint8_t ec_address, ec_value = 1, ec_command, ec_reply;
static bool ec_obf, ec_query;
bool registry_valid(int h, gen_t gen)
{ return h >= 0 && (unsigned)h < lease_count && leases[h].generation == gen && leases[h].state == RS_ACTIVE; }
const struct resource *registry_get(unsigned h) { return h < lease_count ? &leases[h] : NULL; }
unsigned pci_count(void) { return 0; }
const struct pci_func *pci_get(unsigned n) { (void)n; return NULL; }
static void *allocate(size_t n)
{ size_t *p = calloc(1,n+16); if (!p) return NULL; *p=n; live+=n; allocation_count++; return p+2; }
static void release(void *p) { size_t *q=(size_t *)p-2; live-=*q; free(q); }
static size_t charge(size_t n) { return n+16; }
static const void *map(uint32_t a, uint32_t n)
{ if (a>sizeof(rom) || n>sizeof(rom)-a) return NULL; maps++; return rom+a; }
static void unmap(const void *p, uint32_t n) { (void)p; (void)n; assert(maps); maps--; }
static uint64_t clock100ns(void) { return now; }
static void pause_ms(uint32_t ms)
{
    now += ms ? (uint64_t)ms*10000 : 1;
    if (driving_off && !off_driven) { off_driven=true; fw_power_poll(); }
}
static int stall(uint32_t us) { now += (uint64_t)us*10; return 0; }
static uint64_t thread(void) { return 1; }
static uint32_t lock(void) { return 0x200; }
static void unlock(uint32_t flags) { assert(flags==0x200); }
static ACPI_STATUS access(unsigned space, uint64_t a, unsigned width, UINT64 *value, const ACPI_PCI_ID *pci)
{
    if (!fw_region_check(space,a,width,pci)) return AE_ACCESS;
    assert(!space); *value=0;
    if (a==0x66) *value = (!strcmp(scenario,"ec-timeout") ? 2 : 0) | ec_obf | (ec_query ? 0x20 : 0);
    else if (a==0x62) { assert(ec_obf); *value=ec_reply; ec_obf=false; }
    else for (unsigned i=0;i<width/8;i++) *value |= (UINT64)io[a+i] << (8*i);
    return AE_OK;
}
static ACPI_STATUS write_access(unsigned space, uint64_t a, unsigned width, UINT64 value, const ACPI_PCI_ID *pci)
{
    if (!fw_region_check(space,a,width,pci)) return AE_ACCESS;
    writes++;
    if (a==0xb2) {
        if (value==0xf1) { enables++; if (strcmp(scenario,"enable-timeout")) io[0x604]|=1; }
        else { assert(value==0xf0); disables++; if (!strcmp(scenario,"rollback")) return AE_ERROR; io[0x604]&=~1u; }
    } else if (a==0x66) {
        ec_command=value; ec_step=0;
        if (value==0x84) { ec_reply=ec_query ? 0x42 : 0; ec_query=false; ec_obf=true; }
        else assert(value==0x80 || value==0x81);
    } else if (a==0x62) {
        if (!ec_step++) { ec_address=value; if (ec_command==0x80) { ec_reply=ec_value; ec_obf=true; } }
        else { assert(ec_command==0x81); ec_value=value; }
    } else {
        if (a==0x604 && (value&0x2000)) {
            uint32_t pts=0; assert(!fw_integer(ACPI_ROOT_OBJECT,"PTSV",&pts) && pts==5);
            assert(!io[0x622] && !io[0x623] && !(io[0x601]&0x80));
        }
        for (unsigned i=0;i<width/8;i++) {
            if ((a+i>=0x600 && a+i<0x602) || (a+i>=0x620 && a+i<0x622)) io[a+i]&=~(value>>(i*8));
            else io[a+i]=value>>(i*8);
        }
    }
    return AE_OK;
}
static ACPI_STATUS claim(uint32_t a, uint32_t n)
{
    if (!n || a>65535 || n>65536-a || lease_count==16) return AE_BAD_ADDRESS;
    for (unsigned i=0;i<lease_count;i++) if (a<leases[i].end && a+n>leases[i].start) return AE_ACCESS;
    unsigned h=lease_count++;
    leases[h]=(struct resource){RES_PORT,a,a+n,"acpi-power",h+1,RS_ACTIVE,false};
    return fw_region_grant(0,h,h+1,NULL) ? AE_ACCESS : AE_OK;
}
static ACPI_STATUS sci(UINT32 n, ACPI_OSD_HANDLER fn, void *ctx)
{
    assert(n==9);
    if (!strcmp(scenario,"route")) return AE_SUPPORT;
    if (fn && !strcmp(scenario,"rollback")) return AE_SUPPORT;
    if (fn) { irq_fn=fn; irq_context=ctx; } return AE_OK;
}
static ACPI_STATUS unsci(UINT32 n, ACPI_OSD_HANDLER fn)
{ (void)n; (void)fn; irq_fn=NULL; irq_context=NULL; return AE_OK; }
static void finish(bool safe)
{
    finish_called=true; finish_safe=safe;
    for (unsigned i=0;i<lease_count;i++) leases[i].state=safe ? RS_RELEASED : RS_QUARANTINED;
    fw_regions_reset();
}
static const struct fw_ops ops={allocate,release,charge,map,unmap,clock100ns,pause_ms,stall,thread,lock,unlock,access,
    write_access,claim,sci,unsci,finish};
static void fire(void)
{ assert(irq_fn); fw_interrupt=true; (void)irq_fn(irq_context); fw_interrupt=false; }
static void repeat(void *arg)
{ (void)arg; callbacks++; if (repeat_more) assert(!AcpiOsExecute(OSL_NOTIFY_HANDLER,repeat,NULL)); }
int main(int argc, char **argv)
{
    assert(argc==3); scenario=argv[2];
    char path[1024]; snprintf(path,sizeof(path),"%s/power-%s.bin",argv[1],scenario);
    FILE *f=fopen(path,"rb"); assert(f && fread(rom,1,sizeof(rom),f)==sizeof(rom)); fclose(f);
    io[0x602]=io[0x622]=0x80; /* rollback restores the incoming enable masks */
    if (!strcmp(scenario,"already-enabled")) io[0x604]=1;
    ACPI_STATUS s=fw_inventory_power_run(&ops,0xe0000);
    struct ciuki_power_status p;
    assert(!acpi_power_snapshot(&p));
    unsigned query_writes = writes, query_allocations = allocation_count;
    for (unsigned i=0;i<100;i++) assert(!acpi_power_snapshot(&p));
    assert(writes==query_writes && allocation_count==query_allocations);
    bool fails=!strcmp(scenario,"no-crt") || !strcmp(scenario,"passive") || !strcmp(scenario,"hot") ||
        !strcmp(scenario,"route") || !strcmp(scenario,"enable-timeout") || !strcmp(scenario,"rollback") ||
        !strcmp(scenario,"init-failure") || !strcmp(scenario,"global-lock") || !strcmp(scenario,"shared-power") || !strcmp(scenario,"battery-limit");
    if (fails) {
        assert(s && !fw_owned && !acpi_power_can_off());
        assert(finish_called && !live && !maps);
        if (!strcmp(scenario,"rollback")) assert(!finish_safe && p.ownership==CIUKI_POWER_QUARANTINED);
        else assert(finish_safe && p.ownership==CIUKI_POWER_FIRMWARE);
        if (enables && strcmp(scenario,"rollback")) assert(io[0x602]==0x80 && io[0x622]==0x80);
        if (!strcmp(scenario,"enable-timeout")) assert(enables==1 && disables==1 && now>=30000000);
        if (!strcmp(scenario,"no-crt") || !strcmp(scenario,"passive") || !strcmp(scenario,"hot") || !strcmp(scenario,"route")) assert(!enables);
    } else {
        assert(!s && fw_owned && irq_fn && acpi_power_can_off());
        assert(p.ownership==CIUKI_POWER_OWNED && p.ac_online==1 && p.lid_open==1 && p.battery_count==1 && p.zone_count==1);
        if (!strcmp(scenario,"unit")) assert(p.batteries[0].unit==CIUKI_POWER_UNKNOWN && p.batteries[0].remaining==CIUKI_POWER_UNKNOWN);
        else assert(p.batteries[0].unit==0 && p.batteries[0].full==5000 && p.batteries[0].remaining==(!strcmp(scenario,"unknown") ? CIUKI_POWER_UNKNOWN : 2500));
        uint32_t on=0; assert(!fw_integer(ACPI_ROOT_OBJECT,"\\_SB.FANR._STA",&on) && on==1);
        if (!strcmp(scenario,"ec-events") || !strcmp(scenario,"ec-timeout")) {
            uint8_t value=0; int e=acpi_ec_quirk_transfer(false,0,&value);
            if (!strcmp(scenario,"ec-timeout")) {
                assert(e==-110 && !fw_ec_ready()); fw_power_poll();
                assert(!fw_owned && finish_called && !finish_safe);
                (void)AcpiTerminate(); fw_os_cleanup(); assert(!live && !maps);
                puts("PASS power ec-timeout: bounded engine, retained uncertain ports, no second EC command"); return 0;
            }
            else {
                assert(!e && value==1 && !ec_address);
                value=7; assert(!acpi_ec_quirk_transfer(true,0,&value));
                uint32_t v=0; assert(!fw_integer(ACPI_ROOT_OBJECT,"\\_SB.EC00.READ",&v) && v==7);
                ec_query=true; fw_power_poll(); fw_power_poll();
                assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
            }
        }
        if (!strcmp(scenario,"storm")) {
            fw_budget_begin(100000,250);
            for (unsigned i=0;i<16;i++) assert(!AcpiOsExecute(OSL_NOTIFY_HANDLER,repeat,NULL));
            assert(AcpiOsExecute(OSL_NOTIFY_HANDLER,repeat,NULL)==AE_LIMIT);
            fw_queue_drain(16); assert(callbacks==16); fw_budget_end();
            /* Pop/drain the surviving requeued callbacks without replenishment. */
            repeat_more=false; fw_queue_drain(16); assert(callbacks==32);
            for (unsigned i=0;i<1000;i++) { io[0x601]|=1; fire(); }
            assert(fw_power.shutdown_requests==1 && !(io[0x601]&1));
            puts("PASS power storm: bounded 16 callbacks and explicit queue overflow");
        }
        if (!strcmp(scenario,"thermal-change")) {
            assert(!fw_eval(ACPI_ROOT_OBJECT,"\\_TZ.THRM.HEAT",NULL,NULL)); fw_power_poll();
            assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
        }
        if (!strcmp(scenario,"eisa-button") || !strcmp(scenario,"cid-button")) {
            io[0x620]|=1; fire(); fw_power_poll(); fw_power_poll();
            assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
        }
        io[0x601]|=1; fire(); fw_power_poll(); /* fixed power button, W1C acknowledgement */
        assert(!(io[0x601]&1)); assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
        io[0x620]|=1; fire(); fw_power_poll(); fw_power_poll(); /* AML _L00 -> Notify */
        if (!strcmp(scenario,"gpe-loop")) {
            assert(!fw_owned && finish_called && finish_safe && now<10000000);
            (void)AcpiTerminate(); fw_os_cleanup(); assert(!live && !maps);
            puts("PASS power GPE loop: finite AML budget, firmware mode/masks restored, no event worker starvation"); return 0;
        }
        assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
        if (strcmp(scenario,"ec-timeout")) {
            driving_off=true; int result=acpi_power_off_owned(); driving_off=false;
            assert(result==-5);
            if (!strcmp(scenario,"pts-failure")) assert(!(io[0x605]&0x20));
            else assert(io[0x605]&0x20);
        }
        fw_power_stop(); (void)AcpiTerminate(); fw_os_cleanup();
        if (!strcmp(scenario,"already-enabled")) assert(!finish_safe && !enables && !disables &&
            !io[0x622] && !io[0x623] && fw_power.ownership==CIUKI_POWER_QUARANTINED);
        assert(!live && !maps);
    }
    printf("PASS power %s: status=%u enables=%u disables=%u writes=%u allocations=%u\n",scenario,s,enables,disables,writes,allocation_count);
    return 0;
}
