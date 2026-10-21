/* Production plan/checkpoint/audio/selector tests; no guest or device access.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/reboot.h>
#include "../../src/kernel/probes/dos_title_model.h"
#include "../../src/kernel/probes/selector.h"
static uint8_t wire[D53_PLAN_BYTES], bad[D53_PLAN_BYTES];
static struct d53_plan plan;
static void selector_tests(void)
{
    struct probe_selection selected;
    const char *good="f3:title run=1234abcd title=synthetic";
    assert(probes_parse_selector(good,strlen(good),0,&selected));
    assert(selected.phase==3 && !strcmp(selected.title,"synthetic"));
    const char *invalid[]={"f3:title run=1234abcd", "f3:title run=1234abcd title=",
        "f3:title run=1234abcd title=../x", "f3:title run=1234abcd title=Upper",
        "f3:title run=1234abcd title=x title=y", "f3:title title=x run=1234abcd",
        "f3:title run=1234abcd title=x safe=1", "f3:title run=1234abcd title=x platform=e500",
        "f3:title run=1234abcd title=abcdefghijklmnopqrstuvwxyz"};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)
        assert(!probes_parse_selector(invalid[i],strlen(invalid[i]),0,&selected));
    const struct probe_def probes[]={{"vm-window",0},{"title",0},{"dos-conformance",0}};
    struct probe_tables table={.f3=probes,.f3_count=3};
    struct sweep_step step;
    assert(sweep_count(3,&table)==1 && sweep_at(3,0,&table,&step));
    assert(!strcmp(step.name,"vm-window") && !sweep_at(3,1,&table,&step));
}
int main(int argc,char **argv)
{
    assert(argc==3);
    FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(wire,1,sizeof(wire),f)==sizeof(wire) && fgetc(f)==EOF);fclose(f);
    assert(d53_parse(wire,sizeof(wire),&plan));
    assert(!strcmp(plan.title,"synthetic") && plan.irq==5 && plan.audio==3);
    assert(!d53_parse(wire,sizeof(wire)-1,&plan));
    const unsigned offsets[]={0,31,159,160,164,168,179,180,172+76,403+8};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        memcpy(bad,wire,sizeof(bad));bad[offsets[i]]=255;
        assert(!d53_parse(bad,sizeof(bad),&plan));
    }
    assert(d53_parse(wire,sizeof(wire),&plan));
    uint16_t cells[2000]={0};
    struct d53_checkpoint text={.text=1,.value="MAIN MENU"};
    for (unsigned i=0;i<9;i++) cells[80+12+i]=0x1700|text.value[i];
    assert(d53_match(&text,"",cells)); cells[80+12]='x';assert(!d53_match(&text,"",cells));
    memset(cells,0,sizeof(cells));
    for (unsigned i=0;i<9;i++) cells[78+i]=text.value[i];
    assert(!d53_match(&text,"",cells)); /* no matching across row boundaries */
    assert(d53_match(&plan.cp[0],plan.cp[0].value,0));
    assert(!d53_match(&plan.cp[0],"different",0));
    struct d53_audio audio={.pcm_frames=1000,.opl_writes=10,.blocks=4,.irq_entries=4,
        .irq=5,.attached=true,.ready=true,.irq_latency_max_us=20000};
    assert(d53_audio_ok(&plan,&audio));
    audio.irq_latency_max_us++;assert(!d53_audio_ok(&plan,&audio));audio.irq_latency_max_us=1;
    audio.underruns=1;assert(!d53_audio_ok(&plan,&audio));audio.underruns=0;
    audio.irq=7;assert(!d53_audio_ok(&plan,&audio));audio.irq=5;
    audio.attached=false;assert(!d53_audio_ok(&plan,&audio));audio.attached=true;
    audio.ready=false;assert(!d53_audio_ok(&plan,&audio));audio.ready=true;
    audio.opl_writes=0;assert(!d53_audio_ok(&plan,&audio));audio.opl_writes=10;
    audio.pcm_frames=0;assert(!d53_audio_ok(&plan,&audio));
    static uint8_t extended[D53_V2_BYTES], malformed[D53_V2_BYTES];
    f=fopen(argv[2],"rb");assert(f);
    assert(fread(extended,1,sizeof(extended),f)==sizeof(extended) && fgetc(f)==EOF);fclose(f);
    assert(d53_parse(extended,sizeof(extended),&plan));
    assert(plan.extended && plan.calibration && plan.stages==1 && plan.stage[0].count==2);
    assert(plan.stage[0].timeout==5000 && !strcmp(plan.stage[0].command,"\\RUNREC.COM \\SYNTH SETUP.EXE"));
    assert(d53_match(&plan.cp[0],"unmatched",0)); /* capture is never matched=1 */
    const unsigned v2_offsets[]={D53_PLAN_BYTES,D53_PLAN_BYTES+4,D53_PLAN_BYTES+8+147,
        D53_PLAN_BYTES+8+216,D53_PLAN_BYTES+8+220,D53_PLAN_BYTES+8+224,
        D53_PLAN_BYTES+8+D53_STAGE_BYTES};
    for (unsigned i=0;i<sizeof(v2_offsets)/sizeof(v2_offsets[0]);i++) {
        memcpy(malformed,extended,sizeof(extended));malformed[v2_offsets[i]]=255;
        assert(!d53_parse(malformed,sizeof(malformed),&plan));
    }
    memcpy(malformed,extended,sizeof(extended)); malformed[D53_PLAN_BYTES]=0;
    assert(!d53_parse(malformed,sizeof(malformed),&plan)); /* verification rejects capture checkpoints */
    assert(!d53_parse(extended,sizeof(extended)-1,&plan));
    selector_tests();puts("PASS: production DOS title v1/v2 plans, installer bounds, capture isolation, audio and selector/sweep");
    return 0;
}
