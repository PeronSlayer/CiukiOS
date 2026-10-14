/* Production EC transactions and mixer integration with a port model. */
#define THINKPAD_EC_HOST
#include "../../../src/kernel/drivers/thinkpad_ec.c"
struct ec_model {
    uint64_t tick, ready_at;
    uint8_t reg, reply, stage;
    unsigned reads, writes, pauses;
    bool absent, stuck_ibf, stuck_obf, reject_write, frozen_clock;
};
static struct ec_model ehw;
static struct thinkpad_ec hec;
static uint8_t ec_mock_read(void *ctx, uint16_t port)
{
    struct ec_model *h=ctx;h->reads++;
    if(h->absent) return 0xff;
    if(port==EC_SC) return (h->stuck_ibf || h->tick<h->ready_at ? EC_IBF : 0) |
        (h->stage==3 && !h->stuck_obf ? EC_OBF : 0);
    CHECK(port==EC_DATA && h->stage==3);h->stage=0;return h->reply;
}
static void ec_mock_write(void *ctx,uint16_t port,uint8_t value)
{
    struct ec_model *h=ctx;h->writes++;CHECK(h->tick>=h->ready_at);
    if(port==EC_SC) { CHECK(!h->stage && (value==0x80 || value==0x81));h->stage=value==0x80?1:2; }
    else if(h->stage==1) { CHECK(port==EC_DATA && value==EC_AUDIO);h->reply=h->reg;h->stage=3; }
    else if(h->stage==2) { CHECK(port==EC_DATA && value==EC_AUDIO);h->stage=4; }
    else { CHECK(port==EC_DATA && h->stage==4);if(!h->reject_write)h->reg=value;h->stage=0; }
    h->ready_at=h->tick+1;
}
static uint64_t ec_mock_now(void *ctx) { return ((struct ec_model *)ctx)->tick; }
static void ec_mock_pause(void *ctx) { struct ec_model *h=ctx;h->pauses++;if(!h->frozen_clock)h->tick++; }
static struct thinkpad_ec_io ec_mock_io(void)
{ return (struct thinkpad_ec_io){&ehw,ec_mock_read,ec_mock_write,ec_mock_now,ec_mock_pause}; }
void thinkpad_ec_init(void) {}
void thinkpad_ec_start(void) {}
int thinkpad_ec_master_set(struct ciuki_mixer_master *m) { return thinkpad_ec_volume_set(&hec,m); }
int thinkpad_ec_master_poll(struct ciuki_mixer_master *m) { return thinkpad_ec_volume_poll(&hec,m); }
static void test_thinkpad_ec(void)
{
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
        io=ec_mock_io();CHECK(thinkpad_ec_probe(&hec,&io,true,&m)<0&&!hec.active&&ehw.tick<=60&&ehw.pauses<=4096);
        reads=ehw.reads;CHECK(thinkpad_ec_volume_poll(&hec,&m)==-ENODEV&&ehw.reads==reads);
    }
    memset(&ehw,0,sizeof(ehw));ehw.reg=15;io=ec_mock_io();CHECK(thinkpad_ec_probe(&hec,&io,true,&m)==-EIO);
    puts("ThinkPad EC: IBM/DMI gates, timed IBF/OBF read/write/readback, 4 Hz levels/mute, preserved bits, amplifier unity and bounded faults PASS");
}
