/* Original bounded implementation of USB HID 1.11 sections 6.2.2 and 8.
 * Usage constants: USB-IF HUT 1.12. No upstream implementation copied.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/hid.h>
#include <ciuki/kernel.h>

struct globals {
    uint32_t page, size, count, id, unit;
    int32_t minimum, maximum, physical_minimum, physical_maximum, exponent;
    bool physical_low, physical_high;
};
static uint32_t usage(uint32_t page, uint32_t value, unsigned bytes)
{ return bytes==4 ? value : (page<<16)|value; }
static uint32_t class(uint32_t app)
{
    switch (app) {
    case 0x10006: return CIUKI_HID_KEYBOARD;
    case 0x10002: return CIUKI_HID_MOUSE;
    case 0x10004: case 0x10005: return CIUKI_HID_JOYSTICK;
    case 0xc0001: return CIUKI_HID_CONSUMER;
    default: return 0;
    }
}
unsigned hid_consumer_action(uint32_t u)
{
    switch (u) {
    case 0xc00e9: return CIUKI_CONSUMER_VOLUME_UP;
    case 0xc00ea: return CIUKI_CONSUMER_VOLUME_DOWN;
    case 0xc00e2: return CIUKI_CONSUMER_MUTE;
    case 0xc00cd: return CIUKI_CONSUMER_PLAY_PAUSE;
    case 0xc00b5: return CIUKI_CONSUMER_NEXT;
    case 0xc00b6: return CIUKI_CONSUMER_PREVIOUS;
    default: return 0;
    }
}
unsigned hid_axis_index(uint32_t u)
{ return u>=0x10030 && u<=0x10038 ? u-0x10030 : CIUKI_HID_AXES; }
uint32_t hid_usage_at(const struct hid_layout *l, const struct hid_field *f, unsigned at)
{
    uint32_t last=0;
    for (unsigned i=0;i<f->usages;i++) {
        const struct hid_usage *u=&l->usage[f->first_usage+i];
        uint32_t n=u->high-u->low;
        if (at<=n) return u->low+at;
        at-=n+1; last=u->high;
    }
    return last; /* HID 6.2.2.8: repeat the last usage for excess variables. */
}
static bool range_fits(const struct globals *g)
{
    if (g->maximum<g->minimum) return false;
    if (g->size==32) return true;
    if (g->minimum<0)
        return g->minimum>=-(INT32_C(1)<<(g->size-1)) &&
               g->maximum<(INT32_C(1)<<(g->size-1));
    return (uint32_t)g->maximum<(UINT32_C(1)<<g->size);
}
bool hid_parse(struct hid_layout *out, const uint8_t *b, unsigned len)
{
    if (!out || !b || !len || len>HID_DESCRIPTOR_MAX) return false;
    struct hid_layout l={0};
    struct globals g={0}, stack[4]; unsigned sp=0, depth=0, joysticks=0;
    uint32_t apps[8], low=0;
    struct hid_usage local[HID_USAGE_MAX]; unsigned used=0;
    bool pending=false, unnumbered=false;
    for (unsigned at=0;at<len;) {
        unsigned item=b[at++], n=item&3; if (n==3) n=4;
        if (item==0xfe || n>len-at || (item&12)==12) return false;
        uint32_t v=0;
        for (unsigned i=0;i<n;i++) v|=(uint32_t)b[at+i]<<(8*i);
        int32_t s=n==1 ? (int8_t)v : n==2 ? (int16_t)v : (int32_t)v;
        at+=n;
        unsigned tag=item&0xfc, type=item&12;
        if (type==4) {
            if (tag!=0xa4 && tag!=0xb4 && !n) return false;
            switch (tag) {
            case 0x04: if (v>65535) return false; g.page=v; break;
            case 0x14: g.minimum=s; break;
            case 0x24:
                if (g.minimum>=0 && v>INT32_MAX) return false;
                g.maximum=g.minimum<0 ? s : (int32_t)v; break;
            case 0x34: g.physical_minimum=s; g.physical_low=true; break;
            case 0x44:
                if (g.physical_minimum>=0 && v>INT32_MAX) return false;
                g.physical_maximum=g.physical_minimum<0 ? s : (int32_t)v;
                g.physical_high=true; break;
            case 0x54: g.exponent=v<=15 ? (int32_t)(v<8 ? v : v-16) : s; break;
            case 0x64: g.unit=v; break;
            case 0x74: g.size=v; break;
            case 0x84: if (!v || v>255 || n!=1) return false; g.id=v; l.numbered=true; break;
            case 0x94: g.count=v; break;
            case 0xa4: if (n || sp==4) return false; stack[sp++]=g; break;
            case 0xb4: if (n || !sp) return false; g=stack[--sp]; break;
            default: return false;
            }
        } else if (type==8) {
            if (!n) return false;
            switch (tag) {
            case 0x08:
                if (pending || used==HID_USAGE_MAX) return false;
                local[used++]=(struct hid_usage){usage(g.page,v,n),usage(g.page,v,n)}; break;
            case 0x18: if (pending) return false; low=usage(g.page,v,n); pending=true; break;
            case 0x28: {
                uint32_t high=usage(g.page,v,n);
                if (!pending || high<low || (high>>16)!=(low>>16) || used==HID_USAGE_MAX) return false;
                local[used++]=(struct hid_usage){low,high}; pending=false; break;
            }
            /* Designator/String indices describe labels, not data layout. */
            case 0x38: case 0x48: case 0x58: case 0x78: case 0x88: case 0x98: break;
            default: return false; /* Includes alternative usage sets. */
            }
        } else {
            if (pending) return false;
            if (tag==0xa0) {
                if (n!=1 || depth==8 || !used) return false;
                uint32_t app=depth ? apps[depth-1] : 0;
                if (v==1) {
                    app=class(local[0].low) ? local[0].low : 0;
                    if (class(app)==CIUKI_HID_JOYSTICK && ++joysticks>1) return false;
                } else if (!depth || v>=7 || (local[0].low>>16)>=0xff00) app=0;
                apps[depth++]=app;
            } else if (tag==0xc0) {
                if (n || !depth) return false;
                depth--;
            } else if (tag==0x80 || tag==0x90 || tag==0xb0) {
                if (!n || !depth || !g.size || g.size>32 || !g.count || g.count>256) return false;
                unsigned kind=tag==0x80 ? HID_INPUT : tag==0x90 ? HID_OUTPUT : HID_FEATURE;
                unsigned r=0; while (r<l.reports && l.report[r].id!=g.id) r++;
                if (r==l.reports) {
                    if (r==HID_REPORT_MAX) return false;
                    l.report[l.reports++].id=(uint8_t)g.id;
                }
                unsigned limit=(HID_PACKET_MAX-(g.id ? 1u : 0))*8;
                unsigned bits=g.size*g.count, offset=l.report[r].bits[kind];
                if (bits>limit-offset) return false;
                if (!g.id) unnumbered=true;
                l.report[r].bits[kind]=(uint16_t)(offset+bits);
                if (kind==HID_OUTPUT && !(v&1) && (!apps[depth-1] || !used)) l.report[r].unsafe_output=1;
                if (!(v&1) && apps[depth-1] && used) {
                    if (v&0x100 || !range_fits(&g) ||
                        (g.physical_low && g.physical_high && g.physical_maximum<g.physical_minimum) ||
                        l.fields==HID_FIELD_MAX || used>HID_USAGE_MAX-l.usages) return false;
                    struct hid_field *f=&l.field[l.fields++];
                    *f=(struct hid_field){.minimum=g.minimum,.maximum=g.maximum,
                        .physical_minimum=g.physical_low && g.physical_high && (g.physical_minimum || g.physical_maximum) ? g.physical_minimum : g.minimum,
                        .physical_maximum=g.physical_low && g.physical_high && (g.physical_minimum || g.physical_maximum) ? g.physical_maximum : g.maximum,
                        .unit=g.unit,.unit_exponent=g.exponent,.application=apps[depth-1],
                        .bit=offset,.count=g.count,.flags=v,.size=g.size,.report=r,.kind=kind,
                        .first_usage=l.usages,.usages=used,.hat=255};
                    memcpy(l.usage+l.usages,local,used*sizeof(local[0])); l.usages+=used;
                    if (kind==HID_INPUT) {
                        l.classes|=class(f->application);
                        for (unsigned i=0;i<g.count;i++) {
                            uint32_t u=hid_usage_at(&l,f,i);
                            if (hid_consumer_action(u)) l.classes|=CIUKI_HID_CONSUMER;
                            if (class(f->application)==CIUKI_HID_JOYSTICK && u==0x10039) {
                                if (!(v&2) || (v&4) || ((int64_t)g.maximum-g.minimum!=3 && (int64_t)g.maximum-g.minimum!=7) || l.hats==CIUKI_HID_HATS) return false;
                                if (f->hat==255) f->hat=l.hats;
                                l.hats++;
                            }
                        }
                    }
                }
            } else return false;
            used=0;
        }
    }
    if (depth || sp || used || pending || !l.reports || !l.classes || (l.numbered && unnumbered)) return false;
    *out=l; return true;
}
int32_t hid_value(const uint8_t *b, unsigned bit, unsigned size, bool sign)
{
    uint32_t v=0;
    for (unsigned i=0;i<size;i++) if (b[(bit+i)/8]&(1u<<((bit+i)%8))) v|=UINT32_C(1)<<i;
    if (sign && size<32 && (v&(UINT32_C(1)<<(size-1)))) v|=UINT32_MAX<<size;
    return (int32_t)v;
}
int hid_decode(const struct hid_layout *l, const uint8_t *b, unsigned len, struct hid_values *out)
{
    if (!l || !b || !out || !len || len>HID_PACKET_MAX) return -EINVAL;
    unsigned id=l->numbered ? b[0] : 0, r=0;
    while (r<l->reports && l->report[r].id!=id) r++;
    if (r==l->reports || !l->report[r].bits[HID_INPUT]) return 0;
    if (len<(l->report[r].bits[HID_INPUT]+7u)/8+(l->numbered ? 1u : 0)) return -HID_EPROTO;
    b+=l->numbered ? 1 : 0;
    struct hid_values v={0};
    for (unsigned h=0;h<CIUKI_HID_HATS;h++) v.hat[h]=-1;
    for (unsigned j=0;j<l->fields;j++) {
        const struct hid_field *f=&l->field[j];
        if (f->report!=r || f->kind!=HID_INPUT) continue;
        v.classes|=class(f->application);
        unsigned hat=f->hat;
        for (unsigned i=0;i<f->count;i++) {
            int32_t value=hid_value(b,f->bit+i*f->size,f->size,f->minimum<0);
            uint32_t u=hid_usage_at(l,f,i);
            bool valid=value>=f->minimum && value<=f->maximum;
            if (!(f->flags&2)) {
                if (!valid) { if (f->flags&64) continue; return -HID_EPROTO; }
                unsigned selector=(unsigned)((int64_t)value-f->minimum), usages=0;
                for (unsigned s=0;s<f->usages;s++) usages+=l->usage[f->first_usage+s].high-l->usage[f->first_usage+s].low+1;
                if (selector>=usages) { if (f->flags&64) continue; return -HID_EPROTO; }
                u=hid_usage_at(l,f,selector);
            }
            if ((u>>16)==7 && class(f->application)==CIUKI_HID_KEYBOARD) {
                if (f->flags&4 || (u&65535)>255) return -HID_EPROTO;
                if (!valid) { if (f->flags&64) continue; return -HID_EPROTO; }
                if (!(f->flags&2) || value) {
                    unsigned key=u&65535;
                    if (key && key<=3) return 2; /* retain last complete state */
                    if (key) v.keyboard[key/8]|=1u<<(key%8);
                }
            } else if ((u>>16)==12) {
                unsigned action=hid_consumer_action(u);
                if (!action) continue;
                if (f->flags&4) return -HID_EPROTO;
                if (!valid) { if (f->flags&64) continue; return -HID_EPROTO; }
                v.classes|=CIUKI_HID_CONSUMER;
                if (action && (!(f->flags&2) || value)) v.consumers|=1u<<(action-1);
            } else if (class(f->application)==CIUKI_HID_JOYSTICK) {
                if (f->flags&4) return -HID_EPROTO;
                unsigned axis=hid_axis_index(u);
                if (u==0x10039 && (f->flags&2)) {
                    if (hat>=CIUKI_HID_HATS) return -HID_EPROTO;
                    if (!valid && !(f->flags&64)) return -HID_EPROTO;
                    v.hats|=1u<<hat;
                    v.hat[hat++]=valid ? (value-f->minimum)*(f->maximum-f->minimum==3 ? 2 : 1) : -1;
                } else {
                    if (!valid) { if (f->flags&64) continue; return -HID_EPROTO; }
                    if (axis<CIUKI_HID_AXES && (f->flags&2)) { v.axes|=1u<<axis; v.axis[axis]=value; }
                    if ((u>>16)==9 && (u&65535)>=1 && (u&65535)<=32 && (!(f->flags&2) || value)) v.buttons|=1u<<((u&65535)-1);
                }
            }
        }
    }
    *out=v; return 1;
}
