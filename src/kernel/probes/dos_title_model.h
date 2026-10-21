/* f3-53 bounded private plan and checkpoint model; also compiled on the host.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_TITLE_MODEL_H
#define CIUKI_DOS_TITLE_MODEL_H
#include <ciuki/kernel.h>
#define D53_INPUTS 128u
#define D53_PLAN_BYTES 1939u
struct d53_checkpoint { uint32_t at, deadline, text; char value[65]; };
struct d53_input { uint32_t phase, at, edge; };
struct d53_plan {
    char title[24], command[128];
    uint32_t audio, irq, count;
    struct d53_checkpoint cp[3];
    struct d53_input input[D53_INPUTS];
};
static inline uint32_t d53_u32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static inline bool d53_string(const uint8_t *p,unsigned n)
{
    bool ended=false;
    for (unsigned i=0;i<n;i++) {
        if (!p[i]) ended=true;
        else if (ended || p[i]<32 || p[i]>126) return false;
    }
    return ended && p[0];
}
static inline bool d53_parse(const uint8_t *p,unsigned size,struct d53_plan *out)
{
    if (!p || !out || size!=D53_PLAN_BYTES || memcmp(p,"D53PLAN1",8) ||
        !d53_string(p+8,24) || !d53_string(p+32,128)) return false;
    struct d53_plan *d=out;
    memset(d,0,sizeof(*d)); memcpy(d->title,p+8,24); memcpy(d->command,p+32,128);
    if (d->title[0]<'a' || d->title[0]>'z') return false;
    for (unsigned i=0;d->title[i];i++) {
        char c=d->title[i];
        if (!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-')) return false;
    }
    if (strncmp(d->command,"\\RUNREC.COM \\",13)) return false;
    for (unsigned i=0;d->command[i];i++) {
        char c=d->command[i];
        if (c=='&' || c=='|' || c=='<' || c=='>' || c=='%' || c=='"') return false;
    }
    d->audio=d53_u32(p+160); d->irq=d53_u32(p+164); d->count=d53_u32(p+168);
    if (d->audio>3 || (d->irq!=5 && d->irq!=7) || !d->count || d->count>D53_INPUTS) return false;
    unsigned at=172;
    for (unsigned i=0;i<3;i++,at+=77) {
        struct d53_checkpoint *c=&d->cp[i];
        c->at=d53_u32(p+at); c->deadline=d53_u32(p+at+4); c->text=d53_u32(p+at+8);
        if (c->text>1 || c->at>c->deadline || c->deadline>60000 ||
            (i && c->at<d->cp[i-1].at) || !d53_string(p+at+12,65)) return false;
        memcpy(c->value,p+at+12,65);
        if (!c->text) {
            if (strlen(c->value)!=64) return false;
            for (unsigned j=0;j<64;j++) {
                char x=c->value[j]; if (!((x>='0' && x<='9') || (x>='a' && x<='f'))) return false;
            }
        }
    }
    bool held[128]={0}, exit=false;
    for (unsigned i=0;i<D53_INPUTS;i++,at+=12) {
        uint32_t phase=d53_u32(p+at), when=d53_u32(p+at+4), edge=d53_u32(p+at+8);
        if (i>=d->count) { if (phase || when || edge) return false; continue; }
        struct d53_input *e=&d->input[i]; e->phase=phase; e->at=when; e->edge=edge;
        if (phase>2 || when>(phase ? 30000u : 60000u) || !(edge&127) || (edge&~383u)) return false;
        if (i && (phase<d->input[i-1].phase || (phase==d->input[i-1].phase && when<d->input[i-1].at))) return false;
        unsigned code=edge&127; bool down=(edge&256)!=0;
        if (held[code]==down) return false;
        held[code]=down; exit|=phase==2;
    }
    for (unsigned i=0;i<128;i++) if (held[i]) return false;
    return exit;
}
static inline bool d53_match(const struct d53_checkpoint *cp,const char *digest,const uint16_t *text)
{
    if (!cp->text) return !strncmp(cp->value,digest,65);
    if (!text) return false;
    unsigned n=strlen(cp->value);
    for (unsigned row=0;row<25;row++) for (unsigned col=0;col+n<=80;col++) {
        unsigned j=0;
        while (j<n && (uint8_t)text[row*80+col+j]==(uint8_t)cp->value[j]) j++;
        if (j==n) return true;
    }
    return false;
}
struct d53_audio {
    uint64_t underruns, pcm_frames, opl_writes;
    uint32_t blocks, late_acks, irq_latency_max_us, irq_entries, irq;
    bool attached, ready;
};
static inline bool d53_audio_ok(const struct d53_plan *p,const struct d53_audio *a)
{
    if (a->underruns || a->irq_latency_max_us>20000) return false;
    if (p->audio && (!a->attached || !a->ready || a->irq!=p->irq)) return false;
    if ((p->audio&1) && (!a->pcm_frames || !a->blocks || !a->irq_entries)) return false;
    return !(p->audio&2) || a->opl_writes;
}
#endif
