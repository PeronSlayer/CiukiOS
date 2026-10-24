/* Private desktop mixer queue, shared with its host harness. SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKTOP_MIXER_JOB_H
#define CIUKI_DESKTOP_MIXER_JOB_H
/* One packed word per source keeps the complete coalescing snapshot within
 * the existing worker's 64-byte slot. Bits 0/7: levels, 14: mute, 15: Select.
 * Bit 0 of mask is the master; 1..5 playback, 6..10 recording. */
struct mixer_job {
    uint32_t serial;
    int fd;
    struct ciuki_mixer_master master;
    uint16_t mask;
    uint16_t values[10];
    uint16_t save_only;
};
_Static_assert(sizeof(struct mixer_job)<=sizeof(((struct cfg_writer *)0)->job),"mixer job fits worker slot");
static struct mixer_job mixer_pending;
static int mixer_submit(struct mixer_job *job)
{
    job->serial=mixer_serial+1; if (!job->serial) job->serial=1;
    if (cfg_writer_job(&settings_writer,job,sizeof(*job))) return -errno;
    mixer_serial=job->serial; mixer_pending=*job; return 0;
}
static int mixer_pack_line(struct mixer_job *job,const struct ciuki_mixer_line *line)
{
    if (line->line<1 || line->line>5 || line->recording>1 || line->left>100 || line->right>100 || line->mute>1 || line->selected>1 ||
        (line->recording && line->line==CIUKI_MIXER_MIDI)) return -EINVAL;
    unsigned i=line->line+line->recording*5;
    if (line->recording) {
        /* Recording has one mux and shared gain. The unused MIDI-input word
         * carries the newest gain's line and the last pending selection. */
        if (!(job->mask & 0x7c0)) job->values[6]=0;
        unsigned selected=job->values[6]&7;
        if (line->selected) selected=line->line;
        job->values[6]=(uint16_t)(selected | line->line<<3);
    }
    job->mask|=(uint16_t)(1u<<i);
    job->values[i-1]=(uint16_t)(line->left | line->right<<7 | line->mute<<14 | line->selected<<15);
    if (!(job->mask & 1)) job->save_only=0;
    return 0;
}
static int mixer_record_line(const struct mixer_job *job,struct ciuki_mixer_line *line)
{
    if (!(job->mask & 0x7c0)) return 0;
    unsigned selected=job->values[6]&7, latest=(job->values[6]>>3)&7;
    if (!latest || latest>5 || latest==CIUKI_MIXER_MIDI) return 0;
    uint16_t v=job->values[latest+4];
    *line=(struct ciuki_mixer_line){.version=1,.size=sizeof(*line),.line=selected ? selected : latest,
        .recording=1,.left=v&127,.right=(v>>7)&127,.mute=(v>>14)&1,.selected=selected!=0};
    return 1;
}
#endif
