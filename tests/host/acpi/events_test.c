/* Production ACPICA/OSPM over modeled ports, EC and IRQ routing.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include <ciuki/registry.h>
#include <ciuki/apm_ports.h>
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
/* fx-3: T23-shaped EC: hold the RD_EC reply (late OBF), count QR_EC. */
static bool ec_hold_reply, ec_late;
static unsigned ec_query_commands, ec_reads_of_data;
static bool nested_quirk; static int nested_result=1; static uint64_t nested_wait;
static unsigned ec_read_commands; /* fx-3 follow-up 2: RD_EC count */
/* ciuki/power_source.h seam: one ACPI/APM provider, reserved before writes. */
static unsigned reserves, activations, retires, publishes;
static bool retired_uncertain;
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
    else if (a==0x62) { assert(ec_obf); *value=ec_reply; ec_obf=false; ec_reads_of_data++; }
    else for (unsigned i=0;i<width/8;i++) *value |= (UINT64)io[a+i] << (8*i);
    return AE_OK;
}
static ACPI_STATUS write_access(unsigned space, uint64_t a, unsigned width, UINT64 value, const ACPI_PCI_ID *pci)
{
    if (!fw_region_check(space,a,width,pci)) return AE_ACCESS;
    if (a!=0x62 && a!=0x66) writes++; /* EC data transactions are not ownership writes (fx-3: EC-backed _TMP) */
    if (a==0xb2) {
        if (value==0xf1) { enables++; if (strcmp(scenario,"enable-timeout")) io[0x604]|=1; }
        else { assert(value==0xf0); disables++; if (!strcmp(scenario,"rollback")) return AE_ERROR; io[0x604]&=~1u; }
    } else if (a==0x66) {
        ec_command=value; ec_step=0;
        if (value==0x84) {
            ec_query_commands++; ec_reply=ec_query ? 0x42 : 0; ec_query=false; ec_obf=true;
            if (nested_quirk) {
                /* fx-3, ACPI-owned: the volume quirk arrives while the event
                 * worker's QR_EC holds the engine; it waits, bounded. */
                nested_quirk=false; uint8_t v=0; unsigned cost=9; uint64_t start=now;
                nested_result=acpi_ec_quirk_transfer_counted(false,0x30,&v,5,&cost);
                nested_wait=now-start; assert(!cost);
            }
        }
        else { assert(value==0x80 || value==0x81); if (value==0x80) ec_read_commands++; }
    } else if (a==0x62) {
        if (!ec_step++) {
            ec_address=value;
            if (ec_command==0x80 && ec_hold_reply) ec_late=true;
            else if (ec_command==0x80) { ec_reply=ec_value; ec_obf=true; }
        }
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
static bool leases_cover(uint32_t a)
{
    for (unsigned i=0;i<lease_count;i++) if (leases[i].state==RS_ACTIVE && leases[i].start<=a && leases[i].end>a) return true;
    return false;
}
static ACPI_STATUS claim(uint32_t a, uint32_t n)
{
    if (!n || a>65535 || n>65536-a || lease_count==16) return AE_BAD_ADDRESS;
    /* A released lease no longer conflicts (registry semantics); fx-3's
     * legacy EC binding reclaims 62h/66h after a refused takeover. */
    for (unsigned i=0;i<lease_count;i++) if (leases[i].state!=RS_RELEASED && a<leases[i].end && a+n>leases[i].start) return AE_ACCESS;
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
static int reserve(void)
{
    assert(!enables && !writes); reserves++; /* EC leases may precede it; no ownership write does */
    return !strcmp(scenario,"provider-busy") ? -16 : 0; /* APM already holds it */
}
static int activate(void) { assert(reserves && !retires && fw_owned && !activations); activations++; return 0; }
static void retire(bool uncertain) { assert(reserves); retires++; retired_uncertain=uncertain; }
static void publish(const struct ciuki_power_status *p)
{ assert(p->version==CIUKI_POWER_VERSION && p->size==sizeof(*p) && p->battery_count<=CIUKI_POWER_BATTERIES); publishes++; }
static unsigned sleep_requests;
static void sleep_notice(void) { sleep_requests++; }
static const struct fw_ops ops={allocate,release,charge,map,unmap,clock100ns,pause_ms,stall,thread,lock,unlock,access,
    write_access,claim,sci,unsci,finish,reserve,activate,retire,publish,NULL,sleep_notice};
static void fire(void)
{ assert(irq_fn); fw_interrupt=true; (void)irq_fn(irq_context); fw_interrupt=false; }
static void repeat(void *arg)
{ (void)arg; callbacks++; if (repeat_more) assert(!AcpiOsExecute(OSL_NOTIFY_HANDLER,repeat,NULL)); }
/* fx-3: the T23 keeps ACPI in firmware (SMM owns EC events). After the
 * inventory settles, the ThinkPad volume quirk binds the one EC engine to
 * 62h/66h (legacy): RD_EC/WR_EC of register 30h only, never QR_EC, and a
 * timed-out read recovers instead of latching the engine. */
static void legacy_quirk(void)
{
    uint8_t value=0; unsigned cost=0;
    if (!strcmp(scenario,"rollback")) {
        /* Uncertain hand-back (quarantined): no legacy EC access at all. */
        assert(acpi_ec_quirk_attach()==-19 && !acpi_ec_quirk_legacy());
        assert(acpi_ec_quirk_transfer(false,0x30,&value)<0);
        puts("PASS power rollback: quarantined ownership refuses the legacy EC quirk"); return;
    }
    unsigned leases=lease_count, queries=ec_query_commands;
    assert(acpi_ec_quirk_transfer(false,0x30,&value)==-19); /* not attached: no I/O */
    assert(acpi_ec_quirk_attach()==0 && acpi_ec_quirk_legacy() && lease_count==leases+2);
    assert(leases_cover(0x62) && leases_cover(0x66));
    assert(acpi_ec_quirk_attach()==0 && lease_count==leases+2); /* idempotent */
    ec_value=0x47; /* T23 register 30h: level 7, mute */
    assert(!acpi_ec_quirk_transfer_counted(false,0x30,&value,500,&cost) && value==0x47 && ec_address==0x30 && cost>=4);
    value=0x07; assert(!acpi_ec_quirk_transfer(true,0x30,&value) && ec_value==0x07);
    /* SMM owns SCI_EVT: neither the quirk nor the (unowned) poll queries. */
    ec_query=true; fw_power_poll();
    assert(!acpi_ec_quirk_transfer(false,0x30,&value) && value==0x07 && ec_query_commands==queries);
    ec_query=false;
    /* A late reply to a timed-out RD_EC is dropped by the next transaction. */
    ec_hold_reply=true; uint64_t start=now;
    assert(acpi_ec_quirk_transfer_counted(false,0x30,&value,20,&cost)==-110 && now-start<=21*10000);
    ec_hold_reply=false; assert(ec_late); ec_late=false; ec_reply=0x5a; ec_obf=true; /* stale byte */
    unsigned data_reads=ec_reads_of_data; ec_value=0x0c;
    assert(!acpi_ec_quirk_transfer(false,0x30,&value) && value==0x0c && ec_reads_of_data==data_reads+2);
    assert(acpi_ec_quirk_attach()==0 && ec_query_commands==queries);
    /* fx-18: an APM BIOS call holds the engine; a quirk transfer waits
     * within its own deadline and never interleaves; release reopens it. */
    assert(!acpi_ec_firmware_hold(100));
    start=now; assert(acpi_ec_quirk_transfer_counted(false,0x30,&value,5,&cost)==-16 && now-start>=5*10000);
    assert(acpi_ec_firmware_hold(3)==-11); /* one hold at a time */
    acpi_ec_firmware_release();
    assert(!acpi_ec_quirk_transfer(false,0x30,&value) && value==0x0c);
    /* fx-18: the FADT blocks the APM BIOS port window is built from. */
    struct apm_pm_blocks pm; assert(!acpi_pm_blocks(&pm));
    assert(pm.pm1a_evt==0x600 && pm.pm1_evt_len==4 && pm.pm1a_cnt==0x604 && pm.pm1_cnt_len==2 &&
           pm.gpe0==0x620 && pm.gpe0_len==4 && pm.smi_cmd==0xb2 && !pm.pm1b_cnt && !pm.gpe1);
    printf("PASS power %s: firmware-owned EC, legacy quirk binding on 62h/66h, RD/WR 30h, no QR_EC, late reply recovered, APM hold, FADT blocks\n",scenario);
}
int main(int argc, char **argv)
{
    assert(argc==3); scenario=argv[2];
    char path[1024]; snprintf(path,sizeof(path),"%s/power-%s.bin",argv[1],scenario);
    FILE *f=fopen(path,"rb"); assert(f && fread(rom,1,sizeof(rom),f)==sizeof(rom)); fclose(f);
    io[0x602]=io[0x622]=0x80; /* rollback restores the incoming enable masks */
    uint8_t early=0;
    assert(acpi_ec_quirk_attach()==-11 && acpi_ec_quirk_transfer(false,0x30,&early)==-19); /* fx-3: before the inventory settles */
    if (!strcmp(scenario,"already-enabled")) io[0x604]=1;
    if (!strcmp(scenario,"tzp-ec")) ec_value=40; /* _TMP = 40*10+2732 = 3132 K, above _AC0 (fan on) */
    ACPI_STATUS s=fw_inventory_power_run(&ops,0xe0000);
    struct ciuki_power_status p;
    assert(!acpi_power_snapshot(&p));
    unsigned query_writes = writes, query_allocations = allocation_count;
    for (unsigned i=0;i<100;i++) assert(!acpi_power_snapshot(&p));
    assert(writes==query_writes && allocation_count==query_allocations);
    bool fails=!strcmp(scenario,"no-crt") || !strcmp(scenario,"passive") || !strcmp(scenario,"hot") ||
        !strcmp(scenario,"route") || !strcmp(scenario,"enable-timeout") || !strcmp(scenario,"rollback") ||
        !strcmp(scenario,"init-failure") || !strcmp(scenario,"global-lock") || !strcmp(scenario,"shared-power") || !strcmp(scenario,"battery-limit") ||
        !strcmp(scenario,"provider-busy") || !strcmp(scenario,"t23-legacy");
    assert(publishes && !activations == !fw_owned);
    if (fails) {
        assert(s && !fw_owned && !acpi_power_can_off());
        assert(finish_called && !live && !maps);
        /* A refused or failed reservation is never retired by ACPI; an
         * uncertain rollback keeps it so APM can never take over. */
        assert(retires == (reserves && strcmp(scenario,"provider-busy") ? 1u : 0u));
        if (retires) assert(retired_uncertain == !finish_safe);
        if (!strcmp(scenario,"provider-busy")) assert(reserves==1 && !enables && !writes && finish_safe && p.ownership==CIUKI_POWER_FIRMWARE);
        if (!strcmp(scenario,"rollback")) assert(!finish_safe && p.ownership==CIUKI_POWER_QUARANTINED);
        else assert(finish_safe && p.ownership==CIUKI_POWER_FIRMWARE);
        if (enables && strcmp(scenario,"rollback")) assert(io[0x602]==0x80 && io[0x622]==0x80);
        if (!strcmp(scenario,"enable-timeout")) assert(enables==1 && disables==1 && now>=30000000);
        if (!strcmp(scenario,"no-crt") || !strcmp(scenario,"passive") || !strcmp(scenario,"hot") || !strcmp(scenario,"route")) assert(!enables);
        if (!strcmp(scenario,"t23-legacy") || !strcmp(scenario,"no-crt") || !strcmp(scenario,"rollback")) legacy_quirk();
    } else {
        assert(!s && fw_owned && irq_fn && acpi_power_can_off());
        if (!strcmp(scenario,"events")) {
            io[0x601]|=2; fire();
            assert(sleep_requests==1 && !(io[0x601]&2) && !fw_power.shutdown_requests);
            io[0x601]|=2; fire(); assert(sleep_requests==1); /* coalesced press */
        }
        if (!strcmp(scenario,"sleep-button")) {
            io[0x620]|=1;fire();fw_power_poll();fw_power_poll();
            assert(sleep_requests==1 && !fw_power.shutdown_requests);
        }
        assert(p.ownership==CIUKI_POWER_OWNED && p.ac_online==1 && p.lid_open==1 && p.battery_count==1 && p.zone_count==1);
        if (!strcmp(scenario,"unit")) assert(p.batteries[0].unit==CIUKI_POWER_UNKNOWN && p.batteries[0].remaining==CIUKI_POWER_UNKNOWN);
        else assert(p.batteries[0].unit==0 && p.batteries[0].full==5000 && p.batteries[0].remaining==(!strcmp(scenario,"unknown") ? CIUKI_POWER_UNKNOWN : 2500));
        uint32_t on=0; assert(!fw_integer(ACPI_ROOT_OBJECT,"\\_SB.FANR._STA",&on) && on==1);
        if (!strcmp(scenario,"ec-events") || !strcmp(scenario,"ec-timeout")) {
            assert(acpi_ec_quirk_attach()==0 && !acpi_ec_quirk_legacy()); /* fx-3: ACPI-owned engine */
            uint8_t value=0; int e=acpi_ec_quirk_transfer(false,0,&value);
            if (!strcmp(scenario,"ec-timeout")) {
                assert(e==-110 && !fw_ec_ready()); fw_power_poll();
                assert(!fw_owned && finish_called && !finish_safe && retires==1 && retired_uncertain);
                (void)AcpiTerminate(); fw_os_cleanup(); assert(!live && !maps);
                puts("PASS power ec-timeout: bounded engine, retained uncertain ports, no second EC command"); return 0;
            }
            else {
                assert(!e && value==1 && !ec_address);
                value=7; assert(!acpi_ec_quirk_transfer(true,0,&value));
                uint32_t v=0; assert(!fw_integer(ACPI_ROOT_OBJECT,"\\_SB.EC00.READ",&v) && v==7);
                ec_query=true; nested_quirk=true; fw_power_poll(); fw_power_poll();
                assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
                assert(nested_result==-16 && nested_wait>=5*10000 && nested_wait<=7*10000);
                ec_value=0x09; assert(!acpi_ec_quirk_transfer(false,0x30,&value) && value==0x09 && ec_address==0x30);
                value=0x49; assert(!acpi_ec_quirk_transfer(true,0x30,&value) && ec_value==0x49); /* owned: RD/WR 30h */
                puts("PASS power ec-events: ACPI-owned EC serves the volume quirk; busy engine waited, bounded");
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
        if (!strcmp(scenario,"thermal-change") || !strcmp(scenario,"tzp-zero")) {
            /* fx-3 follow-up 2: _TZP absent or 0 = Notify only (ACPI 6.5
             * 11.1.3.2, 11.4.26): a silent change is not polled, the
             * zone's Notify(0x80) is served at the next worker turn. */
            assert(fw_thermal_period_ms(0)==0 && !fw_thermal_ec_backed(0));
            assert(!fw_eval(ACPI_ROOT_OBJECT,"\\_TZ.THRM.HEAT",NULL,NULL));
            for (unsigned i=0;i<60;i++) { now+=10000000; fw_power_poll(); }
            assert(!acpi_power_snapshot(&p) && p.shutdown_requests==0);
            assert(!fw_eval(ACPI_ROOT_OBJECT,"\\_TZ.THRM.NOTE",NULL,NULL)); fw_power_poll();
            assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
            printf("PASS power %s: no thermal polling, Notify(0x80) re-reads the zone\n",scenario);
        }
        if (!strcmp(scenario,"tzp-poll")) {
            assert(fw_thermal_period_ms(0)==5000 && !fw_thermal_ec_backed(0));
            assert(!fw_eval(ACPI_ROOT_OBJECT,"\\_TZ.THRM.HEAT",NULL,NULL));
            now+=10000000; fw_power_poll(); assert(!acpi_power_snapshot(&p) && p.shutdown_requests==0);
            now+=40000000; fw_power_poll(); assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
            puts("PASS power tzp-poll: _TZP=50 polls every 5 s, not before");
        }
        if (!strcmp(scenario,"tzp-ec")) {
            /* _TZP=10 asks for 1 s; the zone's _TMP reads the EC, so it is
             * polled every 10 s at most (f1-100/f2-96 EC budget). */
            assert(fw_thermal_ec_backed(0) && fw_thermal_period_ms(0)==10000);
            unsigned reads=ec_read_commands;
            for (unsigned i=0;i<30;i++) { now+=10000000; fw_power_poll(); }
            unsigned polled=ec_read_commands-reads;
            assert(polled>=2 && polled<=3);
            reads=ec_read_commands; ec_value=110; /* 110*10+2732 = 3832 > _CRT 3700 */
            assert(!fw_eval(ACPI_ROOT_OBJECT,"\\_TZ.THRM.NOTE",NULL,NULL)); fw_power_poll();
            assert(ec_read_commands==reads+1 && !acpi_power_snapshot(&p) && p.shutdown_requests==1);
            ec_value=40;
            printf("PASS power tzp-ec: EC-backed zone polled at 10 s (%u RD_EC in 30 s), Notify read at once\n",polled);
        }
        if (!strcmp(scenario,"eisa-button") || !strcmp(scenario,"cid-button")) {
            io[0x620]|=1; fire(); fw_power_poll(); fw_power_poll();
            assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
        }
        io[0x601]|=1; fire(); fw_power_poll(); /* fixed power button, W1C acknowledgement */
        assert(!(io[0x601]&1)); assert(!acpi_power_snapshot(&p) && p.shutdown_requests==1);
        io[0x620]|=1; fire(); fw_power_poll(); fw_power_poll(); /* AML _L00 -> Notify */
        if (!strcmp(scenario,"gpe-loop")) {
            assert(!fw_owned && finish_called && finish_safe && now<10000000 && retires==1 && !retired_uncertain);
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
        assert(reserves==1 && activations==1 && retires==1 && retired_uncertain==!finish_safe);
        if (!strcmp(scenario,"already-enabled")) assert(!finish_safe && !enables && !disables &&
            !io[0x622] && !io[0x623] && fw_power.ownership==CIUKI_POWER_QUARANTINED);
        assert(!live && !maps);
    }
    printf("PASS power %s: status=%u enables=%u disables=%u writes=%u allocations=%u\n",scenario,s,enables,disables,writes,allocation_count);
    return 0;
}
