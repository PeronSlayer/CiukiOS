/* Coalescing preserves independent controls and the original master request.
 * Exercises the production queue, including its real 64-byte job capacity.
 * SPDX-License-Identifier: MIT */
#include <ciuki/abi.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../apps/desktop/settings_io.h"
static struct cfg_writer settings_writer;
static uint32_t mixer_serial;
#include "../../../apps/desktop/mixer_job.h"
static int unused_job(void *record,size_t n) { (void)record; (void)n; return 0; }
int main(void)
{
    assert(!pthread_mutex_init(&settings_writer.lock,NULL));
    assert(!pthread_cond_init(&settings_writer.changed,NULL));
    settings_writer.live=1; settings_writer.run_job=unused_job;
    struct mixer_job job={.fd=17,.mask=1,.master={.version=1,.size=32,.left=40,.right=60,.mute=1}};
    assert(!mixer_submit(&job));
    for (unsigned i=1;i<=5;i++) {
        job=mixer_pending;
        struct ciuki_mixer_line line={.line=i,.left=10*i,.right=100-i,.mute=i&1};
        assert(!mixer_pack_line(&job,&line) && !mixer_submit(&job));
    }
    job=mixer_pending;
    struct ciuki_mixer_line recording={.line=CIUKI_MIXER_LINE,.recording=1,.left=75,.right=75,.selected=1};
    assert(!mixer_pack_line(&job,&recording) && !mixer_submit(&job));
    job=mixer_pending;
    struct ciuki_mixer_line invalid=recording; invalid.line=CIUKI_MIXER_MIDI;
    assert(mixer_pack_line(&job,&invalid)==-EINVAL);
    /* A later Wave movement keeps the older master, other playback lines
     * and the recording Select; the settings worker coalesces one blob. */
    job=mixer_pending;
    struct ciuki_mixer_line wave={.line=CIUKI_MIXER_WAVE,.left=88,.right=22};
    assert(!mixer_pack_line(&job,&wave) && !mixer_submit(&job));
    struct mixer_job queued; memcpy(&queued,settings_writer.job,sizeof(queued));
    assert(settings_writer.job_length==64 && queued.mask==(0x3f | (1u<<9)));
    assert(queued.master.left==40 && queued.master.right==60 && queued.master.mute==1);
    assert((queued.values[0]&127)==88 && ((queued.values[0]>>7)&127)==22);
    assert((queued.values[1]&127)==20 && ((queued.values[1]>>7)&127)==98);
    assert((queued.values[8]>>15)==1 && (queued.values[8]&127)==75);
    struct ciuki_mixer_line actual;
    assert(mixer_record_line(&queued,&actual) && actual.line==CIUKI_MIXER_LINE && actual.selected && actual.left==75);
    job=mixer_pending; recording.line=CIUKI_MIXER_MIC;
    assert(!mixer_pack_line(&job,&recording));
    recording.line=CIUKI_MIXER_LINE; recording.left=recording.right=60; recording.selected=0;
    assert(!mixer_pack_line(&job,&recording));
    assert(mixer_record_line(&job,&actual) && actual.line==CIUKI_MIXER_MIC && actual.left==60 && actual.selected);
    recording.selected=1; assert(!mixer_pack_line(&job,&recording));
    assert(mixer_record_line(&job,&actual) && actual.line==CIUKI_MIXER_LINE && actual.left==60 && actual.selected);
    uint32_t serial=mixer_serial; settings_writer.stopping=1;
    job=mixer_pending; job.master.left=99;
    assert(mixer_submit(&job)==-EIO && mixer_serial==serial && mixer_pending.master.left==40);
    assert(!pthread_cond_destroy(&settings_writer.changed));
    assert(!pthread_mutex_destroy(&settings_writer.lock));
    puts("PASS mixer queue: 64-byte capacity, master/source/recording coalescing, latest values, rejected submission retains accepted state");
    return 0;
}
