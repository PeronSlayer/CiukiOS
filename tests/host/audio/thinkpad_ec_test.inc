/* Production EC transactions and mixer integration with a port model. */
static uint8_t ec_native_read_port(uint16_t);
static void ec_native_write_port(uint16_t, uint8_t);
static void ec_native_pause(void);
static uint32_t ec_native_pte(uint32_t);
static void *ec_native_phys(uint32_t);
#undef P2V
#define P2V(p) ec_native_phys(p)
#define inb ec_native_read_port
#define outb ec_native_write_port
#define task_yield ec_native_pause
#define vmm_kernel_pte ec_native_pte
#define native_ec hec
#include "../../../src/kernel/drivers/thinkpad_ec.c"
#undef native_ec
#undef vmm_kernel_pte
#undef task_yield
#undef outb
#undef inb
#undef P2V
#define P2V(p) ((void *)(ram + (p)))
struct ec_model {
    uint64_t tick, ready_at;
    uint8_t reg, reply, stage;
    unsigned reads, writes, pauses, register_reads, volume_writes;
    uint8_t unstable_bits;
    bool absent, stuck_ibf, stuck_obf, reject_write, frozen_clock;
};
static struct ec_model ehw;
static uint8_t ec_bios_memory[0x10000], ec_table_memory[512];
static bool ec_table_mapped=true;
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
    if(port==EC_SC) { CHECK(!h->stage && (value==0x80 || value==0x81));h->stage=value==0x80?1:2; }
    else if(h->stage==1) { CHECK(port==EC_DATA && value==EC_AUDIO);h->reply=h->reg;h->stage=3; }
    else if(h->stage==2) { CHECK(port==EC_DATA && value==EC_AUDIO);h->stage=4; }
    else { CHECK(port==EC_DATA && h->stage==4);h->volume_writes++;if(!h->reject_write)h->reg=value;h->stage=0; }
    h->ready_at=h->tick+1;
}
static uint64_t ec_mock_now(void *ctx) { return ((struct ec_model *)ctx)->tick; }
static void ec_mock_pause(void *ctx) { struct ec_model *h=ctx;h->pauses++;if(!h->frozen_clock)h->tick++; }
static struct thinkpad_ec_io ec_mock_io(void)
{ return (struct thinkpad_ec_io){&ehw,ec_mock_read,ec_mock_write,ec_mock_now,ec_mock_pause}; }
static uint8_t ec_native_read_port(uint16_t p) { return ec_mock_read(&ehw,p); }
static void ec_native_write_port(uint16_t p,uint8_t v) { ec_mock_write(&ehw,p,v); }
static void ec_native_pause(void) { ec_mock_pause(&ehw);g_ticks=ehw.tick; }
static uint32_t ec_native_pte(uint32_t va)
{ CHECK(va==KERNEL_VBASE+0xe0000);return ec_table_mapped?PTE_P:0; }
static void *ec_native_phys(uint32_t p)
{
    if(p==0xe0000) { CHECK(ec_table_mapped);return ec_table_memory; }
    CHECK(p>=0xf0000 && p<0x100000);return ec_bios_memory+p-0xf0000;
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
static void ec_test_entry(size_t bytes)
{
    uint8_t *entry=ec_bios_memory;memset(ec_bios_memory,0,sizeof(ec_bios_memory));
    memcpy(entry,"_SM_",4);entry[5]=31;entry[6]=2;entry[7]=3;memcpy(entry+16,"_DMI_",5);
    entry[22]=bytes;entry[23]=bytes>>8;entry[26]=0x0e;
    uint8_t sum=0;for(unsigned i=16;i<31;i++)sum+=entry[i];entry[21]-=sum;
    sum=0;for(unsigned i=0;i<31;i++)sum+=entry[i];entry[4]-=sum;
}
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
            CHECK(hec.active && audio.master_in_hardware && audio.master.left==21 && ehw.volume_writes==1);
            CHECK(second_claim_next==2 && audio_resources[0].state==RS_ACTIVE && audio_resources[1].state==RS_ACTIVE);
            char record[128];snprintf(record,sizeof(record),"[smbios] entry=ok address=000f0000 table=000e0000 bytes=%zu",n);
            ec_expect_record(0,record);
            ec_expect_record(ec_record_count-1,"[ec] thinkpad volume=3 mute=0");
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
    CHECK(!thinkpad_ec_probe(&hec,&io,true,&m) && hec.active && m.left==21);
    CHECK(ehw.register_reads>=4 && ehw.volume_writes==1 && ehw.reg==0xa3);
    ec_expect_record(ec_record_count-1,"[ec] thinkpad volume=3 mute=0");
    ehw.reg=0xe8;ehw.tick=hec.next_poll;
    CHECK(thinkpad_ec_volume_poll(&hec,&m)==1 && m.left==57 && m.mute);
    ec_expect_record(ec_record_count-1,"[ec] thinkpad volume=8 mute=1");
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
static void test_thinkpad_ec(void)
{
    test_thinkpad_ec_init();
    test_thinkpad_ec_records();
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
    CHECK(!thinkpad_ec_probe(&hec,&io,true,&m)&&hec.active&&m.left==21&&m.right==21&&!m.mute&&ehw.reg==0xa3);
    unsigned reads=ehw.reads;CHECK(!thinkpad_ec_volume_poll(&hec,&m)&&ehw.reads==reads);
    for(unsigned level=0;level<=14;level++) {
        ehw.reg=0xa0|level;ehw.tick=hec.next_poll;
        int changed=thinkpad_ec_volume_poll(&hec,&m);CHECK(changed>=0&&m.left==(level*100+7)/14&&m.right==m.left&&!m.mute);
    }
    ehw.reg=0xe8;ehw.tick=hec.next_poll;CHECK(thinkpad_ec_volume_poll(&hec,&m)==1&&m.left==57&&m.mute);
    ehw.reg=0xa8;ehw.tick=hec.next_poll;CHECK(thinkpad_ec_volume_poll(&hec,&m)==1&&!m.mute); /* firmware up/down unmutes */
    hardware_setup(&ac97);ac97.software_master=false;audio.ready=true;audio.master_in_hardware=true;
    audio.master=m;ehw.reg=0xa7;ehw.tick=hec.next_poll;audio_master_refresh();
    CHECK(audio.master.left==50&&!audio.master.mute&&!hw.codec[2/2]&&!hw.codec[4/2]);
    m=(struct ciuki_mixer_master){.version=1,.size=32,.left=20,.right=20};
    CHECK(!audio_master_set(&m)&&m.left==21&&ehw.reg==0xa3&&!hw.codec[2/2]);
    /* EC observations do not write settings or block the refill worker on disk. */
    uint32_t sequence=saved_sequence;ehw.reg=0xe4;ehw.tick=hec.next_poll;audio_master_refresh();
    CHECK(audio.master.left==29&&audio.master.mute&&audio.master.save_state==CIUKI_MIXER_UNSAVED&&saved_sequence==sequence&&hw.codec[2/2]==0x8000);
    audio.master_in_hardware=false;audio.ready=false;
    for(unsigned fault=0;fault<5;fault++) {
        memset(&ehw,0,sizeof(ehw));ehw.reg=7;m=(struct ciuki_mixer_master){.left=20,.right=20};
        ehw.absent=fault==0;ehw.stuck_ibf=fault==1;ehw.stuck_obf=fault==2;ehw.reject_write=fault==3;
        if(fault==4){ehw.stuck_ibf=true;ehw.frozen_clock=true;}
        io=ec_mock_io();CHECK(thinkpad_ec_probe(&hec,&io,true,&m)<0&&!hec.active&&ehw.tick<=80&&ehw.pauses<=4096);
        CHECK(strstr(audio_log,"[ec] thinkpad gate=ec_handshake_failed stage=probe error="));
        reads=ehw.reads;CHECK(thinkpad_ec_volume_poll(&hec,&m)==-ENODEV&&ehw.reads==reads);
    }
    memset(&ehw,0,sizeof(ehw));ehw.reg=15;io=ec_mock_io();CHECK(thinkpad_ec_probe(&hec,&io,true,&m)==-EIO);
    puts("ThinkPad EC: IBM/DMI gates, timed IBF/OBF read/write/readback, 4 Hz levels/mute, preserved bits, amplifier unity and bounded faults PASS");
}
