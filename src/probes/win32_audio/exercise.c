/* Consumed PCM and callbacks through the real WinMM worker. LGPL-2.1-or-later */
#include "../../../sdk/win32/audio/audio.h"
#include "../win32_core/fixture.h"
extern uint32_t W32API waveOutOpen(uintptr_t *,uint32_t,const struct wa_format *,uintptr_t,uintptr_t,uint32_t);
extern uint32_t W32API waveOutPrepareHeader(uintptr_t,struct wa_header *,uint32_t);
extern uint32_t W32API waveOutUnprepareHeader(uintptr_t,struct wa_header *,uint32_t);
extern uint32_t W32API waveOutWrite(uintptr_t,struct wa_header *,uint32_t);
extern uint32_t W32API waveOutReset(uintptr_t);
extern uint32_t W32API waveOutClose(uintptr_t);
struct position {uint32_t type,value[2];};
extern uint32_t W32API waveOutGetPosition(uintptr_t,struct position *,uint32_t);
static volatile uint32_t completed;
static void W32API callback(uintptr_t h,uint32_t message,uintptr_t cookie,uintptr_t header,uintptr_t unused)
{
    (void)h;(void)cookie;(void)header;(void)unused;
    if(message==0x3bd)__atomic_add_fetch(&completed,1,__ATOMIC_RELEASE);
}
static int16_t pcm[2048];
void W32API entry(void)
{
    fixture_begin(W32_FIXTURE_AUDIO);
    /* Two facade handles must share one process engine/native stream. */
    struct wa_format format={1,2,22050,88200,4,16,0};uintptr_t wave=0,other=0;
    CHECK(0,!waveOutOpen(&wave,0xffffffffu,&format,(uintptr_t)callback,0,WA_CALLBACK_FUNCTION));
    CHECK(0,!waveOutOpen(&other,0xffffffffu,&format,0,0,0));
    uint32_t error=0;struct wa_engine *engine=CiukiAudioAcquire(format.rate,&error);
    CHECK(0,engine && !error);
    for(unsigned i=0;i<2048;i++)pcm[i]=(int16_t)((i&63u)*512u-16384u);
    struct wa_header header={.data=(char *)pcm,.bytes=sizeof(pcm)};
    CHECK(0,!waveOutPrepareHeader(wave,&header,sizeof(header)));
    CHECK(0,!waveOutWrite(wave,&header,sizeof(header)));
    uint32_t peak=0;struct position position={.type=2};
    for(unsigned i=0;i<10000;i++) {
        if(engine) {
            CiukiAudioLock();
            uint64_t lead=engine->accepted-engine->consumed;
            if(lead>peak)peak=(uint32_t)lead;
            result->row[0].value[1]=engine->opened;
            CiukiAudioUnlock();
        }
        CHECK(0,!waveOutGetPosition(wave,&position,sizeof(position)) && position.type==2);
        if(__atomic_load_n(&completed,__ATOMIC_ACQUIRE))break;
        Sleep(1);
    }
    result->row[0].value[0]=peak;
    result->row[0].value[2]=position.value[0]==1024 && (header.flags&WA_DONE) && !(header.flags&WA_QUEUED);
    result->row[0].value[3]=__atomic_load_n(&completed,__ATOMIC_ACQUIRE)==1;
    CHECK(0,result->row[0].value[2] && result->row[0].value[3] && peak<=512);
    CHECK(0,!waveOutReset(wave));
    CHECK(0,!waveOutUnprepareHeader(wave,&header,sizeof(header)));
    CHECK(0,!waveOutClose(wave));CHECK(0,!waveOutClose(other));
    fixture_finish();
}
