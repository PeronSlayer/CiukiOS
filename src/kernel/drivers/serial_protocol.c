/* Mouse wire equations adapted from Linux v6.12
 * drivers/input/mouse/sermouse.c, blob 218c8432a13bc9addfe54a8818ee3701f7dd6795.
 * Copyright (c) 1999-2001 Vojtech Pavlik.
 * Ciuki changes: whole packets, bounded resynchronization, no prediction,
 * no guessed middle button, no Linux framework. PnP parser is original,
 * against Microsoft/Hayes External COM 1.00, sections 2 and 3.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <ciuki/serial_protocol.h>
#include <ciuki/kernel.h>

static int hex(unsigned c)
{ return c>='0' && c<='9' ? (int)c-'0' : c>='A' && c<='F' ? (int)c-'A'+10 : -1; }
int serial_pnp_parse(const uint8_t *wire, unsigned n, struct serial_pnp_id *out)
{
    if (!wire || !out || n>256) return -EINVAL;
    unsigned start=0;
    while (start<n && wire[start]!=8 && wire[start]!=0x28) {
        if (wire[start]==9 || wire[start]==0x29 || start==16) return -EINVAL;
        start++;
    }
    if (n-start<11) return -EINVAL;
    wire+=start; n-=start;
    unsigned offset=wire[0]==8 ? 0x20 : 0;
    if (wire[n-1]+offset!=0x29 || wire[1]>0x3f || wire[2]>0x3f) return -EINVAL;
    char b[257];
    for (unsigned i=3;i<n;i++) {
        unsigned c=wire[i]+offset;
        if ((c<0x20 && c!=10 && c!=13) || c>0x7e || (offset && wire[i]>0x3f)) return -EINVAL;
        b[i]=(char)c;
        if (i<n-1 && (c=='(' || c==')')) return -EINVAL;
    }
    for (unsigned i=3;i<6;i++) if (b[i]<'A' || b[i]>'Z') return -EINVAL;
    for (unsigned i=6;i<10;i++) if (hex((unsigned)b[i])<0) return -EINVAL;
    struct serial_pnp_id id={0};
    id.revision=(uint16_t)((wire[1]<<6)|wire[2]); memcpy(id.hid,b+3,7);
    if (n>11) {
        if (n<14 || b[10]!='\\') return -EINVAL;
        int hi=hex((unsigned)b[n-3]), lo=hex((unsigned)b[n-2]);
        if (hi<0 || lo<0) return -EINVAL;
        unsigned sum=wire[n-1];
        for (unsigned i=0;i<n-3;i++) sum+=wire[i];
        if ((sum&255)!=(unsigned)(hi*16+lo)) return -EINVAL;
        char *fields[]={id.serial,id.class_name,id.compatible,id.name};
        const unsigned limits[]={8,32,40,40}; unsigned pos=10, field=0;
        while (pos<n-3) {
            if (field==4 || b[pos++]!='\\') return -EINVAL;
            unsigned begin=pos;
            while (pos<n-3 && b[pos]!='\\') pos++;
            unsigned size=pos-begin;
            if (size>limits[field] || (field==0 && size && size!=8)) return -EINVAL;
            if (field!=3) for (unsigned i=begin;i<pos;i++) if ((unsigned char)b[i]<32) return -EINVAL;
            memcpy(fields[field],b+begin,size); field++;
        }
    }
    *out=id; return 0;
}
static unsigned mouse_id(const char *s, unsigned n)
{
    if (n!=7) return 0;
    if (!memcmp(s,"MSH0001",7)) return CIUKI_SERIAL_MOUSE_WHEEL;
    if (!memcmp(s,"LGI8001",7) || !memcmp(s,"PNP0F08",7) || !memcmp(s,"PNP0F17",7))
        return CIUKI_SERIAL_MOUSE_LOGITECH;
    if (!memcmp(s,"PNP0F04",7) || !memcmp(s,"PNP0F05",7)) return CIUKI_SERIAL_MOUSE_SYSTEMS;
    const char *ids[]={"PNP0F01","PNP0F09","PNP0F0A","PNP0F0B","PNP0F0C","PNP0F0F"};
    for (unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++) if (!memcmp(s,ids[i],7)) return CIUKI_SERIAL_MOUSE_MICROSOFT;
    return 0;
}
unsigned serial_pnp_mouse(const struct serial_pnp_id *id)
{
    if (id->class_name[0] && strncmp(id->class_name,"MOUSE",6)) return 0;
    unsigned p=mouse_id(id->hid,7); if (p) return p;
    const char *s=id->compatible;
    while (*s) { const char *end=s; while (*end && *end!=',') end++;
        p=mouse_id(s,(unsigned)(end-s)); if (p) return p;
        s=*end ? end+1 : end;
    }
    return 0;
}
static int pause_for(const struct serial_pnp_ops *o, unsigned ms)
{
    uint64_t end=o->now(o->context)+ms;
    for (unsigned n=0;n<ms+100;n++) {
        uint64_t now=o->now(o->context);
        if (now>=end) return now-end>35 ? -ETIMEDOUT : 0;
        o->pause(o->context);
    }
    return -ETIMEDOUT;
}
int serial_pnp_enumerate(const struct serial_pnp_ops *o, struct serial_pnp_id *id)
{
    if (!o || !id || !o->modem || !o->setup || !o->dsr || !o->receive || !o->pause || !o->now) return -EINVAL;
    void *c=o->context; int e=o->modem(c,1);
    if (e || (e=pause_for(o,200))) return e;
    e=o->dsr(c); if (e<=0) return e ? e : -ENODEV;
    if ((e=o->setup(c))) return e;
    for (unsigned phase=0;phase<2;phase++) {
        if ((e=o->modem(c,0)) || (e=pause_for(o,200)) || (e=o->modem(c,1))) return e;
        if (!phase && (e=pause_for(o,200))) return e;
        if ((e=o->modem(c,3))) return e;
        uint8_t wire[256]; unsigned n=0, begin=256;
        uint64_t response=o->now(c), first=response, last=response;
        for (unsigned polls=0;polls<3000;polls++) {
            e=o->dsr(c); if (e<=0) return e ? e : -ENODEV;
            uint8_t b; e=o->receive(c,&b);
            uint64_t now=o->now(c);
            if ((n && now-first>=2200) || now-last>=200 || (begin==256 && now-response>=200)) break;
            if (e==1) {
                if (n==sizeof(wire)) return -EOVERFLOW;
                if (!n) first=now;
                wire[n++]=b; last=now;
                if (begin==256 && (b==8 || b==0x28)) begin=n-1;
                else if (begin!=256 && b==wire[begin]+1) return serial_pnp_parse(wire,n,id);
            } else if (e!=-EAGAIN && e!=0) return e;
            o->pause(c);
        }
        if (n) return -EINVAL; /* malformed response is never a legacy mouse guess */
    }
    return -ETIMEDOUT;
}
static int signed8(unsigned v) { return v&128 ? (int)v-256 : (int)v; }
bool serial_mouse_byte(struct serial_mouse_decoder *d, uint8_t b, bool error,
                       uint64_t ms, struct serial_mouse_packet *p)
{
    if (!d || !p || !d->protocol || d->protocol>CIUKI_SERIAL_MOUSE_WHEEL) return false;
    if (error || ms-d->last>100) d->count=0;
    d->last=ms; if (error) return false;
    *p=(struct serial_mouse_packet){0};
    if (d->protocol==CIUKI_SERIAL_MOUSE_SYSTEMS) {
        if (!d->count && (b&0xf8)!=0x80) return false;
        d->bytes[d->count++]=b;
        if (d->count!=5) return false;
        d->count=0;
        p->buttons=(!(d->bytes[0]&4)) | ((!(d->bytes[0]&1))<<1) | ((!(d->bytes[0]&2))<<2);
        p->dx=signed8(d->bytes[1])+signed8(d->bytes[3]);
        p->dy=-signed8(d->bytes[2])-signed8(d->bytes[4]);
    } else {
        if (b&0x80) { d->count=0; return false; }
        if (b&0x40) d->count=0;
        else if (!d->count) return false;
        if (d->count>=4) { d->count=0; return false; }
        d->bytes[d->count++]=b;
        if (d->count==3) {
            if (d->protocol==CIUKI_SERIAL_MOUSE_WHEEL) return false;
            p->dx=signed8(((d->bytes[0]<<6)&0xc0)|d->bytes[1]);
            p->dy=signed8(((d->bytes[0]<<4)&0xc0)|d->bytes[2]);
            p->buttons=((d->bytes[0]>>5)&1)|((d->bytes[0]>>3)&2)|(d->buttons&4);
            if (d->protocol==CIUKI_SERIAL_MOUSE_MICROSOFT) d->count=0;
        } else if (d->count==4) {
            d->count=0;
            if (d->protocol==CIUKI_SERIAL_MOUSE_LOGITECH) {
                if (b&0x0c) return false; /* unimplemented M++ extension */
                p->buttons=(d->buttons&3)|((b>>3)&4);
            } else {
                p->dx=signed8(((d->bytes[0]<<6)&0xc0)|d->bytes[1]);
                p->dy=signed8(((d->bytes[0]<<4)&0xc0)|d->bytes[2]);
                p->buttons=((d->bytes[0]>>5)&1)|((d->bytes[0]>>3)&2)|((b>>2)&4);
                p->wheel=(b&8)-(b&7);
            }
        } else return false;
    }
    d->buttons=(uint8_t)p->buttons; return true;
}
