/* F4-32: production VM renderer, 32 distinct 64-unit blocks at 8 kHz.
 * Source queue is a deterministic f3-05 boundary, not emulated hardware. */
#include <stdio.h>
#include <stdlib.h>
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include "../vm_audio_shim.h"
/* Weak only so this fixture can measure the unmodified implementation. */
extern void audio_vm_reanchor(void) __attribute__((weak));
#include "../../../src/kernel/audio/vm_source.c"
struct audio_state audio;
volatile uint64_t g_ticks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint64_t clock_now, source_start;
static unsigned block_index, block_offset;
bool dosvm_audio_info(struct dosvm_key key, struct dosvm_audio_info *out)
{
    CHECK(key.id==1);
    *out=(struct dosvm_audio_info){.clock=clock_now,.master={31,31},.voice={31,31},.fm={31,31},
        .blocks_queued=32-block_index,.pcm_queued=(32-block_index)*64-block_offset};
    return true;
}
int dosvm_audio_fetch(struct dosvm_key key, uint32_t token, struct dosvm_audio_block *b, int16_t *out, unsigned n)
{
    CHECK(key.id==1 && token==1);
    if(block_index==32) return -EAGAIN;
    *b=(struct dosvm_audio_block){.start=source_start+block_index*384,.generation=1,.sequence=block_index+1,
        .rate=8000,.frames=64,.offset=block_offset,.closed=true,.flags=DOSVM_BLOCK_COMPLETE};
    if(n>64-block_offset) n=64-block_offset;
    for(unsigned i=0;i<n;i++) out[2*i]=out[2*i+1]=(int16_t)(1000+block_index*100);
    block_offset+=n;
    b->closed=block_offset==64;
    if(block_offset==64) { block_index++; block_offset=0; }
    return (int)n;
}
int dosvm_audio_events(struct dosvm_key key,uint32_t token,struct dosvm_audio_event *out,unsigned max)
{ (void)key;(void)token;(void)out;(void)max; return 0; }
bool dosvm_audio_opl_registers(struct dosvm_key key,uint8_t regs[512],uint64_t *clock)
{ (void)key;(void)regs;(void)clock; return false; }
static int64_t sums[AUDIO_QUANTUM][2];
static int16_t reference[16000], recovered[16000];
static unsigned capture(unsigned rate, unsigned recovery, bool retained, int16_t *out)
{
    memset(vm_slots,0,sizeof(vm_slots)); memset(&audio,0,sizeof(audio));
    vm_rate_cached=0; vm_rate_next=0; rate_epoch++; template_ready=true;
    vm_slots[0]=(struct vm_slot){.state=AUDIO_VM_ACTIVE,.key={1,1},.epoch=1,.token=1};
    vm_slots[0].gain[0]=vm_slots[0].gain[1]=100;
    audio.master.left=audio.master.right=100; audio.caps.output_rate=rate;
    audio.lead_periods=AUDIO_VM_LEAD; audio.output=AUDIO_VM_LEAD*AUDIO_QUANTUM;
    audio.played_output=audio.output-(audio.lead_periods-1)*AUDIO_QUANTUM;
    clock_now=10000; source_start=clock_now-AUDIO_VM_FLOOR-AUDIO_QUANTUM*48000u/rate;
    block_index=block_offset=0;
    unsigned sound=0, quanta=rate==48000 ? 60 : 56;
    for(unsigned q=0;q<quanta;q++) {
        if(((recovery==1 || recovery==3) && q==2) || ((recovery==2 || recovery==4) && q==0)) {
            /* Retained quantum: already rendered, refused by CIV/PIV, then
             * published once before the remaining silence prime. */
            if(retained) {
                memset(sums,0,sizeof(sums)); audio_vm_mix(sums,AUDIO_QUANTUM);
                for(unsigned k=0;k<AUDIO_QUANTUM;k++) if(sums[k][0]) out[sound++]=(int16_t)sums[k][0];
                audio.output+=AUDIO_QUANTUM; clock_now+=AUDIO_QUANTUM*48000u/rate;
            }
            void (*volatile reanchor)(void)=audio_vm_reanchor;
            if(reanchor) reanchor();
            if(recovery==3 || recovery==4) audio.lead_periods=28;
            audio.output+=audio.lead_periods*AUDIO_QUANTUM;
            /* The failing probe lost 127 output frames / 21 source frames
             * before its first mix. Case 4 instead drains the full new
             * 2048-frame lead in one callback before any source render,
             * matching transient_test.inc's production transport recovery.
             * Priming is synchronous, not DMA time. */
            clock_now+=recovery==2 ? 127 : recovery==3 ? 5000 : AUDIO_VM_LEAD*AUDIO_QUANTUM*48000u/rate;
        }
        audio.played_output=audio.output-(audio.lead_periods-1)*AUDIO_QUANTUM;
        memset(sums,0,sizeof(sums)); audio_vm_mix(sums,AUDIO_QUANTUM);
        for(unsigned k=0;k<AUDIO_QUANTUM;k++) if(sums[k][0]) out[sound++]=(int16_t)sums[k][0];
        audio.output+=AUDIO_QUANTUM; clock_now+=AUDIO_QUANTUM*48000u/rate;
    }
    struct audio_vm_stats *s=&vm_slots[0].stats;
    unsigned fresh=0;
    for(unsigned i=0;i<s->log_count;i++) if(s->log[i].frames==64 && s->log[i].flags==DOSVM_BLOCK_COMPLETE) fresh++;
    printf("F4-32 VM recovery: rate=%u recovery=%u retained=%u blocks=%u fresh=%u frames=%u pcm_frames=%llu late=%llu skips=%u vm_frames=%llu\n",
        rate,recovery,retained,s->blocks,fresh,sound,(unsigned long long)s->pcm_frames,
        (unsigned long long)s->late_frames,s->skips,(unsigned long long)s->pcm.frames);
    CHECK(fresh==32 && s->blocks==32 && s->pcm_frames==2048 && !s->late_frames && !s->skips);
    CHECK(s->pcm.frames==12288);
    return sound;
}
int main(void)
{
    setvbuf(stdout,0,_IOLBF,0);
    for(unsigned r=0;r<2;r++) {
        unsigned rate=r ? 44100 : 48000;
        unsigned expected=capture(rate,false,false,reference);
        if(!r) CHECK(expected==12288);
        for(unsigned retained=0;retained<2;retained++) {
            unsigned got=capture(rate,true,retained,recovered);
            for(unsigned i=0;!r && i<got && i<expected;i++) if(reference[i]!=recovered[i]) {
                printf("first difference: frame=%u expected=%d got=%d\n",i,reference[i],recovered[i]); break;
            }
            CHECK(r ? (got>expected ? got-expected : expected-got)<=2 : got==expected);
            /* Fractional physical resampling can interpolate the inserted
             * silence edges differently. 48 kHz must be sample-exact;
             * both rates must retain every source frame and fresh block. */
            if(!r) CHECK(!memcmp(reference,recovered,got*sizeof(*reference)));
        }
        unsigned startup=capture(rate,2,false,recovered);
        CHECK(r ? (startup>expected ? startup-expected : expected-startup)<=2 : startup==expected);
        if(!r) CHECK(!memcmp(reference,recovered,startup*sizeof(*reference)));
        unsigned capacity=capture(rate,4,false,recovered);
        CHECK(r ? (capacity>expected ? capacity-expected : expected-capacity)<=2 : capacity==expected);
        if(!r) CHECK(!memcmp(reference,recovered,capacity*sizeof(*reference)));
        printf("F4-32 VM capacity recovery: rate=%u requested_frames=%u fresh=32 vm_frames=12288 pcm_frames=2048 late=0 PASS\n",
            rate,AUDIO_VM_LEAD*AUDIO_QUANTUM);
        unsigned adapted=capture(rate,3,false,recovered);
        CHECK(r ? (adapted>expected ? adapted-expected : expected-adapted)<=2 : adapted==expected);
        if(!r) CHECK(!memcmp(reference,recovered,adapted*sizeof(*reference)));
    }
    puts("F4-32 VM recovery: exact 48 kHz PCM; all source frames retained at 48/44.1 kHz PASS");
}
