/* MIDI parser, native output queue and real FM renderer. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kheap_limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/desktop.h>
#include <ciuki/audio.h>
#include <ciuki/midi_fm.h>
#include <ciuki/opl3.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %u: %s\n",__LINE__,#x); exit(1); } } while (0)
struct audio_state audio;
static unsigned allocations,wakes;
static bool refuse_allocation;
void *(kzalloc)(size_t bytes) { if (bytes > KHEAP_MAX_REQUEST) return 0; if (refuse_allocation) return NULL; void *p=calloc(1,bytes); if (p) allocations++; return p; }
void kfree(void *p) { if (p) { CHECK(allocations); allocations--; free(p); } }
void *desktop_buffer_alloc(uint32_t bytes,enum mm_owner owner)
{ CHECK(owner==MM_IO_REQUEST); if (refuse_allocation) return 0; void *p=calloc(1,bytes); if (p) allocations++; return p; }
void desktop_buffer_free(void *p,uint32_t bytes,enum mm_owner owner)
{ CHECK(bytes && owner==MM_IO_REQUEST); kfree(p); }
void audio_wake(void) { wakes++; }
static uint8_t regs[512];
static void reg_write(void *ctx,unsigned reg,uint8_t value)
{ (void)ctx; CHECK(reg<512); regs[reg]=value; }
static void bytes(struct midi_fm *m,const uint8_t *data,unsigned count)
{ for (unsigned i=0;i<count;i++) midi_fm_byte(m,data[i],reg_write,NULL); }
static unsigned keys(const struct midi_fm *m)
{ unsigned n=0; for (unsigned i=0;i<18;i++) n+=m->voice[i].key; return n; }
static void parser(void)
{
    struct midi_fm m; midi_fm_reset(&m,reg_write,NULL);
    CHECK(!keys(&m) && regs[0x105]==1 && m.channel[0].bend==8192);
    bytes(&m,(const uint8_t[]){0x90,60},2); CHECK(!keys(&m));
    bytes(&m,(const uint8_t[]){0xf8,100,64,80},4); CHECK(keys(&m)==2 && m.status==0x90);
    CHECK((regs[0xb0]&32) && (regs[0xb1]&32));
    bytes(&m,(const uint8_t[]){60,0},2); CHECK(keys(&m)==1); /* running zero velocity */
    bytes(&m,(const uint8_t[]){0xb0,64,127,0x80,64,0},6); CHECK(keys(&m)==1 && m.voice[1].held);
    bytes(&m,(const uint8_t[]){0xb0,64,0},3); CHECK(!keys(&m));
    bytes(&m,(const uint8_t[]){0x90,65,0xf0,1,2,0xf8,0xf7,100},8); CHECK(!keys(&m) && !m.status);
    bytes(&m,(const uint8_t[]){0x90,60,100,0xf2,0,0,62,100},8); CHECK(keys(&m)==1 && !m.status);
    bytes(&m,(const uint8_t[]){0xc1,40,0x91,70,100,0xe1,127,127},8);
    CHECK(m.channel[1].program==40 && m.channel[1].bend==16383);
    for (unsigned n=0;n<128;n++) bytes(&m,(const uint8_t[]){0x92,(uint8_t)n,127},3);
    CHECK(keys(&m)==18);
    bytes(&m,(const uint8_t[]){0xb2,120,0},3); CHECK(keys(&m)<18);
    bytes(&m,(const uint8_t[]){0xff},1); CHECK(!keys(&m) && !m.status);
    uint32_t random=7;
    for (unsigned i=0;i<100000;i++) { random=random*1664525+1013904223; midi_fm_byte(&m,(uint8_t)(random>>24),reg_write,NULL); }
}
static void native_output(void)
{
    audio.caps.output_rate=48000;
    struct audio_stream a={.gain={100,100}},b={.gain={100,100}};
    struct ciuki_midi_data d={.version=1,.size=sizeof(d),.bytes=3,.data={0x90,60,100}};
    refuse_allocation=true; CHECK(audio_midi_write(&a,&d)==-ENOMEM && !a.midi && !d.accepted && !allocations);
    refuse_allocation=false;
    CHECK(!audio_midi_write(&a,&d) && d.accepted==3 && a.source==CIUKI_MIXER_MIDI && allocations==1);
    CHECK(audio_midi_active(&a));
    static int64_t sum[AUDIO_QUANTUM][2];
    audio_midi_mix(&a,sum,AUDIO_QUANTUM);
    int64_t energy=0;
    for (unsigned i=0;i<AUDIO_QUANTUM;i++) energy+=sum[i][0]<0 ? -sum[i][0] : sum[i][0];
    CHECK(energy>0 && audio_midi_active(&a));
    memset(sum,0,sizeof(sum)); audio_midi_mix(&b,sum,AUDIO_QUANTUM); CHECK(!sum[100][0]);
    d.accepted=0; d.bytes=128; memset(d.data,0xf8,sizeof(d.data));
    for (unsigned i=0;i<4;i++) { d.accepted=0; CHECK(!audio_midi_write(&a,&d) && d.accepted==128); }
    d.accepted=0; CHECK(audio_midi_write(&a,&d)==-EAGAIN && !d.accepted);
    audio_midi_mix(&a,sum,AUDIO_QUANTUM); d.accepted=0; CHECK(!audio_midi_write(&a,&d));
    d.accepted=0; d.bytes=129; CHECK(audio_midi_write(&a,&d)==-EINVAL);
    d.bytes=1; d.accepted=1; CHECK(audio_midi_write(&a,&d)==-EINVAL);
    audio_midi_close(&a); CHECK(!allocations && !a.midi && !audio_midi_active(&a));
    a=(struct audio_stream){.gain={100,100}};
    audio.caps.output_rate=44100; d.accepted=0; CHECK(audio_midi_write(&a,&d)==-EOPNOTSUPP && !allocations);
    CHECK(wakes==6);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    parser(); native_output();
    printf("MIDI parser/native FM: running status/realtime/SysEx cancellation/sustain/bend/voice bound/fuzz/queue/backpressure/close PASS; controller=%zu chip=%zu bytes\n",sizeof(struct midi_fm),sizeof(struct opl3_chip));
    return 0;
}
