/* Device-dependent mixer lines. SPDX-License-Identifier: GPL-2.0-only
 * AC97 register semantics: Linux v6.12 include/sound/ac97/regs.h.
 * The caller holds audio.control; codec transactions use register_lock. */
#include <ciuki/kernel.h>
#include <ciuki/audio_backend.h>
static void line_defaults(void)
{
    if (audio.lines_initialized) return;
    for (unsigned i=1;i<=5;i++) audio.lines[i]=(struct ciuki_mixer_line){
        .version=1,.size=sizeof(struct ciuki_mixer_line),.line=i,.left=100,.right=100};
    audio.record_gain=100; audio.record_source=CIUKI_CAPTURE_MIC;
    audio.lines_initialized=true;
}
int audio_line_control(struct ciuki_mixer_line *r, bool set)
{
    if (!r || r->line<CIUKI_MIXER_WAVE || r->line>CIUKI_MIXER_MIC || r->recording>1 ||
        (r->recording && r->line==CIUKI_MIXER_MIDI)) return -EINVAL;
    if (set && (r->left>100 || r->right>100 || r->mute>1 || r->selected>1 || (!r->recording && r->selected) || r->available)) return -EINVAL;
    line_defaults();
    unsigned line=r->line, rec=r->recording;
    struct audio_dma_device *d=&audio_dma;
    bool codec=audio.ready && !audio.error && d->codec_read && d->codec_write && !d->state.quarantine;
    bool available=rec ? d->capture_ops && audio.ready && !audio.error && !d->state.quarantine :
        line<=CIUKI_MIXER_CD ? audio.ready && !audio.error : codec;
    if (set && !available) return -EOPNOTSUPP;
    if (rec) {
        unsigned source=line==CIUKI_MIXER_MIC ? CIUKI_CAPTURE_MIC :
            line==CIUKI_MIXER_LINE ? CIUKI_CAPTURE_LINE : line==CIUKI_MIXER_CD ? CIUKI_CAPTURE_CD : CIUKI_CAPTURE_MIX;
        if (set) {
            /* Source/gain changes affect an active copied recording too. */
            if (r->selected && d->capture.generation) {
                int e=audio_capture_source(d,d->capture.generation,source); if (e) return e;
            }
            if (r->selected) audio.record_source=source;
            audio.record_gain=r->mute ? 0 : (r->left+r->right)/2;
        }
        *r=(struct ciuki_mixer_line){.version=1,.size=sizeof(*r),.line=line,.recording=1,
            .left=audio.record_gain,.right=audio.record_gain,
            .selected=audio.record_source==source,.available=available};
        return 0;
    }
    /* Digital CD and analog CD share the same visible column. PCM/MIDI
     * attenuation is applied before summation; analog inputs use codec regs. */
    if (line>=CIUKI_MIXER_CD && codec) {
        static const uint16_t regs[]={0,0,0,0x12,0x10,0x0e};
        uint16_t old; kmutex_lock(&d->register_lock);
        int e=d->codec_read(d,regs[line],&old);
        if (!e && set) {
            unsigned bit=1u<<(line-CIUKI_MIXER_CD);
            if (!(d->mixer_line_mask & bit)) {
                d->mixer_line_saved[line-CIUKI_MIXER_CD]=old;
                d->mixer_line_mask |= bit; /* save before a possibly posted write */
            }
            unsigned l=(100-r->left)*31/100, right=(100-r->right)*31/100;
            const struct ac97_codec_profile *profile=d->codec.profile ? d->codec.profile : ac97_codec_lookup(d->state.codec_id);
            uint16_t mask=line==CIUKI_MIXER_MIC ? 0x801f :
                (uint16_t)(0x9f1f | (profile->master_mask & 0x80));
            uint16_t value=(old & ~mask) | (r->mute ? (mask & 0x8080) : 0) | right;
            if (line!=CIUKI_MIXER_MIC) value |= l<<8;
            e=d->codec_write(d,regs[line],value);
        }
        kmutex_unlock(&d->register_lock);
        if (e) return e;
        if (!set && line>CIUKI_MIXER_CD) {
            audio.lines[line].left=100-((old>>8)&31)*100/31;
            audio.lines[line].right=100-(old&31)*100/31;
            if (line==CIUKI_MIXER_MIC) audio.lines[line].left=audio.lines[line].right;
            audio.lines[line].mute=!!(old&0x8000);
        }
    }
    if (set) audio.lines[line]=*r;
    *r=audio.lines[line]; r->available=available;
    return 0;
}

int audio_lines_restore(void)
{
    struct audio_dma_device *d=&audio_dma;
    if (!d->mixer_line_mask) return 0;
    if (!d->codec_write || d->state.quarantine) return -EIO;
    static const uint16_t regs[]={0x12,0x10,0x0e};
    kmutex_lock(&d->register_lock); int e=0;
    for (unsigned i=3;i>0 && !e;i--) if (d->mixer_line_mask & (1u<<(i-1))) {
        e=d->codec_write(d,regs[i-1],d->mixer_line_saved[i-1]);
        if (!e) d->mixer_line_mask &= ~(1u<<(i-1));
    }
    kmutex_unlock(&d->register_lock); return e;
}

#include <ciuki/registry_value.h>
void audio_lines_load(struct vfs *v)
{
    line_defaults();
    if (!v || !kernel_registry_int) return;
    for (unsigned i=1;i<=6;i++) {
        char key[16]="Source0";
        if (i==6) memcpy(key,"Recording",10); else key[6]=(char)('0'+i);
        int32_t packed;
        if (!kernel_registry_int(v,"System\\Audio",key,&packed)) continue;
        uint32_t value=(uint32_t)packed;
        unsigned line=i==6 ? value>>24 : i;
        if ((value & 0x00fe0000u) || (i<6 && (value>>24)) || line<1 || line>5 ||
            (i==6 && line==CIUKI_MIXER_MIDI) || (value&255)>100 || ((value>>8)&255)>100) continue;
        struct ciuki_mixer_line r={.version=1,.size=sizeof(r),.line=line,.recording=i==6,
            .left=value&255,.right=(value>>8)&255,.mute=(value>>16)&1,.selected=i==6};
        int e=audio_line_control(&r,true);
        if (e && e!=-EOPNOTSUPP) klog("[audio] source_preference=%u error=%d",i,e);
    }
}
