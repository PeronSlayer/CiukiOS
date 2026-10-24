/* Bounded MIDI byte parser and 18-voice FM mapper. Original compact patches,
 * not a General MIDI wavetable. MIDI message semantics: MIDI Association;
 * OPL register renderer: the existing DBOPL port. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MIDI_FM_H
#define CIUKI_MIDI_FM_H
#include <stdint.h>
#include <stdbool.h>
struct midi_fm_channel { uint16_t bend; uint8_t program, volume, expression, pan, sustain; };
struct midi_fm_voice { uint32_t age; uint8_t channel, note, velocity, key, held; };
struct midi_fm {
    struct midi_fm_channel channel[16];
    struct midi_fm_voice voice[18];
    uint32_t age;
    uint8_t status, count, data[2];
    bool sysex;
};
typedef void (*midi_fm_output)(void *,unsigned,uint8_t);
static inline unsigned midi_fm_op(unsigned v)
{ static const uint8_t op[9]={0,1,2,8,9,10,16,17,18}; return (v/9)*256+op[v%9]; }
static inline unsigned midi_fm_ch(unsigned v) { return (v/9)*256+v%9; }
static inline void midi_fm_pitch(struct midi_fm *m,unsigned v,midi_fm_output out,void *ctx)
{
    static const uint16_t f[13]={345,365,387,410,434,460,488,517,548,580,615,652,690};
    struct midi_fm_voice *s=&m->voice[v];
    int pitch=(int)s->note*1024+((int)m->channel[s->channel].bend-8192)/4;
    if (pitch<0) pitch=0;
    if (pitch>127*1024) pitch=127*1024;
    unsigned note=(unsigned)pitch/1024, semitone=note%12, frac=(unsigned)pitch%1024;
    unsigned freq=f[semitone]+(f[semitone+1]-f[semitone])*frac/1024;
    int block=(int)(note/12)-1;
    if (block<0) { freq>>=(unsigned)-block; block=0; }
    while (block>7) { freq*=2; block--; }
    if (freq>1023) freq=1023;
    unsigned reg=midi_fm_ch(v);
    out(ctx,0xa0+reg,(uint8_t)freq);
    out(ctx,0xb0+reg,(uint8_t)((freq>>8)|((unsigned)block<<2)|(s->key ? 32 : 0)));
}
static inline void midi_fm_level(struct midi_fm *m,unsigned v,midi_fm_output out,void *ctx)
{
    struct midi_fm_voice *s=&m->voice[v]; struct midi_fm_channel *c=&m->channel[s->channel];
    unsigned level=(unsigned)s->velocity*c->volume*c->expression/(127*127);
    out(ctx,0x43+midi_fm_op(v),(uint8_t)(63-level*63/127));
    out(ctx,0xc0+midi_fm_ch(v),(uint8_t)((c->pan<96 ? 16 : 0)|(c->pan>31 ? 32 : 0)|6));
}
static inline void midi_fm_reset(struct midi_fm *m,midi_fm_output out,void *ctx)
{
    *m=(struct midi_fm){0};
    for (unsigned c=0;c<16;c++) m->channel[c]=(struct midi_fm_channel){8192,0,100,127,64,0};
    out(ctx,0x105,1); out(ctx,0x104,0); out(ctx,0xbd,0);
    for (unsigned v=0;v<18;v++) {
        out(ctx,0x80+midi_fm_op(v),0xff); out(ctx,0x83+midi_fm_op(v),0xff);
        out(ctx,0xb0+midi_fm_ch(v),0); out(ctx,0x43+midi_fm_op(v),63);
    }
}
static inline void midi_fm_message(struct midi_fm *m,midi_fm_output out,void *ctx)
{
    unsigned ch=m->status&15, type=m->status>>4, a=m->data[0], b=m->data[1];
    struct midi_fm_channel *c=&m->channel[ch];
    if (type==8 || (type==9 && !b)) {
        for (unsigned v=0;v<18;v++) if (m->voice[v].key && m->voice[v].channel==ch && m->voice[v].note==a) {
            if (c->sustain) m->voice[v].held=1;
            else { m->voice[v].key=0; midi_fm_pitch(m,v,out,ctx); }
        }
    } else if (type==9) {
        unsigned v=0;
        for (unsigned k=0;k<18;k++) {
            if (!m->voice[k].key) { v=k; break; }
            if (m->voice[k].age<m->voice[v].age) v=k;
        }
        out(ctx,0xb0+midi_fm_ch(v),0);
        m->voice[v]=(struct midi_fm_voice){++m->age,(uint8_t)ch,(uint8_t)a,(uint8_t)b,1,0};
        unsigned op=midi_fm_op(v), family=c->program/8;
        /* Sixteen original two-operator colours. Channel 10 uses a short,
         * percussive envelope. No external instrument bank is imported. */
        static const uint8_t colour[16]={1,2,3,1,2,1,4,2,3,1,5,2,4,3,6,1};
        out(ctx,0x20+op,(uint8_t)(colour[family]|0x20)); out(ctx,0x23+op,0x21);
        out(ctx,0x40+op,(uint8_t)(24+family));
        out(ctx,0x60+op,0xf2); out(ctx,0x63+op,ch==9 ? 0xf8 : 0xf2);
        out(ctx,0x80+op,0x73); out(ctx,0x83+op,ch==9 ? 0x48 : 0x74);
        out(ctx,0xe0+op,(uint8_t)(family%4)); out(ctx,0xe3+op,0);
        midi_fm_level(m,v,out,ctx); midi_fm_pitch(m,v,out,ctx);
    } else if (type==12) c->program=(uint8_t)a;
    else if (type==14) {
        c->bend=(uint16_t)(a|(b<<7));
        for (unsigned v=0;v<18;v++) if (m->voice[v].key && m->voice[v].channel==ch) midi_fm_pitch(m,v,out,ctx);
    } else if (type==11) {
        if (a==7) c->volume=(uint8_t)b;
        if (a==10) c->pan=(uint8_t)b;
        if (a==11) c->expression=(uint8_t)b;
        if (a==64) c->sustain=b>=64;
        if (a==121) { c->volume=100; c->expression=127; c->pan=64; c->bend=8192; c->sustain=0; }
        for (unsigned v=0;v<18;v++) if (m->voice[v].channel==ch) {
            struct midi_fm_voice *s=&m->voice[v];
            if (a==120 || a>=123 || (!c->sustain && s->held)) {
                if (a!=120 && a>=123 && c->sustain) s->held=1;
                else { s->key=s->held=0; midi_fm_pitch(m,v,out,ctx); }
            }
            if (a==120) { s->velocity=0; out(ctx,0x80+midi_fm_op(v),0xff); out(ctx,0x83+midi_fm_op(v),0xff); }
            midi_fm_level(m,v,out,ctx);
            if (a==121) midi_fm_pitch(m,v,out,ctx);
        }
    }
}
static inline void midi_fm_byte(struct midi_fm *m,uint8_t b,midi_fm_output out,void *ctx)
{
    if (b>=0xf8) { if (b==0xff) midi_fm_reset(m,out,ctx); return; }
    if (b&128) {
        m->count=0;
        if (b>=0xf0) { m->status=0; m->sysex=b==0xf0; return; }
        m->sysex=false; m->status=b; return;
    }
    if (!m->status || m->sysex) return;
    m->data[m->count++]=b;
    unsigned need=(m->status>>4)==12 || (m->status>>4)==13 ? 1 : 2;
    if (m->count==need) { if (need==1) m->data[1]=0; midi_fm_message(m,out,ctx); m->count=0; }
}
#endif
