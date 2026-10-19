/* Numeric lateness bucket edges and pending OPL ordering under a common
 * timestamp shift. The integration companion uses real SB16/DMA production. */
#include <stdio.h>
#include <stdlib.h>
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include "../vm_audio_shim.h"
#include "../../../src/kernel/audio/vm_source.c"
struct audio_state audio;
volatile uint64_t g_ticks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
int dosvm_audio_events(struct dosvm_key k, uint32_t token, struct dosvm_audio_event *e, unsigned n)
{ (void)k; (void)token; (void)e; (void)n; return 0; }
bool dosvm_audio_opl_registers(struct dosvm_key k, uint8_t r[512], uint64_t *clock)
{ (void)k; (void)r; (void)clock; return false; }
static struct vm_slot slot;
static unsigned queued_offset;
int dosvm_audio_fetch(struct dosvm_key k, uint32_t token, struct dosvm_audio_block *b, int16_t *out, unsigned n)
{
    (void)k; (void)token;
    if (queued_offset==64) return -EAGAIN;
    *b=(struct dosvm_audio_block){.start=200,.rate=8000,.frames=64,.offset=queued_offset,
        .generation=1,.sequence=2,.flags=DOSVM_BLOCK_COMPLETE};
    if (n>64-queued_offset) n=64-queued_offset;
    for (unsigned i=0;i<n;i++) out[i*2]=out[i*2+1]=1000;
    queued_offset+=n; b->closed=queued_offset==64;
    return (int)n;
}
bool dosvm_audio_info(struct dosvm_key k, struct dosvm_audio_info *out)
{
    (void)k; *out=(struct dosvm_audio_info){.clock=744}; return true;
}
static int64_t sums[AUDIO_QUANTUM][2];
int main(void)
{
    const unsigned ages[]={47,48,239,240,960,961};
    for (unsigned i=0;i<ARRAY_SIZE(ages);i++) late_histogram(&slot,(uint64_t)ages[i]*VM_HZ,VM_HZ,1);
    late_histogram(&slot,960ull*11025+1,11025,1); /* just over 20 ms */
    CHECK(slot.stats.late_hist[0]==1 && slot.stats.late_hist[1]==2 &&
          slot.stats.late_hist[2]==2 && slot.stats.late_hist[3]==2);
    slot.key=(struct dosvm_key){1,1}; slot.probe_allow=true;
    slot.stats.delay_frames=240; slot.ev_count=2;
    slot.ev[0]=(struct dosvm_audio_event){.clock=100,.generation=1,.kind=DOSVM_EVENT_OPL,.reg=0x20,.value=1};
    slot.ev[1]=(struct dosvm_audio_event){.clock=148,.generation=1,.kind=DOSVM_EVENT_OPL,.reg=0x40,.value=2};
    opl3_init(&slot.chip);
    slot.next_t=339; CHECK(events_due(&slot,1)==340 && !slot.stats.opl_events);
    slot.next_t=340; CHECK(events_due(&slot,1)==388 && slot.stats.opl_events==1);
    slot.next_t=387; CHECK(events_due(&slot,1)==388 && slot.stats.opl_events==1);
    slot.next_t=388; CHECK(events_due(&slot,1)==UINT64_MAX && slot.stats.opl_events==2 && !slot.stats.late_events);
    puts("F4-35 histogram: exact 1/5/20 ms and fractional >20 ms edges; OPL writes shifted together by 240 frames, spacing/order retained PASS");
    /* An old window ends exactly where an OPL write is due. The next PCM
     * segment is late: discover that boundary before committing the write. */
    memset(&slot,0,sizeof(slot)); opl3_init(&slot.chip);
    slot.key=(struct dosvm_key){1,1}; slot.state=AUDIO_VM_ACTIVE; slot.live_epoch=1;
    slot.anchored=slot.stream=true; slot.next_t=440; slot.rendered_to=8192;
    slot.rate=8000; slot.idx=slot.win_first=64;
    slot.ev_count=1; slot.ev[0]=(struct dosvm_audio_event){.clock=440,.generation=1,
        .kind=DOSVM_EVENT_OPL,.reg=0x20,.value=1};
    audio.lead_periods=AUDIO_VM_LEAD;
    render(&slot,1,1,sums,1,8192,8192-7*AUDIO_QUANTUM);
    CHECK(slot.stats.delay_frames==240 && slot.stats.reanchors==1 && !slot.stats.opl_events);
    CHECK(events_due(&slot,1)==680 && slot.ev_count==1);
    puts("F4-35 PCM boundary: late restart delays an OPL write due on the exhausted-window edge PASS");
}
