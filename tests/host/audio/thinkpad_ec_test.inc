/* Production EC transactions and mixer integration with a port model. */
static uint8_t ec_native_read_port(uint16_t);
static void ec_native_write_port(uint16_t, uint8_t);
static void ec_native_sleep(uint32_t);
static uint32_t ec_native_pte(uint32_t);
static void *ec_native_phys(uint32_t);
static const void *ec_native_map_ro(uint32_t, uint32_t);
static void ec_native_unmap(void *, uint32_t);
#undef P2V
#define P2V(p) ec_native_phys(p)
#define inb ec_native_read_port
#define outb ec_native_write_port
#define task_sleep_ms ec_native_sleep
#define vmm_kernel_pte ec_native_pte
#define vmm_map_physical_ro ec_native_map_ro
#define vmm_revoke_mmio ec_native_unmap
#define native_ec hec
#include "../../../src/kernel/drivers/thinkpad_ec.c"
#undef native_ec
#undef vmm_revoke_mmio
#undef vmm_map_physical_ro
#undef vmm_kernel_pte
#undef task_sleep_ms
#undef outb
#undef inb
#undef P2V
#define P2V(p) ((void *)(ram + (p)))
struct ec_model {
    uint64_t tick, ready_at;
    uint8_t reg, reply, stage;
    unsigned reads, writes, pauses, register_reads, volume_writes, busy_every, write_delay;
    uint8_t unstable_bits;
    bool absent, stuck_ibf, stuck_obf, reject_write, frozen_clock;
};
static struct ec_model ehw;
static uint8_t ec_bios_memory[0x10000], ec_table_memory[65536];
static bool ec_table_mapped=true;
static uint32_t ec_table_phys=0xe0000, ec_table_bytes;
static uint32_t ec_missing_page=UINT32_MAX, ec_wrong_page=UINT32_MAX;
static unsigned ec_map_calls, ec_unmap_calls, ec_pte_calls;
static bool ec_map_failed, ec_window_active;
static uint8_t ec_mock_read(void *ctx, uint16_t port)
{
    struct ec_model *h=ctx;h->reads++;
    if(h->absent) return 0xff;
    if(port==EC_SC) return (h->stuck_ibf || h->tick<h->ready_at ? EC_IBF : 0) |
        (h->stage==3 && !h->stuck_obf ? EC_OBF : 0);
    CHECK(port==EC_DATA && h->stage==3);h->stage=0;h->register_reads++;
    uint8_t reply=h->reply;
    if(h->register_reads==1)h->reg^=h->unstable_bits;
    return reply;
}
static void ec_mock_write(void *ctx,uint16_t port,uint8_t value)
{
    struct ec_model *h=ctx;h->writes++;CHECK(h->tick>=h->ready_at);
    if(port==EC_SC) {
        CHECK(value==0x80 || value==0x81);CHECK(!h->stuck_ibf);
        CHECK(h->stage!=3 || h->stuck_obf); /* command restarts incomplete input */
        h->stage=value==0x80?1:2;
    }
    else if(h->stage==1) { CHECK(port==EC_DATA && value==EC_AUDIO);h->reply=h->reg;h->stage=3; }
    else if(h->stage==2) { CHECK(port==EC_DATA && value==EC_AUDIO);h->stage=4; }
    else { CHECK(port==EC_DATA && h->stage==4);h->volume_writes++;if(!h->reject_write)h->reg=value;h->stage=0; }
    h->ready_at=h->tick+(h->write_delay ? h->write_delay :
        h->busy_every && h->writes%h->busy_every==0 ? 100 : 1);
}
static uint64_t ec_mock_now(void *ctx) { return ((struct ec_model *)ctx)->tick; }
static void ec_mock_pause(void *ctx) { struct ec_model *h=ctx;h->pauses++;if(!h->frozen_clock)h->tick++; }
static struct thinkpad_ec_io ec_mock_io(void)
{ return (struct thinkpad_ec_io){&ehw,ec_mock_read,ec_mock_write,ec_mock_now,ec_mock_pause}; }
static uint8_t ec_native_read_port(uint16_t p) { return ec_mock_read(&ehw,p); }
static void ec_native_write_port(uint16_t p,uint8_t v) { ec_mock_write(&ehw,p,v); }
static void ec_native_sleep(uint32_t ms) { CHECK(ms==1 || ms==EC_POLL_MS); while(ms--)ec_mock_pause(&ehw);g_ticks=ehw.tick; }
static uint32_t ec_native_pte(uint32_t va)
{
    CHECK(va>=KERNEL_VBASE && va<KERNEL_VBASE+PMM_MAX_PAGES*PAGE_SIZE);
    uint32_t p=va-KERNEL_VBASE;ec_pte_calls++;
    CHECK(p>=PAGE_ALIGN_DOWN(ec_table_phys) && (uint64_t)p<(uint64_t)ec_table_phys+ec_table_bytes);
    if(!ec_table_mapped || p==ec_missing_page)return 0;
    return (p==ec_wrong_page?p+PAGE_SIZE:p)|PTE_P;
}
static void *ec_native_phys(uint32_t p)
{
    if(p==ec_table_phys) { CHECK(ec_table_mapped);return ec_table_memory; }
    CHECK(p>=0xf0000 && p<0x100000);return ec_bios_memory+(p-0xf0000);
}
static const void *ec_native_map_ro(uint32_t p, uint32_t bytes)
{
    CHECK(p==ec_table_phys && bytes==ec_table_bytes && !ec_window_active);
    ec_map_calls++;
    if(ec_map_failed)return NULL;
    ec_window_active=true;return ec_table_memory;
}
static void ec_native_unmap(void *p, uint32_t bytes)
{
    CHECK(p==ec_table_memory && bytes==ec_table_bytes && ec_window_active);
    ec_unmap_calls++;ec_window_active=false;
    /* All logged strings must survive revocation as owned copies. */
    memset(ec_table_memory,0xa5,sizeof(ec_table_memory));
}
static size_t ec_test_table(uint8_t *table, const char *vendor, const char *product, const char *version)
{
    /* Deliberately use reordered string indices for both types. */
    const uint8_t bios[]={0,6,0,0,2,1};
    memcpy(table,bios,sizeof(bios));size_t n=sizeof(bios);
    const char *strings[]={"1AET64WW (1.20 )","IBM"};
    for(unsigned i=0;i<2;i++){size_t bytes=strlen(strings[i])+1;memcpy(table+n,strings[i],bytes);n+=bytes;}
    table[n++]=0;
    const uint8_t system[]={1,8,1,0,3,1,2,0};
    memcpy(table+n,system,sizeof(system));n+=sizeof(system);
    const char *identity[]={product,version,vendor};
    for(unsigned i=0;i<3;i++){size_t bytes=strlen(identity[i])+1;memcpy(table+n,identity[i],bytes);n+=bytes;}
    table[n++]=0;
    const uint8_t end[]={127,4,2,0,0,0};memcpy(table+n,end,sizeof(end));return n+sizeof(end);
}
static void ec_capture_begin(void)
{ ec_record_count=0;capture_ec_records=true; }
static void ec_expect_record(unsigned index, const char *record)
{ CHECK(index<ec_record_count && !strcmp(ec_records[index],record)); }
static void ec_test_entry_at(size_t bytes, uint32_t address, unsigned offset, uint8_t minor)
{
    CHECK(offset<=sizeof(ec_bios_memory)-31 && !(offset&15));
    ec_table_phys=address;ec_table_bytes=bytes;
    memset(ec_bios_memory,0,sizeof(ec_bios_memory));uint8_t *entry=ec_bios_memory+offset;
    memcpy(entry,"_SM_",4);entry[5]=31;entry[6]=2;entry[7]=minor;memcpy(entry+16,"_DMI_",5);
    entry[22]=bytes;entry[23]=bytes>>8;
    for(unsigned i=0;i<4;i++)entry[24+i]=address>>(8*i);
    uint8_t sum=0;for(unsigned i=16;i<31;i++)sum+=entry[i];entry[21]-=sum;
    sum=0;for(unsigned i=0;i<31;i++)sum+=entry[i];entry[4]-=sum;
}
static void ec_test_entry(size_t bytes) { ec_test_entry_at(bytes,0xe0000,0,3); }
static void test_thinkpad_ec_init(void)
{
    struct audio_state saved_audio=audio;uint32_t flags=g_boot.flags;uint64_t ticks=g_ticks;
    for(unsigned scenario=0;scenario<12;scenario++) {
        const char *vendor=scenario==2?"QEMU":scenario==3?"Dell Inc.":"IBM";
        size_t n=ec_test_table(ec_table_memory,vendor,"26475FU","Not Available");ec_test_entry(n);
        if(scenario==4)memset(ec_bios_memory,0,sizeof(ec_bios_memory));
        if(scenario==5)ec_bios_memory[4]++;
        if(scenario==7)ec_test_entry(n-1);
        ec_table_mapped=scenario!=6;
        memset(&ehw,0,sizeof(ehw));ehw.reg=7;ehw.absent=scenario==8;
        memset(&hec,0,sizeof(hec));memset(audio_resources,0,sizeof(audio_resources));
        second_claim_next=0;second_claim_failure=scenario==10?0:scenario==11?1:-1;
        audio.master=(struct ciuki_mixer_master){.left=20,.right=20};audio.master_in_hardware=false;
        g_boot.flags=scenario==1?CBI_F_SAFE_MODE:scenario==9?CBI_F_SMBIOS_QEMU:0;g_ticks=0;ec_capture_begin();
        /* Exercise the actual safe-mode audio_init early-return ordering. */
        if(scenario==1) { audio.initialized=false;audio_init(NULL);CHECK(audio.reason && !strcmp(audio.reason,"safe_mode")); }
        else thinkpad_ec_init();
        CHECK(ec_record_count>=4 && strstr(ec_records[1],"[smbios] version=") && strstr(ec_records[2],"[smbios] bios vendor="));
        if(!scenario) {
            CHECK(hec.active && audio.master_in_hardware && audio.master.left==20 && ehw.volume_writes==0);
            CHECK(second_claim_next==2 && audio_resources[0].state==RS_ACTIVE && audio_resources[1].state==RS_ACTIVE);
            char record[128];snprintf(record,sizeof(record),"[smbios] entry=ok address=000f0000 table=000e0000 bytes=%zu",n);
            ec_expect_record(0,record);
            ec_expect_record(ec_record_count-1,"[ec] thinkpad volume=7 mute=0");
        } else {
            const char *reason=scenario==1?"safe_mode":(scenario==2 || scenario==9)?"qemu":scenario==3?"not_ibm":(scenario==8 || scenario>=10)?"ec_handshake_failed":"no_smbios";
            char expected[80];snprintf(expected,sizeof(expected),"[ec] thinkpad gate=%s",reason);
            CHECK(!hec.active && !audio.master_in_hardware && strstr(ec_records[ec_record_count-1],expected));
            if(scenario==8)CHECK(second_claim_next==2 && audio_resources[0].state==RS_QUARANTINED && audio_resources[1].state==RS_QUARANTINED);
            else if(scenario==11) {
                CHECK(!ehw.reads && !ehw.writes && second_claim_next==1 && audio_resources[0].state==RS_QUARANTINED);
                ec_expect_record(4,"[ec] thinkpad gate=ec_handshake_failed stage=claim_command error=-16");
            } else CHECK(!ehw.reads && !ehw.writes && !second_claim_next);
            if(scenario==10)ec_expect_record(4,"[ec] thinkpad gate=ec_handshake_failed stage=claim_data error=-16");
        }
        const char *verdict=scenario==4?"not_found":scenario==5?"bad_entry":scenario==6?"unmapped_table":scenario==7?"malformed_table":"ok";
        char expected[64];snprintf(expected,sizeof(expected),"[smbios] entry=%s ",verdict);CHECK(strstr(ec_records[0],expected));
    }
    ec_table_mapped=true;capture_ec_records=false;memset(&hec,0,sizeof(hec));
    second_claim_next=0;second_claim_failure=-1;audio=saved_audio;g_boot.flags=flags;g_ticks=ticks;
    puts("ThinkPad EC init: production BIOS scan/checksums/mapping, T23 handshake, off-gate records, safe audio init and port quarantine PASS");
}
static void test_thinkpad_ec_records(void)
{
    uint8_t table[512];struct thinkpad_ec_smbios s;
    size_t n=ec_test_table(table,"IBM","26475FU","Not Available");
    CHECK(thinkpad_ec_smbios_decode(table,n,&s) && s.eligible && s.hint && s.has_bios && s.has_system);
    CHECK(thinkpad_ec_identity(table,n));
    s.major=2;s.minor=3;
    ec_capture_begin();thinkpad_ec_smbios_log(&s);
    ec_expect_record(0,"[smbios] entry=ok address=00000000 table=00000000 bytes=0");
    ec_expect_record(1,"[smbios] version=2.3 vendor=\"IBM\" product=\"26475FU\" version=\"Not Available\" type1=1");
    ec_expect_record(2,"[smbios] bios vendor=\"IBM\" version=\"1AET64WW (1.20 )\" type0=1");
    struct ciuki_mixer_master m={.left=20,.right=20};
    memset(&ehw,0,sizeof(ehw));ehw.reg=0xa7;struct thinkpad_ec_io io=ec_mock_io();
    CHECK(thinkpad_ec_gate(&s,0));ec_expect_record(3,"[ec] thinkpad hint=1");
    CHECK(!thinkpad_ec_probe(&hec,&io,true,&m) && hec.active && m.left==20);
    CHECK(ehw.register_reads>=4 && ehw.volume_writes==0 && ehw.reg==0xa7);
    ec_expect_record(ec_record_count-1,"[ec] thinkpad volume=7 mute=0");
    ehw.reg=0xe8;ehw.tick=hec.next_poll;
    CHECK(thinkpad_ec_volume_poll(&hec,&m)==1 && m.left==25 && m.mute);
    ec_expect_record(ec_record_count-1,"[ec] thinkpad volume=8 mute=1");
    /* EC level is an edge source. Wraps count as one key press and master
     * changes stay in five point increments at both ends of the range. */
    ehw.reg=0xe9; ehw.tick=hec.next_poll;
    CHECK(thinkpad_ec_volume_poll(&hec,&m)==1 && m.left==30);
    ehw.reg=0xee; ehw.tick=hec.next_poll;
    CHECK(thinkpad_ec_volume_poll(&hec,&m)==1 && m.left==30);
    ehw.reg=0xe0; ehw.tick=hec.next_poll;
    CHECK(thinkpad_ec_volume_poll(&hec,&m)==1 && m.left==35);
    ehw.reg=0xee; ehw.tick=hec.next_poll;
    CHECK(thinkpad_ec_volume_poll(&hec,&m)==1 && m.left==30);
    m.left=m.right=100; ehw.reg=0xe0; ehw.tick=hec.next_poll;
    CHECK(thinkpad_ec_volume_poll(&hec,&m)==1 && m.left==100);
    m.left=m.right=0; ehw.reg=0xee; ehw.tick=hec.next_poll;
    CHECK(thinkpad_ec_volume_poll(&hec,&m)==1 && m.left==0);
    /* Both flags exclude a recognized IBM system before any port access. */
    for(unsigned i=0;i<2;i++) {
        memset(&ehw,0,sizeof(ehw));ec_capture_begin();
        CHECK(!thinkpad_ec_gate(&s,i?CBI_F_SMBIOS_QEMU:CBI_F_SAFE_MODE));
        ec_expect_record(1,i?"[ec] thinkpad gate=qemu":"[ec] thinkpad gate=safe_mode");
        CHECK(!ehw.reads && !ehw.writes);
    }
    const char *vendors[]={"QEMU","Dell Inc.","IBM Corporation","LENOVO","LENOVO","IBM"};
    const char *products[]={"Standard PC (i440FX + PIIX, 1996)","26475FU","ThinkPad T23","20X5S12300","IdeaPad","Unknown"};
    for(unsigned i=0;i<6;i++) {
        n=ec_test_table(table,vendors[i],products[i],"Not Available");
        CHECK(thinkpad_ec_smbios_decode(table,n,&s));
        memset(&ehw,0,sizeof(ehw));ec_capture_begin();thinkpad_ec_smbios_log(&s);
        CHECK(strstr(ec_records[1],vendors[i]) && strstr(ec_records[1],products[i]));
        bool eligible=i==3 || i==5;CHECK(thinkpad_ec_gate(&s,0)==eligible);
        if(!eligible)ec_expect_record(4,i?"[ec] thinkpad gate=not_ibm":"[ec] thinkpad gate=qemu");
        else ec_expect_record(3,i==3?"[ec] thinkpad hint=1":"[ec] thinkpad hint=0");
        CHECK(!ehw.reads && !ehw.writes);
    }
    n=ec_test_table(table,"LENOVO","ThinkPad T60","Not Available");CHECK(thinkpad_ec_identity(table,n));
    n=ec_test_table(table,"LENOVO","Unknown","ThinkPad T60");CHECK(thinkpad_ec_identity(table,n));
    n=ec_test_table(table,"LENOVO","2647-5FU","Not Available");CHECK(thinkpad_ec_identity(table,n));
    /* Printable copies stay within klog capacity; control/quote bytes cannot
     * inject another record. Eligibility still checks the original vendor. */
    char long_string[200];memset(long_string,'X',sizeof(long_string));long_string[199]=0;
    n=ec_test_table(table,"IBM\n","a\"b\\c\td\x80",long_string);
    CHECK(thinkpad_ec_smbios_decode(table,n,&s) && !s.eligible && strlen(s.version)==47);
    ec_capture_begin();thinkpad_ec_smbios_log(&s);
    CHECK(!strcmp(s.vendor,"IBM?") && !strcmp(s.product,"a?b?c?d?"));
    CHECK(strstr(ec_records[1],"vendor=\"IBM?\" product=\"a?b?c?d?\"") && !strchr(ec_records[1],'\n'));
    n=ec_test_table(table,long_string,"01234567890123456789012345678901234567890123456789",long_string);
    CHECK(thinkpad_ec_smbios_decode(table,n,&s));ec_capture_begin();thinkpad_ec_smbios_log(&s);
    CHECK(strlen(ec_records[1])<255 && strstr(ec_records[1],"type1=1"));
    /* Missing type-1 identity and malformed tail fail closed. */
    const uint8_t empty[]={127,4,0,0,0,0};
    CHECK(thinkpad_ec_smbios_decode(empty,sizeof(empty),&s) && !s.has_system);
    ec_capture_begin();thinkpad_ec_smbios_log(&s);CHECK(!thinkpad_ec_gate(&s,0));
    ec_expect_record(1,"[smbios] version=0.0 vendor=\"\" product=\"\" version=\"\" type1=0");
    ec_expect_record(4,"[ec] thinkpad gate=no_smbios");
    n=ec_test_table(table,"IBM","26475FU","Not Available");
    for(size_t i=0;i<n;i++) {
        /* Exact record boundaries are valid for older tables without type127. */
        if(thinkpad_ec_smbios_decode(table,i,&s))continue;
        CHECK(!s.valid);ec_capture_begin();CHECK(!thinkpad_ec_gate(&s,0));
        ec_expect_record(1,"[ec] thinkpad gate=no_smbios");
    }
    CHECK(!thinkpad_ec_smbios_decode(table,n-1,&s));
    ec_capture_begin();thinkpad_ec_smbios_log(&s);CHECK(!thinkpad_ec_gate(&s,0));
    ec_expect_record(0,"[smbios] entry=malformed_table address=00000000 table=00000000 bytes=0");
    ec_expect_record(4,"[ec] thinkpad gate=no_smbios");
    for(unsigned bits=1;bits<=0x80;bits<<=1) {
        memset(&ehw,0,sizeof(ehw));ehw.reg=4;ehw.unstable_bits=bits;io=ec_mock_io();
        m=(struct ciuki_mixer_master){.left=20,.right=20};ec_capture_begin();
        CHECK(thinkpad_ec_probe(&hec,&io,true,&m)==-EIO && !hec.active);
        CHECK(ehw.register_reads==2 && !ehw.volume_writes && m.left==20 && !m.mute);
        ec_expect_record(0,"[ec] thinkpad gate=ec_handshake_failed stage=probe error=-5");
    }
    capture_ec_records=false;
    puts("ThinkPad EC records: T23 type-model, BIOS/system strings, printable bounds, QEMU/vendor/safe gates and stable read before write PASS");
}
static void test_thinkpad_ec_reserved_table(void)
{
    struct ciuki_boot_info saved_boot=g_boot;struct audio_state saved_audio=audio;uint64_t ticks=g_ticks;
    for(unsigned scenario=0;scenario<21;scenario++) {
        memset(ec_table_memory,0,sizeof(ec_table_memory));
        size_t n=ec_test_table(ec_table_memory,"IBM","26475FU","Not Available");
        uint32_t address=0x1ff7c000,bytes=1717;
        if(scenario==1 || scenario==4 || scenario==7 || scenario==11) { address+=0xff0;bytes=n; }
        if(scenario==2) { address=0x40000ff0;bytes=n; }
        if(scenario==3) { address=PMM_MAX_PAGES*PAGE_SIZE-16;bytes=n; }
        if(scenario==17)bytes=n-1; /* physically present terminator outside the declared extent */
        if(scenario==18)bytes=UINT16_MAX;
        if(scenario==20)bytes=PAGE_SIZE;
        ec_test_entry_at(bytes,address,0x7350,31);
        memset(&g_boot,0,sizeof(g_boot));g_boot.e820_count=1;
        uint32_t start=PAGE_ALIGN_DOWN(address),end=PAGE_ALIGN_UP(address+bytes);
        g_boot.e820[0]=(struct ciuki_e820){.base=start,.length=end-start,.type=2,.ext=1};
        if(!scenario) {
            /* A realistic normalized map has allocator RAM before the table. */
            g_boot.e820_count=3;g_boot.e820[2]=g_boot.e820[0];
            g_boot.e820[0]=(struct ciuki_e820){.base=0,.length=0xa0000,.type=CBI_E820_RAM,.ext=1};
            g_boot.e820[1]=(struct ciuki_e820){.base=0x100000,.length=start-0x100000,.type=CBI_E820_RAM,.ext=1};
        }
        ec_table_mapped=scenario==5 || scenario==7 || scenario==19;
        ec_missing_page=scenario==7?start+PAGE_SIZE:UINT32_MAX;
        ec_wrong_page=scenario==19?start:UINT32_MAX;
        ec_map_failed=scenario==16;ec_window_active=false;
        ec_map_calls=ec_unmap_calls=ec_pte_calls=0;
        if(scenario==4) {
            /* Coverage may span two adjacent reserved descriptors. */
            g_boot.e820_count=2;g_boot.e820[0].length=PAGE_SIZE;
            g_boot.e820[1]=(struct ciuki_e820){.base=start+PAGE_SIZE,.length=PAGE_SIZE,.type=2,.ext=1};
        }
        if(scenario==5 || scenario==6 || scenario==7 || scenario==19)g_boot.e820[0].type=CBI_E820_RAM;
        if(scenario==8) { g_boot.e820[0].base=address;g_boot.e820[0].length=bytes; }
        if(scenario==9) { g_boot.e820[0].base++;g_boot.e820[0].length--; }
        if(scenario==10)g_boot.e820[0].length--;
        if(scenario==11) {
            g_boot.e820_count=2;g_boot.e820[0].length=PAGE_SIZE-1;
            g_boot.e820[1]=(struct ciuki_e820){.base=start+PAGE_SIZE,.length=PAGE_SIZE,.type=2,.ext=1};
        }
        if(scenario==12)g_boot.e820_count=0;
        if(scenario>=13 && scenario<=15)g_boot.e820[0].type=scenario-10;
        memset(&ehw,0,sizeof(ehw));ehw.reg=7;
        memset(&hec,0,sizeof(hec));memset(audio_resources,0,sizeof(audio_resources));
        second_claim_next=0;second_claim_failure=-1;
        audio.master=(struct ciuki_mixer_master){.left=20,.right=20};audio.master_in_hardware=false;
        g_ticks=0;ec_capture_begin();thinkpad_ec_init();
        bool success=scenario<=5 || scenario==18 || scenario==20;
        bool attempted=success?scenario!=5:scenario==16 || scenario==17;
        CHECK(ec_map_calls==attempted && ec_unmap_calls==(attempted && !ec_map_failed) && !ec_window_active);
        CHECK(hec.active==success && audio.master_in_hardware==success);
        char record[128];snprintf(record,sizeof(record),"[smbios] entry=%s address=000f7350 table=%08x bytes=%u",
            success?"ok":scenario==17?"malformed_table":"unmapped_table",address,bytes);
        ec_expect_record(0,record);
        if(success) {
            ec_expect_record(1,"[smbios] version=2.31 vendor=\"IBM\" product=\"26475FU\" version=\"Not Available\" type1=1");
            ec_expect_record(2,"[smbios] bios vendor=\"IBM\" version=\"1AET64WW (1.20 )\" type0=1");
            CHECK(audio.master.left==20 && ehw.volume_writes==0 && second_claim_next==2);
            ec_expect_record(ec_record_count-1,"[ec] thinkpad volume=7 mute=0");
            ehw.reg=0xe8;ehw.tick=hec.next_poll;g_ticks=ehw.tick;
            CHECK(thinkpad_ec_master_poll(&audio.master)==1 && audio.master.left==25 && audio.master.mute);
        } else {
            ec_expect_record(ec_record_count-1,"[ec] thinkpad gate=no_smbios");
            CHECK(!ehw.reads && !ehw.writes && !second_claim_next);
        }
        if(scenario==2 || scenario==3)CHECK(!ec_pte_calls); /* no probing unrelated/wrapped virtual addresses */
        if(scenario==7)CHECK(ec_pte_calls==2); /* all pages, not just the first */
    }
    /* A table ending exactly at 4 GiB can be borrowed; an overflowing entry
     * cannot reach the mapper. No type-1 record means no EC port access. */
    for(unsigned overflow=0;overflow<2;overflow++) {
        memcpy(ec_table_memory,(const uint8_t[]){127,4,0,0,0,0},6);
        ec_test_entry_at(6,0xfffffffa+overflow,0x7350,31);
        g_boot.e820_count=1;g_boot.e820[0]=(struct ciuki_e820){.base=0xfffff000,.length=PAGE_SIZE,.type=2,.ext=1};
        ec_map_calls=ec_unmap_calls=ec_pte_calls=0;ec_map_failed=false;ec_capture_begin();
        struct thinkpad_ec_smbios s;native_identity(&s);thinkpad_ec_smbios_log(&s);
        CHECK(!strcmp(s.verdict,overflow?"bad_entry":"ok") && !s.has_system && !ec_window_active);
        CHECK(ec_map_calls==!overflow && ec_unmap_calls==!overflow && !ec_pte_calls);
    }
    ec_table_mapped=true;ec_table_phys=0xe0000;ec_missing_page=ec_wrong_page=UINT32_MAX;
    ec_map_failed=false;capture_ec_records=false;memset(&hec,0,sizeof(hec));
    second_claim_next=0;second_claim_failure=-1;g_boot=saved_boot;audio=saved_audio;g_ticks=ticks;
    puts("ThinkPad SMBIOS reserved window: T23 1ff7c000/1717, unaligned/cross-page/above-direct-map tables, copied strings, EC master, E820 refusals, map failure, malformed cleanup and 4 GiB bounds PASS");
}
static unsigned ec_records_containing(const char *needle)
{
    unsigned n=0;
    for(unsigned i=0;i<ec_record_count;i++)if(strstr(ec_records[i],needle))n++;
    return n;
}
static void test_thinkpad_ec_tolerance(void)
{
    audio_settings_load(&vfs);
    struct ciuki_mixer_master m={.version=1,.size=32,.left=20,.right=20};
    memset(&ehw,0,sizeof(ehw));ehw.reg=0xa7;ehw.busy_every=3;
    struct thinkpad_ec_io io=ec_mock_io();
    CHECK(!thinkpad_ec_probe(&hec,&io,true,&m) && hec.available && ehw.pauses>=100);
    ec_capture_begin();
    for(unsigned i=0;i<20;i++) {
        ehw.reg=0xa0|(i%15);ehw.tick=hec.next_poll;
        uint64_t start=ehw.tick;CHECK(thinkpad_ec_volume_poll(&hec,&m)>=0);
        CHECK(hec.active && hec.available && !hec.failures && ehw.tick-start<=500);
    }
    hardware_setup(&ac97);ac97.software_master=false;
    audio.ready=true;audio.master_in_codec=true;audio.master_in_hardware=true;audio.master=m;
    uint32_t seq=saved_sequence;
    m=(struct ciuki_mixer_master){.version=1,.size=32,.left=35,.right=35};
    CHECK(!audio_master_set(&m) && m.left==35 && m.save_state==CIUKI_MIXER_SAVED);
    CHECK(saved_sequence==seq+1 && !hw.codec[2/2] && hec.available);
    CHECK(!ec_records_containing("timeout") && !ec_records_containing("disabled"));

    /* A dead EC fails eight operations, not eight constituent transfers.
     * User writes during backoff save exact stereo/mute without retrying it. */
    ehw.busy_every=0;ehw.stuck_ibf=true;
    /* A 250 ms cadence (the pre-f1-94 4 Hz) still exercises the 1 s
     * timeout-record limit; the default 1 Hz cadence is checked below. */
    hec.poll_ms=250;
    for(unsigned i=1;i<=8;i++) {
        ehw.tick=hec.next_poll;uint64_t start=ehw.tick;
        seq=saved_sequence;audio_master_refresh();
        CHECK(ehw.tick-start==500 && hec.failures==i && !hec.available);
        CHECK(hec.active==(i<8) && !audio.master_in_hardware && saved_sequence==seq);
        CHECK(ec_records_containing("disabled")== (i==8));
        unsigned reads=ehw.reads;
        m=(struct ciuki_mixer_master){.version=1,.size=32,.left=25,.right=75,.mute=i%2};
        CHECK(!audio_master_set(&m) && m.left==25 && m.right==75 && m.save_state==CIUKI_MIXER_SAVED);
        CHECK(ehw.reads==reads && saved_sequence==seq+1 && audio.master.mute==i%2);
        CHECK(hw.codec[2/2]==(uint16_t)(0x0802|(m.mute?0x8000:0)));
        struct ciuki_mixer_master loaded=audio.master;
        audio_settings_load(&vfs);
        CHECK(audio.master.left==loaded.left && audio.master.right==loaded.right && audio.master.mute==loaded.mute);
    }
    CHECK(ec_records_containing("timeout stage=ibf ms=500")>=1);
    CHECK(ec_records_containing("timeout")<8); /* one-second rate limit */
    CHECK(hec.next_poll==ehw.tick+10000);
    hec.poll_ms=EC_POLL_MS;
    unsigned reads=ehw.reads;ehw.tick=hec.next_poll-1;audio_master_refresh();CHECK(ehw.reads==reads);
    ehw.tick=hec.next_poll;audio_master_refresh();
    CHECK(!hec.active && hec.failures==8 && ec_records_containing("disabled")==1);
    CHECK(hec.next_poll==ehw.tick+10000);
    ehw.stuck_ibf=false;ehw.reg=0xe1;ehw.tick=hec.next_poll;seq=saved_sequence;
    audio_master_refresh();
    CHECK(hec.active && hec.available && !hec.failures && audio.master_in_hardware);
    CHECK(audio.master.left==25 && audio.master.right==75 && !audio.master.mute && ehw.reg==0xa1);
    CHECK(saved_sequence==seq && ec_records_containing("resumed")==1);

    /* A lone timeout must recover at 250 ms with an offline software master. */
    ac97.software_master=true;audio.master_in_codec=false;
    ehw.stuck_ibf=true;ehw.tick=hec.next_poll;
    m=(struct ciuki_mixer_master){.version=1,.size=32,.left=80,.right=80,.mute=1};seq=saved_sequence;
    uint64_t set_start=ehw.tick;
    CHECK(!audio_master_set(&m) && saved_sequence==seq+1 && m.save_state==CIUKI_MIXER_SAVED);
    CHECK(ehw.tick-set_start==500 && m.left==80 && m.mute);
    CHECK(hec.active && hec.failures==1 && !audio.master_in_hardware);
    struct audio_stream *stream;CHECK(!audio_stream_open(&stream));
    struct audio_frame frame={10000,10000},out;stream->rate=48000;
    CHECK(audio_stream_enqueue(stream,&frame,sizeof(frame))==sizeof(frame));
    audio_mix(&out,1);CHECK(!out.left && !out.right);
    m.mute=0;m.save_state=0;m.save_error=0;CHECK(!audio_master_set(&m));
    CHECK(audio_stream_enqueue(stream,&frame,sizeof(frame))==sizeof(frame));
    audio_mix(&out,1);CHECK(out.left==8000 && out.right==8000);
    m.mute=1;m.save_state=0;m.save_error=0;CHECK(!audio_master_set(&m));
    audio_stream_close(stream);
    ehw.stuck_ibf=false;ehw.tick=hec.next_poll;audio_master_refresh();
    CHECK(hec.available && hec.active && !hec.failures && audio.master_in_hardware &&
          audio.master.left==80 && audio.master.right==80 && audio.master.mute && (ehw.reg&EC_MASK)==0x41);

    /* Late output belongs to the old read and is drained before recovery. */
    ehw.stuck_obf=true;ehw.tick=hec.next_poll;audio_master_refresh();
    CHECK(hec.pending_read && hec.failures==1 && !audio.master_in_hardware);
    ehw.stuck_obf=false;ehw.tick=hec.next_poll;audio_master_refresh();
    CHECK(hec.available && !hec.pending_read && !hec.failures);
    CHECK(ec_records_containing("timeout stage=obf ms=500"));

    /* Firmware reset loses the pending reply; a fresh command must recover. */
    ehw.tick+=1000;ehw.stuck_obf=true;ehw.tick=hec.next_poll+1000;audio_master_refresh();
    CHECK(hec.pending_read && !hec.available);
    ehw.stage=0;ehw.stuck_obf=false;ehw.tick=hec.next_poll;audio_master_refresh();
    CHECK(hec.available && !hec.pending_read && !hec.failures);

    /* Even read/write/readback together cannot exceed the task budget. */
    ehw.tick+=1000;ehw.write_delay=100;m=(struct ciuki_mixer_master){.version=1,.size=32,.left=20,.right=20};
    uint64_t start=ehw.tick;CHECK(thinkpad_ec_volume_set(&hec,&m)==-ETIMEDOUT);
    CHECK(ehw.tick-start==500 && hec.active && !hec.available && hec.failures==1);
    CHECK(ec_records_containing("timeout stage=write ms=500"));
    /* Frozen clock still exhausts a bounded number of sleeping polls. */
    ehw.write_delay=0;ehw.stuck_ibf=true;ehw.frozen_clock=true;ehw.tick=hec.next_poll;
    unsigned pauses=ehw.pauses;CHECK(thinkpad_ec_volume_poll(&hec,&m)==-ETIMEDOUT);
    CHECK(ehw.pauses-pauses==500 && hec.failures==2);
    memset(&ehw,0,sizeof(ehw));ehw.reg=7;ehw.write_delay=600;io=ec_mock_io();
    CHECK(thinkpad_ec_probe(&hec,&io,true,&m)==-ETIMEDOUT && ehw.tick==500);
    CHECK(ec_records_containing("timeout stage=read ms=500"));
    capture_ec_records=false;memset(&hec,0,sizeof(hec));
    audio.ready=audio.master_in_codec=audio.master_in_hardware=false;
    puts("ThinkPad EC tolerance: 100 ms busy windows, shared 500 ms budget, sleeping polls, offline codec/software master persistence, eight failures, 10 s reprobe, late reply, resync and rate-limited records PASS");
}
static void test_thinkpad_ec(void)
{
    test_thinkpad_ec_init();
    test_thinkpad_ec_records();
    test_thinkpad_ec_reserved_table();
    const uint8_t table[]={1,8,0,0,1,2,3,0,'I','B','M',0,'2','6','4','7',0,'T','h','i','n','k','P','a','d',' ','T','2','3',0,0,127,4,0,0,0,0};
    CHECK(thinkpad_ec_identity(table,sizeof(table)));
    uint8_t bad[sizeof(table)];memcpy(bad,table,sizeof(table));bad[8]='L';CHECK(!thinkpad_ec_identity(bad,sizeof(bad)));
    memcpy(bad,table,sizeof(table));bad[1]=3;CHECK(!thinkpad_ec_identity(bad,sizeof(bad)));
    for(unsigned n=0;n<sizeof(table);n++) if(n<31) CHECK(!thinkpad_ec_identity(table,n));
    uint8_t entry[31]={0};memcpy(entry,"_SM_",4);entry[5]=31;memcpy(entry+16,"_DMI_",5);
    entry[22]=sizeof(table);entry[25]=0xe0;/* set both checksums below */
    uint8_t sum=0;for(unsigned i=16;i<31;i++)sum+=entry[i];entry[21]-=sum;
    sum=0;for(unsigned i=0;i<31;i++)sum+=entry[i];entry[4]-=sum;
    uint32_t addr;uint16_t len;CHECK(thinkpad_ec_entry(entry,31,&addr,&len)&&addr==0xe000&&len==sizeof(table));
    entry[4]++;CHECK(!thinkpad_ec_entry(entry,31,&addr,&len));
    struct ciuki_mixer_master m={.version=1,.size=32,.left=20,.right=20};
    memset(&ehw,0,sizeof(ehw));ehw.reg=0xa7;struct thinkpad_ec_io io=ec_mock_io();
    CHECK(thinkpad_ec_probe(&hec,&io,false,&m)==-ENODEV && !ehw.reads && !ehw.writes);
    CHECK(!thinkpad_ec_probe(&hec,&io,true,&m)&&hec.active&&m.left==20&&m.right==20&&!m.mute&&ehw.reg==0xa7);
    unsigned reads=ehw.reads;CHECK(!thinkpad_ec_volume_poll(&hec,&m)&&ehw.reads==reads);
    for(unsigned i=1;i<=15;i++) {
        unsigned level=(7+i)%15;
        ehw.reg=0xa0|level;ehw.tick=hec.next_poll;
        int changed=thinkpad_ec_volume_poll(&hec,&m);
        CHECK(changed==1&&m.left==20u+i*5u&&m.right==m.left&&!m.mute);
    }
    ehw.reg=0xe7;ehw.tick=hec.next_poll;CHECK(thinkpad_ec_volume_poll(&hec,&m)==1&&m.left==95&&m.mute);
    ehw.reg=0xa7;ehw.tick=hec.next_poll;CHECK(thinkpad_ec_volume_poll(&hec,&m)==1&&!m.mute);
    hardware_setup(&ac97);ac97.software_master=false;audio.ready=true;audio.master_in_hardware=true;
    audio.master=m;ehw.reg=0xa7;ehw.tick=hec.next_poll;audio_master_refresh();
    CHECK(audio.master.left==95&&!audio.master.mute);
    m=(struct ciuki_mixer_master){.version=1,.size=32,.left=20,.right=20};
    CHECK(!audio_master_set(&m)&&m.left==20&&ehw.reg==0xa7);
    /* EC observations save the new master preference through the normal path. */
    uint32_t sequence=saved_sequence;ehw.reg=0xe4;ehw.tick=hec.next_poll;audio_master_refresh();
    CHECK(audio.master.left==20&&audio.master.mute&&audio.master.save_state==CIUKI_MIXER_UNSAVED&&saved_sequence==sequence&&hw.codec[2/2]==0x8000);
    audio.master_in_hardware=false;audio.ready=false;
    for(unsigned fault=0;fault<5;fault++) {
        memset(&ehw,0,sizeof(ehw));ehw.reg=7;m=(struct ciuki_mixer_master){.left=20,.right=20,.mute=fault==3};
        ehw.absent=fault==0;ehw.stuck_ibf=fault==1;ehw.stuck_obf=fault==2;ehw.reject_write=fault==3;
        if(fault==4){ehw.stuck_ibf=true;ehw.frozen_clock=true;}
        io=ec_mock_io();CHECK(thinkpad_ec_probe(&hec,&io,true,&m)<0&&!hec.active&&ehw.tick<=EC_TIMEOUT_MS&&ehw.pauses<=EC_TIMEOUT_MS);
        CHECK(strstr(audio_log,"[ec] thinkpad gate=ec_handshake_failed stage=probe error="));
        reads=ehw.reads;CHECK(thinkpad_ec_volume_poll(&hec,&m)==-ENODEV&&ehw.reads==reads);
    }
    memset(&ehw,0,sizeof(ehw));ehw.reg=15;io=ec_mock_io();CHECK(thinkpad_ec_probe(&hec,&io,true,&m)==-EIO);
    test_thinkpad_ec_tolerance();
    /* f1-94 amendment 1: 1 Hz default cadence, BOOT.CFG ecpoll=0 stops all
     * periodic EC port traffic; exact token, loader separators. */
    CHECK(EC_POLL_MS==1000u && thinkpad_ec_poll_option("",128)==1000u);
    static const char *const off[]={"ecpoll=0","safe=1 ecpoll=0","serial=0\r\necpoll=0\r\n"," ecpoll=0 usbscan=0"};
    static const char *const on[]={"ecpoll=1","ecpoll=00","xecpoll=0","ECPOLL=0","ecpoll=","usbscan=0 accel=0"};
    for(unsigned i=0;i<4;i++) CHECK(thinkpad_ec_poll_option(off[i],strlen(off[i])+1)==0);
    for(unsigned i=0;i<6;i++) CHECK(thinkpad_ec_poll_option(on[i],strlen(on[i])+1)==1000u);
    CHECK(thinkpad_ec_poll_option("ecpoll=01",8)==0); /* bounded: the 9th byte is outside */
    memset(&ehw,0,sizeof(ehw));ehw.reg=0xa7;io=ec_mock_io();m=(struct ciuki_mixer_master){.left=20,.right=20};
    CHECK(!thinkpad_ec_probe(&hec,&io,true,&m)&&hec.poll_ms==1000u);
    uint64_t probed=ehw.tick;CHECK(hec.next_poll==probed+1000u);
    ehw.reg=0xe8;ehw.tick=hec.next_poll-1;reads=ehw.reads;
    CHECK(!thinkpad_ec_volume_poll(&hec,&m)&&ehw.reads==reads);           /* not due before 1 s */
    ehw.tick=hec.next_poll;CHECK(thinkpad_ec_volume_poll(&hec,&m)==1&&hec.next_poll==ehw.tick+1000u);
    hec.poll_ms=0;ehw.reg=0xe9;ehw.tick=hec.next_poll+5000;reads=ehw.reads;unsigned writes=ehw.writes;
    CHECK(!thinkpad_ec_volume_poll(&hec,&m)&&ehw.reads==reads&&ehw.writes==writes&&m.left==25);
    puts("ThinkPad EC: IBM/DMI gates, timed IBF/OBF read/write/readback, 1 Hz levels/mute, ecpoll=0 no traffic, preserved bits, amplifier unity and bounded faults PASS");
}
