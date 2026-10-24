/* SPDX-License-Identifier: MIT */
#include "protocol.h"
#include "settings_catalog.h"
#include "sounds.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
/* Bounded NUL check; the freestanding Win32 user32 build of this file has
 * no memchr (lead fix 2026-10-23 after the f2-101b kernel-suite run). */
static int has_nul(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!s[i]) return 1;
    return 0;
}
uint32_t desk_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
void desk_put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}
void desk_message(struct ciuki_message *m, unsigned op, unsigned n)
{
    memset(m, 0, sizeof(*m));
    m->length = n;
    m->data[0] = DESK_VERSION;
    m->data[1] = (uint8_t)op;
}
void desk_event_message(struct ciuki_message *m, unsigned op,
                        const struct ciuki_input_event *e)
{
    desk_message(m, op, 44);
    desk_put32(m->data + 4, e->sequence);
    desk_put32(m->data + 8, e->source);
    desk_put32(m->data + 12, e->generation);
    desk_put32(m->data + 16, e->type);
    desk_put32(m->data + 20, (uint32_t)e->monotonic_ns);
    desk_put32(m->data + 24, (uint32_t)(e->monotonic_ns >> 32));
    desk_put32(m->data + 28, (uint32_t)e->code);
    desk_put32(m->data + 32, (uint32_t)e->value);
    desk_put32(m->data + 36, (uint32_t)e->value2);
    desk_put32(m->data + 40, e->lost_count);
}
int desk_surface_valid(const struct ciuki_surface_info *s)
{
    if (!s || s->size != sizeof(*s) || !s->width || !s->height ||
        s->width > CIUKI_SURFACE_DIMENSION_MAX ||
        s->height > CIUKI_SURFACE_DIMENSION_MAX ||
        s->format != CIUKI_SURFACE_XRGB8888 || s->stride != s->width * 4)
        return 0;
    uint32_t n = (s->stride * s->height + 4095u) & ~4095u;
    return n <= CIUKI_SURFACE_MAX && s->allocation_bytes == n;
}
static int valid_event(unsigned op, const struct ciuki_input_event *e)
{
    if (op == DESK_KEY) {
        if (e->type==CIUKI_INPUT_CONSUMER)
            return (unsigned)e->code>=CIUKI_CONSUMER_PLAY_PAUSE && (unsigned)e->code<=CIUKI_CONSUMER_PREVIOUS &&
                e->value>=0 && e->value<=1 && e->value2>=1 && (unsigned)e->value2<=CIUKI_HID_DEVICES &&
                e->lost_count==CIUKI_HID_VERSION;

        if (e->type == CIUKI_INPUT_KEY)
            return e->code >= 0 && e->code <= 0x200 &&
                   e->value >= 0 && e->value <= 2 && !e->value2 && !e->lost_count;
        if (e->type == CIUKI_INPUT_TEXT)
            return e->code >= 0 && e->code <= 0x10ffff &&
                   !(e->code >= 0xd800 && e->code <= 0xdfff) &&
                   !e->value && !e->value2 && !e->lost_count;
        if (e->type == CIUKI_INPUT_RESYNC)
            return !e->code && e->value >= 0 && e->value <= 7 && !e->value2;
        return 0;
    }
    if (op == DESK_MOTION)
        return (e->type == CIUKI_INPUT_MOTION || e->type == CIUKI_INPUT_WHEEL) && !e->code && !e->lost_count;
    return op == DESK_BUTTON && e->type == CIUKI_INPUT_BUTTON &&
           e->code >= 1 && e->code <= 3 && e->value >= 0 &&
           e->value <= 1 && !e->value2 && !e->lost_count;
}
int desk_utf8_valid(const uint8_t *s, unsigned n)
{
    for (unsigned i=0;i<n;) {
        uint32_t c=s[i++], minimum=0; unsigned extra=0;
        if (c<0x80) { if (c<32 || c==127) return 0; continue; }
        if (c>=0xc2 && c<=0xdf) { c&=31; extra=1; minimum=0x80; }
        else if (c>=0xe0 && c<=0xef) { c&=15; extra=2; minimum=0x800; }
        else if (c>=0xf0 && c<=0xf4) { c&=7; extra=3; minimum=0x10000; }
        else return 0;
        if (extra>n-i) return 0;
        while (extra--) { uint32_t b=s[i++]; if ((b&0xc0)!=0x80) return 0; c=(c<<6)|(b&63); }
        if (c<minimum || c>0x10ffff || (c>=0xd800 && c<=0xdfff) ||
            (c>=0x80 && c<0xa0)) return 0;
    }
    return 1;
}
void desk_v3_message(struct ciuki_message *m, unsigned op, unsigned payload,
                     uint32_t request, uint32_t id, uint32_t generation)
{
    desk_message(m,op,DESK_V3_HEADER+payload); m->data[0]=DESK_V3_VERSION;
    desk_put32(m->data+4,request); desk_put32(m->data+8,id); desk_put32(m->data+12,generation);
}
/* G5 clipboard payload (after the 4- or 16-byte header). Every field an
 * operation does not take is zero; fds are exact. */
static int clip_parse(const uint8_t *b, unsigned n, int client, unsigned fds, struct desk_clip_wire *w)
{
    memset(w,0,sizeof(*w));
    if (client) {
        if (n<28) return 0;
        w->serial=desk_get32(b); w->op=desk_get32(b+4); w->format=desk_get32(b+8); w->flags=desk_get32(b+12);
        w->bytes=desk_get32(b+16); w->token=desk_get32(b+20); w->count=desk_get32(b+24);
        if (!w->serial || w->op<DESK_CLIP_QUERY || w->op>DESK_CLIP_RELEASE || w->count>DESK_CLIP_NAME_MAX || n!=28+w->count) return 0;
        unsigned want_fd=0;
        switch (w->op) {
        case DESK_CLIP_PUT:
            if (!w->format || w->token || w->count || (w->flags&~3u) || w->flags==3 ||
                w->bytes>DESK_CLIP_ITEM_MAX || ((w->flags&DESK_CLIP_DELAYED) && w->bytes)) return 0;
            want_fd=w->bytes ? 1 : 0; break;
        case DESK_CLIP_GET: case DESK_CLIP_NAME:
            if (!w->format || w->flags || w->bytes || w->token || w->count) return 0;
            break;
        case DESK_CLIP_EMPTY: case DESK_CLIP_OPEN: case DESK_CLIP_RELEASE:
            if (w->format || w->flags || w->bytes || w->count) return 0;
            break;
        case DESK_CLIP_WATCH:
            if (w->format || w->flags>1 || w->bytes || w->token || w->count) return 0;
            break;
        case DESK_CLIP_REGISTER:
            if (w->format || w->flags || w->bytes || w->token || !w->count || !desk_utf8_valid(b+28,w->count) ||
                has_nul((const char *)b+28,w->count)) return 0;
            memcpy(w->name,b+28,w->count); break;
        default: /* QUERY, CLOSE */
            if (w->format || w->flags || w->bytes || w->token || w->count) return 0;
            break;
        }
        return fds==want_fd;
    }
    if (n<36) return 0;
    w->serial=desk_get32(b); w->kind=desk_get32(b+4); w->op=desk_get32(b+8); w->result=(int32_t)desk_get32(b+12);
    w->format=desk_get32(b+16); w->bytes=desk_get32(b+20); w->sequence=desk_get32(b+24); w->token=desk_get32(b+28);
    w->count=desk_get32(b+32);
    if (w->kind<DESK_CLIP_REPLY || w->kind>DESK_CLIP_DESTROY || w->result>0 || w->result < -4095) return 0;
    if (w->kind!=DESK_CLIP_REPLY) {
        if (w->serial || w->op || w->result || w->count || n!=36 || fds) return 0;
        if (w->kind==DESK_CLIP_RENDER ? !w->format || w->bytes : w->format || w->bytes) return 0;
        return 1;
    }
    if (!w->serial || w->op<DESK_CLIP_QUERY || w->op>DESK_CLIP_RELEASE) return 0;
    if (w->op==DESK_CLIP_NAME) {
        if (w->count>DESK_CLIP_NAME_MAX || n!=36+w->count || !desk_utf8_valid(b+36,w->count) ||
            has_nul((const char *)b+36,w->count)) return 0;
        memcpy(w->name,b+36,w->count);
    } else {
        if (w->count>DESK_CLIP_LIST_MAX || n!=36+4*w->count || (w->count && w->op!=DESK_CLIP_QUERY)) return 0;
        for (unsigned i=0;i<w->count;i++) w->formats[i]=desk_get32(b+36+4*i);
    }
    if (w->bytes>DESK_CLIP_ITEM_MAX && w->op!=DESK_CLIP_QUERY) return 0;
    return fds==(w->op==DESK_CLIP_GET && !w->result && w->bytes ? 1u : 0u);
}
int desk_clip_message(struct ciuki_message *m, unsigned version, int client,
                      const struct desk_clip_wire *w, int fd)
{
    uint8_t b[240]; unsigned n;
    memset(b,0,sizeof(b));
    if (client) {
        unsigned names=w->op==DESK_CLIP_REGISTER ? w->count : 0;
        if (names>DESK_CLIP_NAME_MAX) return -EINVAL;
        desk_put32(b,w->serial); desk_put32(b+4,w->op); desk_put32(b+8,w->format); desk_put32(b+12,w->flags);
        desk_put32(b+16,w->bytes); desk_put32(b+20,w->token); desk_put32(b+24,names);
        memcpy(b+28,w->name,names); n=28+names;
    } else {
        unsigned count=w->kind==DESK_CLIP_REPLY ? w->count : 0;
        unsigned extra=w->op==DESK_CLIP_NAME ? count : 4*count;
        if (count>(w->op==DESK_CLIP_NAME ? DESK_CLIP_NAME_MAX : DESK_CLIP_LIST_MAX)) return -EINVAL;
        desk_put32(b,w->serial); desk_put32(b+4,w->kind); desk_put32(b+8,w->op); desk_put32(b+12,(uint32_t)w->result);
        desk_put32(b+16,w->format); desk_put32(b+20,w->bytes); desk_put32(b+24,w->sequence); desk_put32(b+28,w->token);
        desk_put32(b+32,count);
        if (w->op==DESK_CLIP_NAME) memcpy(b+36,w->name,count);
        else for (unsigned i=0;i<count;i++) desk_put32(b+36+4*i,w->formats[i]);
        n=36+extra;
    }
    if (version==DESK_V3_VERSION) {
        if (n>240) return -EINVAL;
        desk_v3_message(m,client ? DESK_V3_CLIPBOARD : DESK_V3_CLIPBOARD_EVENT,n,client ? w->serial : 0,0,0);
        memcpy(m->data+16,b,n);
    } else {
        if (n>252) return -EINVAL;
        desk_message(m,client ? DESK_CLIPBOARD : DESK_CLIPBOARD_EVENT,4+n); m->data[0]=DESK_SHELL_VERSION;
        memcpy(m->data+4,b,n);
    }
    if (fd>=0) { m->fd_count=1; m->fds[0]=fd; }
    struct desk_packet p;
    struct ciuki_message check=*m;
    if (version!=DESK_V3_VERSION) check.data[2]=0;
    return desk_parse(&check,client,NULL,&p) ? 0 : -EINVAL;
}
void desk_clip_dims(uint32_t n, uint32_t *width, uint32_t *height)
{
    uint32_t pixels=n ? (n+3)/4 : 1;
    uint32_t w=pixels<2048 ? pixels : 2048;
    *width=w; *height=(pixels+w-1)/w;
}
static int v3_parse(const struct ciuki_message *m, int client,
                    const struct ciuki_surface_info *s, struct desk_packet *p)
{
    if (m->length<16 || m->length>256 || m->reserved || m->data[2] || m->data[3] || m->fd_count>1) return 0;
    for (unsigned k=m->length;k<256;k++) if (m->data[k]) return 0;
    for (unsigned k=m->fd_count;k<4;k++) if (m->fds[k]) return 0;
    if (m->fd_count && m->fds[0]<0) return 0;
    memset(p,0,sizeof(*p)); p->opcode=m->data[1];
    p->request=desk_get32(m->data+4); p->window_id=desk_get32(m->data+8); p->generation=desk_get32(m->data+12);
    const uint8_t *b=m->data+16; unsigned n=m->length-16, expected=0, fd=0;
    if (p->opcode==DESK_HELLO) return client && !n && !m->fd_count && !p->window_id && !p->generation && !p->request;
    if (p->opcode==DESK_V3_CLIPBOARD || p->opcode==DESK_V3_CLIPBOARD_EVENT) {
        if (p->window_id || p->generation || client!=(p->opcode==DESK_V3_CLIPBOARD) ||
            !clip_parse(b,n,client,m->fd_count,&p->clip)) return 0;
        return client ? p->request==p->clip.serial : !p->request;
    }
    if (!p->window_id || !p->generation) return 0;
    switch (p->opcode) {
    case DESK_V3_TRAY_ICON:
        if (!client || n<20) return 0;
        p->flags=desk_get32(b); p->index=desk_get32(b+4);
        p->style=desk_get32(b+8); p->serial=desk_get32(b+12); p->count=desk_get32(b+16);
        if (p->flags>2 || (p->style&~7u) || p->count>127 || n!=20+p->count ||
            !desk_utf8_valid(b+20,p->count) || (!(p->style&4u) && p->count) ||
            (!(p->style&1u) && p->serial) || (p->flags==2 && (p->style || p->serial || p->count))) return 0;
        memcpy(p->tooltip,b+20,p->count);
        fd=!!(p->style&2u);
        if (fd && (!desk_surface_valid(s) || s->width!=16 || s->height!=16)) return 0;
        expected=n; break;
    case DESK_V3_TRAY_BALLOON:
        if (!client || n<16) return 0;
        p->index=desk_get32(b); p->count=desk_get32(b+4); p->flags=desk_get32(b+8);
        p->serial=desk_get32(b+12)&0xffffu; p->style=desk_get32(b+12)>>16;
        if (p->count>63 || p->flags>159 || n!=16+p->count+p->flags ||
            !desk_utf8_valid(b+16,p->count) || !desk_utf8_valid(b+16+p->count,p->flags) ||
            p->serial<10000 || p->serial>30000 || p->style>3) return 0;
        memcpy(p->title,b+16,p->count); memcpy(p->argument,b+16+p->count,p->flags);
        expected=n; break;
    case DESK_V3_TRAY_EVENT:
        if (client || n!=20) return 0;
        p->index=desk_get32(b); p->serial=desk_get32(b+4); p->flags=desk_get32(b+8);
        p->x=(int32_t)desk_get32(b+12); p->y=(int32_t)desk_get32(b+16);
        if (!p->serial) return 0;
        expected=20; break;
    case DESK_V3_FLASH: case DESK_ACCEPT_FILES:
        if (!client || n!=4 || (p->flags=desk_get32(b))>1) return 0;
        expected=4; break;
    case DESK_DROP_FILES:
        if (client || n!=12 || !(p->serial=desk_get32(b))) return 0;
        p->x=(int32_t)desk_get32(b+4); p->y=(int32_t)desk_get32(b+8);
        expected=12; fd=1; break;
    case DESK_CREATE_WINDOW:
        if (!client || n<36) return 0;
        p->style=desk_get32(b); p->owner=desk_get32(b+4); p->owner_generation=desk_get32(b+8);
        p->x=(int32_t)desk_get32(b+12); p->y=(int32_t)desk_get32(b+16);
        p->width=desk_get32(b+20); p->height=desk_get32(b+24); p->scale=desk_get32(b+28);
        p->count=desk_get32(b+32);
        if (p->count>DESK_TITLE_MAX || n!=36+p->count || !desk_utf8_valid(b+36,p->count) ||
            (p->style&~DESK_V3_STYLES) || (p->style&3)>DESK_V3_TOOL ||
            (!!p->owner != !!p->owner_generation) || ((p->style&DESK_V3_MODAL) && !p->owner)) return 0;
        memcpy(p->title,b+36,p->count); fd=1; expected=n; break;
    case DESK_V3_PLACE:
        if (!client || n!=28) return 0;
        p->x=(int32_t)desk_get32(b); p->y=(int32_t)desk_get32(b+4);
        p->width=desk_get32(b+8); p->height=desk_get32(b+12); p->scale=desk_get32(b+16);
        p->flags=desk_get32(b+20); p->style=desk_get32(b+24);
        if (p->flags>3 || (p->style&~DESK_V3_STYLES) || (p->style&3)>DESK_V3_TOOL) return 0;
        expected=28; break;
    case DESK_V3_LIMITS:
        if (!client || n!=16) return 0;
        p->min_width=desk_get32(b); p->min_height=desk_get32(b+4); p->max_width=desk_get32(b+8); p->max_height=desk_get32(b+12);
        if (p->min_width>2048 || p->min_height>2048 || p->max_width>2048 || p->max_height>2048 ||
            (p->max_width && p->min_width>p->max_width) || (p->max_height && p->min_height>p->max_height)) return 0;
        expected=16; break;
    case DESK_V3_WIN32_HWND:
        if(!client || n!=4 || !(p->flags=desk_get32(b)))return 0;
        expected=4;break;
    case DESK_V3_WINDOW_ORDER:
        if (!client || n!=20) return 0;
        p->flags=desk_get32(b); p->index=desk_get32(b+4); p->serial=desk_get32(b+8);
        p->owner=desk_get32(b+12); p->owner_generation=desk_get32(b+16);
        if (!p->flags || (p->flags&~3u) ||
            (!(p->flags&DESK_V3_ORDER_Z) && (p->index || p->serial)) ||
            (!(p->flags&DESK_V3_ORDER_OWNER) && (p->owner || p->owner_generation)) ||
            (!!p->owner != !!p->owner_generation)) return 0;
        if (p->flags&DESK_V3_ORDER_Z) {
            if (!p->index || p->index>=DESK_V3_ORDER_NOTOPMOST) {
                if (p->serial) return 0;
            } else if (!p->serial) return 0;
        }
        expected=20; break;
    case DESK_V3_RESIZE:
        if (!client || n!=12) return 0;
        p->width=desk_get32(b); p->height=desk_get32(b+4); p->serial=desk_get32(b+8);
        if (!p->serial) return 0;
        expected=12; fd=1; break;
    case DESK_DAMAGE: case DESK_V3_PRESENT:
        if (!client || n!=16 || !desk_surface_valid(s)) return 0;
        p->x=(int32_t)desk_get32(b); p->y=(int32_t)desk_get32(b+4);
        p->width=desk_get32(b+8); p->height=desk_get32(b+12);
        if (p->x<0 || p->y<0 || (unsigned)p->x>s->width || (unsigned)p->y>s->height ||
            p->width>s->width-(unsigned)p->x || p->height>s->height-(unsigned)p->y) return 0;
        expected=16; break;
    case DESK_SET_TITLE: case DESK_V3_CRASH: {
        unsigned offset=p->opcode==DESK_SET_TITLE ? 4 : 12;
        if (!client || n<offset) return 0;
        p->count=desk_get32(b+offset-4);
        if ((p->opcode==DESK_V3_CRASH && !p->count) || p->count>DESK_TITLE_MAX || n!=offset+p->count || !desk_utf8_valid(b+offset,p->count)) return 0;
        memcpy(p->title,b+offset,p->count);
        if (offset==12) { p->flags=desk_get32(b); p->values[0]=desk_get32(b+4); }
        expected=n; break;
    }
    case DESK_SET_CURSOR:
        if (!client || n!=12) return 0;
        p->flags=desk_get32(b); p->index=desk_get32(b+4); p->result=(int32_t)desk_get32(b+8);
        if (p->flags>=DESK_CURSOR_COUNT || p->index>4 || p->result < -32768 || p->result>32767) return 0;
        expected=12; break;
    case DESK_V3_CURSOR_BITMAP:
        if (!client || n!=12 || !desk_surface_valid(s) || s->width!=32 || s->height!=32) return 0;
        p->index=desk_get32(b); p->x=(int32_t)desk_get32(b+4); p->y=(int32_t)desk_get32(b+8);
        if (!p->index || p->index>4 || p->x<0 || p->x>31 || p->y<0 || p->y>31) return 0;
        expected=12; fd=1; break;
    case DESK_V3_POINTER:
        if (!client || n!=20) return 0;
        p->flags=desk_get32(b); p->x=(int32_t)desk_get32(b+4); p->y=(int32_t)desk_get32(b+8);
        p->width=desk_get32(b+12); p->height=desk_get32(b+16);
        if ((p->flags&~15u) || ((p->flags&DESK_V3_RELATIVE) && !(p->flags&(DESK_V3_CAPTURE|DESK_V3_CLIP))) ||
            ((p->flags&DESK_V3_CLIP) && (!p->width || !p->height || p->width>2048 || p->height>2048)) ||
            (!(p->flags&DESK_V3_CLIP) && (p->x || p->y || p->width || p->height))) return 0;
        if (p->x < -8192 || p->x>8192 || p->y < -8192 || p->y>8192) return 0;
        expected=20; break;
    case DESK_V3_WARP:
        if (!client || n!=8) return 0;
        p->x=(int32_t)desk_get32(b); p->y=(int32_t)desk_get32(b+4); expected=8; break;
    case DESK_V3_LEASE:
        if (!client || n!=16) return 0;
        p->flags=desk_get32(b); p->width=desk_get32(b+4); p->height=desk_get32(b+8); p->bpp=desk_get32(b+12);
        if (p->flags>1 || (!p->flags && (p->width || p->height || p->bpp)) ||
            (p->flags && (!p->width || !p->height || p->width>2048 || p->height>2048 ||
                (p->bpp!=8 && p->bpp!=16 && p->bpp!=32)))) return 0;
        expected=16; break;
    case DESK_V3_POINTER_STATE:
        if (client || n!=4) return 0;
        p->flags=desk_get32(b); if (p->flags&~15u) return 0;
        expected=4; break;
    case DESK_V3_RESTART:
        if (!client || n!=4) return 0;
        p->flags=desk_get32(b); if (p->flags&~15u) return 0;
        expected=4; break;
    case DESK_CLOSE_ACK:
        if (!client || n!=4) return 0;
        p->flags=desk_get32(b); if (p->flags>1) return 0;
        expected=4; break;
    case DESK_CLOSE: if (!client) return 0; break;
    case DESK_CLOSED: case DESK_CLOSE_REQUEST: if (client) return 0; break;
    case DESK_PING: case DESK_PONG:
        if (n!=4) return 0;
        p->serial=desk_get32(b); expected=4; break;
    case DESK_CONFIGURE:
        if (client || n!=32) return 0;
        p->width=desk_get32(b); p->height=desk_get32(b+4); p->scale=desk_get32(b+8); p->serial=desk_get32(b+12);
        p->x=(int32_t)desk_get32(b+16); p->y=(int32_t)desk_get32(b+20);
        p->flags=desk_get32(b+24); p->style=desk_get32(b+28);
        if (!p->serial || p->flags>3 || (p->style&~DESK_V3_STYLES) || (p->style&3)>DESK_V3_TOOL) return 0;
        expected=32; break;
    case DESK_V3_RESULT:
        if (client || n!=8) return 0;
        p->index=desk_get32(b); p->result=(int32_t)desk_get32(b+4);
        if (!p->request || p->result>0 || p->result < -4095) return 0;
        expected=8; break;
    case DESK_V3_ACTIVATE:
        if (n!=8) return 0;
        p->focused=desk_get32(b); p->flags=desk_get32(b+4);
        if (p->focused>1 || p->flags>1 || (client && p->focused!=p->flags)) return 0;
        expected=8; break;
    case DESK_V3_LEASE_EVENT:
        if (client || (n!=20 && n!=72)) return 0;
        p->flags=desk_get32(b); p->serial=desk_get32(b+4); p->width=desk_get32(b+8); p->height=desk_get32(b+12); p->bpp=desk_get32(b+16);
        if (p->flags>3 || !p->serial || !p->width || !p->height || p->width>2048 || p->height>2048 ||
            (p->bpp!=8 && p->bpp!=16 && p->bpp!=32)) return 0;
        if (n==72) {
            p->lease=(struct desk_lease_tuple){.token=desk_get32(b+20),.generation=desk_get32(b+24),
                .mode=desk_get32(b+28),.scale=desk_get32(b+32),
                .surface={desk_get32(b+36),desk_get32(b+40),desk_get32(b+44),desk_get32(b+48),desk_get32(b+52)},
                .native_width=desk_get32(b+56),.native_height=desk_get32(b+60),.native_bpp=desk_get32(b+64)};
            p->result=(int32_t)desk_get32(b+68);
            if (p->result>0 || p->result < -4095) return 0;
            if (p->flags==1 && (!p->lease.token || !p->lease.generation ||
                !p->lease.scale || p->lease.scale>3 || p->lease.mode>65535 ||
                p->lease.surface.width!=p->width || p->lease.surface.height!=p->height ||
                p->lease.surface.format<1 || p->lease.surface.format>4 ||
                p->lease.native_width<p->width*p->lease.scale ||
                p->lease.native_height<p->height*p->lease.scale ||
                (p->lease.native_bpp!=16 && p->lease.native_bpp!=32) || p->result)) return 0;
        }
        expected=n; break;
    case DESK_KEY: case DESK_MOTION: case DESK_BUTTON: case DESK_V3_RELATIVE_EVENT: {
        if (client || n!=40) return 0;
        struct ciuki_input_event *e=&p->event;
        e->sequence=desk_get32(b); e->source=desk_get32(b+4); e->generation=desk_get32(b+8); e->type=desk_get32(b+12);
        e->monotonic_ns=(uint64_t)desk_get32(b+16)|(uint64_t)desk_get32(b+20)<<32;
        e->code=(int32_t)desk_get32(b+24); e->value=(int32_t)desk_get32(b+28); e->value2=(int32_t)desk_get32(b+32); e->lost_count=desk_get32(b+36);
        if (!valid_event(p->opcode==DESK_V3_RELATIVE_EVENT ? DESK_MOTION : p->opcode,e) ||
            (p->opcode==DESK_V3_RELATIVE_EVENT && e->type!=CIUKI_INPUT_MOTION)) return 0;
        expected=40; break;
    }
    default: return 0;
    }
    if ((p->opcode==DESK_CREATE_WINDOW || p->opcode==DESK_V3_PLACE || p->opcode==DESK_CONFIGURE || p->opcode==DESK_V3_RESIZE) &&
        (!p->width || !p->height || p->width>2048 || p->height>2048 ||
         (p->opcode!=DESK_V3_RESIZE && (!p->scale || p->scale>3)) ||
         p->x < -8192 || p->y < -8192 || p->x>8192 || p->y>8192)) return 0;
    if ((p->opcode==DESK_CREATE_WINDOW || p->opcode==DESK_V3_RESIZE) &&
        (!desk_surface_valid(s) || s->width!=p->width || s->height!=p->height)) return 0;
    return n==expected && m->fd_count==fd;
}
int desk_parse(const struct ciuki_message *m, int client,
               const struct ciuki_surface_info *s, struct desk_packet *p)
{
    if (m->length && m->data[0]==DESK_V3_VERSION) return v3_parse(m,client,s,p);
    if (m->length < 4 || m->length > 256 || m->fd_count > 4 || m->reserved ||
        (m->data[0] != DESK_VERSION && m->data[0] != DESK_SHELL_VERSION) ||
        m->data[3] || (m->data[2] &&
        (m->data[0] != DESK_SHELL_VERSION || m->data[2] != DESK_RESIZE_MINOR))) return 0;
    for (unsigned i = m->length; i < 256; i++) if (m->data[i]) return 0;
    for (unsigned i = m->fd_count; i < 4; i++) if (m->fds[i]) return 0;
    for (unsigned i = 0; i < m->fd_count; i++) if (m->fds[i] < 0) return 0;
    memset(p, 0, sizeof(*p));
    p->opcode = m->data[1];
    int resize = m->data[2] == DESK_RESIZE_MINOR;
    if (p->opcode==DESK_CLIPBOARD || p->opcode==DESK_CLIPBOARD_EVENT)
        return m->data[0]==DESK_SHELL_VERSION && client==(p->opcode==DESK_CLIPBOARD) &&
               clip_parse(m->data+4,m->length-4,client,m->fd_count,&p->clip);
    unsigned fds = client && p->opcode == DESK_CREATE_WINDOW ? 1u : 0u;
    if (client && resize && p->opcode == DESK_DAMAGE && m->length == 24 && desk_get32(m->data + 20)) fds = 1;
    if (client && resize && p->opcode==DESK_GFX_FRAME && (m->length==68 || m->length==84) && desk_get32(m->data+64)) fds=1;
    if (m->fd_count != fds) return 0;
    unsigned n = 4;
    if (p->opcode==DESK_DOS_CLIPBOARD) {
        p->flags=desk_get32(m->data+4);
        return !client && m->data[0]==DESK_SHELL_VERSION && m->length==8 && !m->fd_count && !resize && p->flags>=1 && p->flags<=4;
    }
    if (p->opcode==DESK_DOS_FULLSCREEN)
        return !client && m->data[0]==DESK_SHELL_VERSION && m->length==4 && !m->fd_count && !resize;
    switch (p->opcode) {
    case DESK_CLOSE_ACK:
        if (!client || m->data[0]!=DESK_SHELL_VERSION || m->length!=8) return 0;
        p->flags=desk_get32(m->data+4); if (p->flags>DESK_CLOSE_DOS_ACTIVE) return 0;
        n=8; break;
    case DESK_QUERY_END_SESSION: case DESK_SESSION_ANSWER:
        /* G3: the query carries the WM_QUERYENDSESSION lParam, the answer one DESK_SESSION_* value. */
        if (client!=(p->opcode==DESK_SESSION_ANSWER) || m->data[0]!=DESK_SHELL_VERSION || m->length!=12) return 0;
        p->serial=desk_get32(m->data+4); p->flags=desk_get32(m->data+8);
        /* Serial 0 with AGREE is the registration (the window handles queries). */
        if ((!p->serial && (p->opcode==DESK_QUERY_END_SESSION || p->flags!=DESK_SESSION_AGREE)) ||
            (p->opcode==DESK_QUERY_END_SESSION ? (p->flags&~DESK_ENDSESSION_FLAGS)!=0 : p->flags>=DESK_SESSION_ANSWERS)) return 0;
        n=12; break;
    case DESK_END_SESSION:
        if (client || m->data[0]!=DESK_SHELL_VERSION || m->length!=16) return 0;
        p->serial=desk_get32(m->data+4); p->focused=desk_get32(m->data+8); p->flags=desk_get32(m->data+12);
        if (!p->serial || p->focused>1 || (p->flags&~DESK_ENDSESSION_FLAGS)) return 0;
        n=16; break;
    case DESK_GFX_FRAME:
        if (!client || !resize || (m->length!=64 && m->length!=68 && m->length!=84) || !desk_surface_valid(s)) return 0;
        if (m->length>=68) { p->resize_serial=desk_get32(m->data+64);if (m->length==68 && !p->resize_serial) return 0; }
        if (m->length==84) {
            p->timing_serial=desk_get32(m->data+68);p->render_us=desk_get32(m->data+72);
            p->submit_us=desk_get32(m->data+76);p->wait_us=desk_get32(m->data+80);
            if (!p->timing_serial && (p->render_us || p->submit_us || p->wait_us)) return 0;
        }
        p->serial=desk_get32(m->data+4);
        if (!p->serial) return 0;
        p->x=(int32_t)desk_get32(m->data+8); p->y=(int32_t)desk_get32(m->data+12);
        p->width=desk_get32(m->data+16); p->height=desk_get32(m->data+20);
        p->source_w=desk_get32(m->data+24); p->source_h=desk_get32(m->data+28);
        p->damage_x=desk_get32(m->data+32); p->damage_y=desk_get32(m->data+36);
        p->damage_w=desk_get32(m->data+40); p->damage_h=desk_get32(m->data+44);
        p->ui_x=desk_get32(m->data+48); p->ui_y=desk_get32(m->data+52);
        p->ui_w=desk_get32(m->data+56); p->ui_h=desk_get32(m->data+60);
        if (p->ui_x>s->width || p->ui_y>s->height || p->ui_w>s->width-p->ui_x || p->ui_h>s->height-p->ui_y) return 0;
        if (!p->width && !p->height) {
            for (unsigned k=8;k<48;k++) if (m->data[k]) return 0;
        } else if (p->x<0 || p->y<0 || !p->source_w || !p->source_h ||
            (uint32_t)p->x>s->width || (uint32_t)p->y>s->height ||
            p->width>s->width-(uint32_t)p->x || p->height>s->height-(uint32_t)p->y ||
            p->source_w>p->width || p->source_h>p->height ||
            p->damage_x>p->source_w || p->damage_y>p->source_h ||
            p->damage_w>p->source_w-p->damage_x || p->damage_h>p->source_h-p->damage_y) return 0;
        n=m->length; break;
    case DESK_GFX_PRESENTED:
        if (client || !resize || (m->length!=32 && m->length!=64) || desk_get32(m->data+28)) return 0;
        p->serial=desk_get32(m->data+4); p->result=(int32_t)desk_get32(m->data+8);
        p->completed_ns=(uint64_t)desk_get32(m->data+12)|(uint64_t)desk_get32(m->data+16)<<32;
        p->next_frame_ns=(uint64_t)desk_get32(m->data+20)|(uint64_t)desk_get32(m->data+24)<<32;
        if (!p->serial || p->result>0 || p->result < -4095 || p->next_frame_ns<p->completed_ns) return 0;
        if (m->length==64) {
            p->convert_ns=(uint64_t)desk_get32(m->data+32)|(uint64_t)desk_get32(m->data+36)<<32;
            p->copy_ns=(uint64_t)desk_get32(m->data+40)|(uint64_t)desk_get32(m->data+44)<<32;
            p->slot_ns=(uint64_t)desk_get32(m->data+48)|(uint64_t)desk_get32(m->data+52)<<32;
            p->refresh_ns=(uint64_t)desk_get32(m->data+56)|(uint64_t)desk_get32(m->data+60)<<32;
            if (!p->refresh_ns) return 0;
        }
        n=m->length; break;
    case DESK_GFX_QUERY: case DESK_GFX_BENCH:
        if (!client || m->data[0]!=DESK_SHELL_VERSION || m->length!=(p->opcode==DESK_GFX_QUERY ? 8u : 12u)) return 0;
        p->serial=desk_get32(m->data+4); n=m->length;
        if (p->opcode==DESK_GFX_BENCH) { p->index=desk_get32(m->data+8); if (p->index>=6) return 0; }
        break;
    case DESK_GFX_QUERY_ENGINE:
        if (!client || m->data[0]!=DESK_SHELL_VERSION || m->length!=12 ||
            desk_get32(m->data+8)!=CIUKI_DISPLAY_ENGINE_VERSION) return 0;
        p->serial=desk_get32(m->data+4); n=12; break;
    case DESK_GFX_INFO_ENGINE:
        if (client || m->data[0]!=DESK_SHELL_VERSION || m->length!=256) return 0;
        p->serial=desk_get32(m->data+4); p->result=(int32_t)desk_get32(m->data+8);
        p->width=desk_get32(m->data+12); p->height=desk_get32(m->data+16); p->bpp=desk_get32(m->data+20);
        /* Ciuki wire records use pack(4), little-endian scalars, no pointers.
         * Copy into aligned storage before inspecting the negotiated payload. */
        memcpy(&p->engine,m->data+24,sizeof(p->engine));
        if (p->result>0 || p->result < -4095 || p->engine.version!=CIUKI_DISPLAY_ENGINE_VERSION ||
            p->engine.size!=sizeof(p->engine) || p->engine.kind>CIUKI_ENGINE_NATIVE ||
            p->engine.capabilities>3 || p->engine.quarantined>1 || p->engine.ready>1 ||
            p->engine.native_used>1 || p->engine.cpu_ready>1 ||
            p->engine.software_operations!=CIUKI_ENGINE_SOFTWARE_OPERATIONS ||
            !has_nul(p->engine.driver_id,sizeof(p->engine.driver_id)) ||
            !has_nul(p->engine.display_name,sizeof(p->engine.display_name)) ||
            !has_nul(p->engine.reason,sizeof(p->engine.reason))) return 0;
        p->accelerator.driver=p->engine.kind==CIUKI_ENGINE_NATIVE ? CIUKI_ACCEL_DRIVER_LEGACY_NATIVE : CIUKI_ACCEL_DRIVER_NONE;
        p->accelerator.capabilities=p->engine.capabilities; p->accelerator.generation=p->engine.generation;
        p->accelerator.quarantined=p->engine.quarantined;
        for (unsigned k=0;k<CIUKI_ENGINE_OPERATIONS;k++) {
            p->accelerator.operations+=p->engine.native[k]; p->accelerator.fallbacks+=p->engine.software[k];
        }
        n=256; break;
    case DESK_GFX_INFO:
        if (client || m->data[0]!=DESK_SHELL_VERSION || m->length!=64) return 0;
        p->serial=desk_get32(m->data+4); p->result=(int32_t)desk_get32(m->data+8);
        p->width=desk_get32(m->data+12); p->height=desk_get32(m->data+16); p->bpp=desk_get32(m->data+20);
        p->accelerator.driver=desk_get32(m->data+24); p->accelerator.capabilities=desk_get32(m->data+28);
        p->accelerator.quarantined=desk_get32(m->data+32); p->accelerator.generation=desk_get32(m->data+36);
        p->accelerator.operations=(uint64_t)desk_get32(m->data+40)|(uint64_t)desk_get32(m->data+44)<<32;
        p->accelerator.idle_waits=(uint64_t)desk_get32(m->data+48)|(uint64_t)desk_get32(m->data+52)<<32;
        p->accelerator.fallbacks=(uint64_t)desk_get32(m->data+56)|(uint64_t)desk_get32(m->data+60)<<32;
        if (p->result>0 || p->result < -4095 || p->accelerator.driver>CIUKI_ACCEL_DRIVER_LEGACY_NATIVE ||
            p->accelerator.capabilities>3 || p->accelerator.quarantined>1) return 0;
        n=64; break;
    case DESK_GFX_RESULT:
        if (client || m->data[0]!=DESK_SHELL_VERSION || m->length!=40) return 0;
        p->serial=desk_get32(m->data+4); p->index=desk_get32(m->data+8); p->result=(int32_t)desk_get32(m->data+12);
        if (p->index>=6 || p->result>0 || p->result < -4095) return 0;
        for (unsigned k=0;k<3;k++) p->benchmark[k]=(uint64_t)desk_get32(m->data+16+k*8)|(uint64_t)desk_get32(m->data+20+k*8)<<32;
        n=40; break;
    case DESK_SYSTEM_QUERY:
        if (!client || m->data[0]!=DESK_SHELL_VERSION || m->length!=8) return 0;
        p->serial=desk_get32(m->data+4); n=8; break;
    case DESK_SYSTEM_INFO:
        if (client || m->data[0]!=DESK_SHELL_VERSION || m->length!=80) return 0;
        p->serial=desk_get32(m->data+4);
        for (unsigned i=0;i<9;i++) p->performance[i]=(uint64_t)desk_get32(m->data+8+i*8) |
            (uint64_t)desk_get32(m->data+12+i*8)<<32;
        n=80; break;
    case DESK_DRAG_BEGIN:
        if (!client || m->data[0]!=DESK_SHELL_VERSION || m->length!=12) return 0;
        p->flags=desk_get32(m->data+4); p->count=desk_get32(m->data+8);
        if ((p->flags!=1 && p->flags!=2) || !p->count || p->count>DESK_DRAG_ITEMS) return 0;
        n=12; break;
    case DESK_DRAG_OVER:
        if (client || m->data[0]!=DESK_SHELL_VERSION || m->length!=24) return 0;
        p->serial=desk_get32(m->data+4); p->flags=desk_get32(m->data+8);
        p->x=(int32_t)desk_get32(m->data+12); p->y=(int32_t)desk_get32(m->data+16); p->index=desk_get32(m->data+20);
        if (!p->serial || p->flags>1 || p->index>7 || p->x < -65535 || p->x > 65535 || p->y < -65535 || p->y > 65535) return 0;
        n=24; break;
    case DESK_DRAG_STATUS:
        if (!client || m->data[0]!=DESK_SHELL_VERSION || m->length!=16) return 0;
        p->serial=desk_get32(m->data+4); p->flags=desk_get32(m->data+8); p->values[0]=desk_get32(m->data+12);
        if (!p->serial || (p->flags!=0 && p->flags!=1 && p->flags!=2 && p->flags!=4) || p->values[0]>7 ||
            (p->flags && !(p->values[0]&p->flags))) return 0;
        n=16; break;
    case DESK_DRAG_DROP: case DESK_DROP_FILES: {
        unsigned size=p->opcode==DESK_DRAG_DROP ? 20u : 16u;
        if (client || m->data[0]!=DESK_SHELL_VERSION || m->length!=size) return 0;
        p->serial=desk_get32(m->data+4); p->x=(int32_t)desk_get32(m->data+8); p->y=(int32_t)desk_get32(m->data+12);
        if (size==20) p->flags=desk_get32(m->data+16);
        if (!p->serial || p->x < -65535 || p->x > 65535 || p->y < -65535 || p->y > 65535 ||
            (size==20 && p->flags!=1 && p->flags!=2 && p->flags!=4)) return 0;
        n=size; break;
    }
    case DESK_DRAG_END: case DESK_ACCEPT_FILES:
        if (client!=(p->opcode==DESK_ACCEPT_FILES) || m->data[0]!=DESK_SHELL_VERSION || m->length!=8) return 0;
        p->flags=desk_get32(m->data+4);
        if (p->opcode==DESK_ACCEPT_FILES ? p->flags>1 : p->flags>7) return 0;
        n=8; break;
    case DESK_FILE_DROP:
        if (m->data[0]!=DESK_SHELL_VERSION || m->length!=16) return 0;
        p->flags=desk_get32(m->data+4); p->x=(int32_t)desk_get32(m->data+8); p->y=(int32_t)desk_get32(m->data+12);
        if (p->flags>1 || p->x < -65535 || p->x > 65535 || p->y < -65535 || p->y > 65535) return 0;
        n=16; break;
    case DESK_HELLO: case DESK_CLOSE:
        if (!client) return 0;
        break;
    case DESK_CLOSED:
        if (client) return 0;
        break;
    case DESK_CLOSE_REQUEST:
        if (client || m->data[0] != DESK_SHELL_VERSION) return 0;
        break;
    case DESK_PING: case DESK_PONG:
        n = 8;
        if (m->length != n) return 0;
        p->serial = desk_get32(m->data + 4);
        break;
    case DESK_CREATE_WINDOW: {
        if (!client || m->length < 17 || !desk_surface_valid(s)) return 0;
        p->width = desk_get32(m->data + 4);
        p->height = desk_get32(m->data + 8);
        unsigned t = desk_get32(m->data + 12);
        if (!t || t > DESK_TITLE_MAX || m->length != 16 + t + (resize ? 28u : 0u) ||
            p->width != s->width || p->height != s->height) return 0;
        for (unsigned i = 0; i < t; i++) {
            if (m->data[16+i] < 32 || m->data[16+i] > 126) return 0;
            p->title[i] = (char)m->data[16+i];
        }
        n = 16 + t;
        if (resize) {
            p->flags = desk_get32(m->data + n);
            p->min_width = desk_get32(m->data + n + 4); p->min_height = desk_get32(m->data + n + 8);
            p->max_width = desk_get32(m->data + n + 12); p->max_height = desk_get32(m->data + n + 16);
            p->step_width = desk_get32(m->data + n + 20); p->step_height = desk_get32(m->data + n + 24);
            if (p->flags & ~DESK_RESIZABLE) return 0;
            if (p->flags) {
                if (!p->min_width || !p->min_height || p->min_width > p->width || p->min_height > p->height ||
                    p->max_width < p->width || p->max_height < p->height ||
                    p->max_width > CIUKI_SURFACE_DIMENSION_MAX || p->max_height > CIUKI_SURFACE_DIMENSION_MAX ||
                    !p->step_width || !p->step_height || p->step_width > p->max_width || p->step_height > p->max_height ||
                    (p->width - p->min_width) % p->step_width || (p->height - p->min_height) % p->step_height) return 0;
            } else if (p->min_width || p->min_height || p->max_width || p->max_height || p->step_width || p->step_height) return 0;
            n += 28;
        }
        break;
    }
    case DESK_SET_CURSOR:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 8) return 0;
        p->flags = desk_get32(m->data + 4);
        if (p->flags >= DESK_CURSOR_COUNT) return 0;
        n = 8; break;
    case DESK_SET_TITLE: {
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length < 9) return 0;
        unsigned t = desk_get32(m->data + 4);
        if (!t || t > DESK_TITLE_MAX || m->length != 8 + t) return 0;
        for (unsigned i = 0; i < t; i++) {
            if (m->data[8 + i] < 32 || m->data[8 + i] > 126) return 0;
            p->title[i] = (char)m->data[8 + i];
        }
        n = 8 + t;
        break;
    }
    case DESK_DAMAGE:
        if (!client || m->length != (resize ? 24u : 20u) || !desk_surface_valid(s)) return 0;
        p->x = (int32_t)desk_get32(m->data + 4);
        p->y = (int32_t)desk_get32(m->data + 8);
        p->width = desk_get32(m->data + 12);
        p->height = desk_get32(m->data + 16);
        if (p->x < 0 || p->y < 0 || (uint32_t)p->x > s->width ||
            (uint32_t)p->y > s->height || p->width > s->width - (uint32_t)p->x ||
            p->height > s->height - (uint32_t)p->y) return 0;
        p->serial = resize ? desk_get32(m->data + 20) : 0;
        if (p->serial && (p->x || p->y || p->width != s->width || p->height != s->height)) return 0;
        n = resize ? 24 : 20;
        break;
    case DESK_MOVE:
        if (!client || m->length != 12) return 0;
        p->x = (int32_t)desk_get32(m->data + 4);
        p->y = (int32_t)desk_get32(m->data + 8);
        if (p->x < -2048 || p->x > 2048 || p->y < -2048 || p->y > 2048) return 0;
        n = 12;
        break;
    case DESK_CONFIGURE:
        if (client || m->length != (resize ? 20u : 12u)) return 0;
        p->width = desk_get32(m->data + 4);
        p->height = desk_get32(m->data + 8);
        if (!p->width || !p->height || p->width > 2048 || p->height > 2048) return 0;
        if (resize) {
            p->serial = desk_get32(m->data + 12); p->flags = desk_get32(m->data + 16);
            if ((p->flags & ~DESK_CONFIGURE_COMMITTED) || (!p->serial && !p->flags)) return 0;
        }
        n = resize ? 20 : 12;
        break;
    case DESK_FOCUS:
        if (client || m->length != 8) return 0;
        p->focused = desk_get32(m->data + 4);
        if (p->focused > 1) return 0;
        n = 8;
        break;
    case DESK_WINDOWS_QUERY:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 8) return 0;
        p->serial = desk_get32(m->data + 4);
        n = 8;
        break;
    case DESK_WINDOW_INFO: {
        if (client || m->data[0] != DESK_SHELL_VERSION || m->length < 32) return 0;
        p->serial = desk_get32(m->data + 4);
        p->index = desk_get32(m->data + 8);
        p->count = desk_get32(m->data + 12);
        p->window_id = desk_get32(m->data + 16);
        p->pid = desk_get32(m->data + 20);
        p->flags = desk_get32(m->data + 24);
        unsigned t = desk_get32(m->data + 28);
        if (p->count > 16 || t > DESK_TITLE_MAX || m->length != 32 + t ||
            (p->flags & ~DESK_WINDOW_FLAGS)) return 0;
        if (!p->count ? p->index || p->window_id || p->pid || p->flags || t :
            p->index >= p->count || !p->window_id) return 0;
        for (unsigned i = 0; i < t; i++) {
            if (m->data[32+i] < 32 || m->data[32+i] > 126) return 0;
            p->title[i] = (char)m->data[32+i];
        }
        n = 32 + t;
        break;
    }
    case DESK_END_TASK:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 16) return 0;
        p->serial = desk_get32(m->data + 4);
        p->window_id = desk_get32(m->data + 8);
        p->pid = desk_get32(m->data + 12);
        if (!p->window_id && !p->pid) return 0;
        n = 16;
        break;
    case DESK_SWITCH_TO:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 12) return 0;
        p->serial = desk_get32(m->data + 4);
        p->window_id = desk_get32(m->data + 8);
        if (!p->window_id) return 0;
        n = 12;
        break;
    case DESK_LAUNCH: {
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length < 13) return 0;
        p->serial = desk_get32(m->data + 4);
        unsigned t = desk_get32(m->data + 8);
        if (!t || t > DESK_TITLE_MAX || m->length < 12 + t) return 0;
        for (unsigned i = 0; i < t; i++) {
            if (m->data[12+i] < 33 || m->data[12+i] > 126) return 0;
            p->title[i] = (char)m->data[12+i];
        }
        n = 12 + t;
        if (m->length != n) {   /* optional argument: u32 length, then that many bytes (no control characters) */
            if (m->length < n + 5) return 0;
            unsigned a = desk_get32(m->data + n);
            if (!a || a > DESK_ARG_MAX || m->length != n + 4 + a) return 0;
            for (unsigned i = 0; i < a; i++) {
                unsigned char ch = m->data[n+4+i];
                if (ch < 32 || ch == 127) return 0;
                p->argument[i] = (char)ch;
            }
            n += 4 + a;
        }
        break;
    }
    case DESK_TERMINAL_POWER:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 12) return 0;
        p->serial = desk_get32(m->data + 4); p->flags = desk_get32(m->data + 8);
        if (p->flags != CIUKI_POWER_REBOOT && p->flags != CIUKI_POWER_OFF) return 0;
        n = 12; break;
    case DESK_TASK_RESULT:
        if (client || m->data[0] != DESK_SHELL_VERSION || m->length != 16) return 0;
        p->serial = desk_get32(m->data + 4);
        p->request = desk_get32(m->data + 8);
        p->result = (int32_t)desk_get32(m->data + 12);
        if ((p->request != DESK_WINDOWS_QUERY && p->request != DESK_END_TASK &&
             p->request != DESK_SWITCH_TO && p->request != DESK_LAUNCH && p->request != DESK_SETTINGS && p->request != DESK_MIXER_SET &&
             p->request != DESK_SYSTEM_QUERY && p->request != DESK_TERMINAL_POWER &&
             p->request != DESK_GFX_QUERY && p->request != DESK_GFX_BENCH) ||
            p->result < -4095) return 0;
        n = 16;
        break;
    case DESK_SOUND:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 8) return 0;
        p->flags = desk_get32(m->data+4);
        if (p->flags != SOUND_MENU_OPEN && p->flags != SOUND_MENU_SELECT &&
            p->flags != SOUND_ERROR && p->flags != SOUND_INFORMATION && p->flags != SOUND_QUESTION &&
            p->flags != SOUND_AUTOPLAY) return 0;
        n = 8; break;
    case DESK_JOYSTICK: case DESK_JOYSTICK_RESULT: {
        int response=p->opcode==DESK_JOYSTICK_RESULT;
        if (client==response || m->data[0]!=DESK_SHELL_VERSION || m->length!=(response ? 112u : 108u)) return 0;
        p->serial=desk_get32(m->data+4);
        memcpy(&p->joystick,m->data+8,sizeof(p->joystick));
        if (!p->serial || p->joystick.version!=CIUKI_JOYSTICK_VERSION || p->joystick.size!=sizeof(p->joystick) ||
            p->joystick.operation>CIUKI_JOY_GET_GAME || p->joystick.reserved) return 0;
        if (response) { p->result=(int32_t)desk_get32(m->data+108); if (p->result>0 || p->result < -4095) return 0; }
        n=m->length; break;
    }
    case DESK_SETTINGS:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 24) return 0;
        p->serial = desk_get32(m->data + 4);
        p->request = desk_get32(m->data + 8);
        for (unsigned i = 0; i < 3; i++) p->values[i] = desk_get32(m->data + 12 + i * 4);
        if (p->request == DESK_SETTINGS_MASTER) {
            if (p->values[0] > 100 || p->values[1] > 100 || p->values[2] > 1) return 0;
        } else if (p->request == DESK_SETTINGS_DISPLAY || p->request == DESK_SETTINGS_DISPLAY_PREVIEW) {
            if (p->values[0] > UINT16_MAX || p->values[1] || p->values[2]) return 0;
        } else if (p->request==DESK_SETTINGS_SAVER_PREVIEW) {
            if (p->values[0]>2 || p->values[1] || p->values[2]) return 0;
        } else if (p->request == DESK_SETTINGS_SOUND_TEST) {
            if (p->values[0] >= SOUND_EVENTS || p->values[1] || p->values[2]) return 0;
        } else if ((p->request != DESK_SETTINGS_RELOAD && p->request != DESK_SETTINGS_SOUND_RELOAD &&
                    p->request != DESK_SETTINGS_AUDIO_TEST && p->request != DESK_SETTINGS_OPEN_POWER && p->request != DESK_SETTINGS_PROGRAMS) || p->values[0] || p->values[1] || p->values[2]) return 0;
        n = 24;
        break;
    case DESK_MIXER_SET:
        if (!client || m->data[0] != DESK_SHELL_VERSION || m->length != 20) return 0;
        p->serial = desk_get32(m->data + 4);
        p->width = desk_get32(m->data + 8);    /* left */
        p->height = desk_get32(m->data + 12);  /* right */
        p->flags = desk_get32(m->data + 16);   /* mute */
        if (p->width > 100 || p->height > 100 || p->flags > 1) return 0;
        n = 20;
        break;
    case DESK_KEY: case DESK_MOTION: case DESK_BUTTON:
        if (client || m->length != 44) return 0;
        p->event.sequence = desk_get32(m->data + 4);
        p->event.source = desk_get32(m->data + 8);
        p->event.generation = desk_get32(m->data + 12);
        p->event.type = desk_get32(m->data + 16);
        p->event.monotonic_ns = desk_get32(m->data + 20) |
                                 (uint64_t)desk_get32(m->data + 24) << 32;
        p->event.code = (int32_t)desk_get32(m->data + 28);
        p->event.value = (int32_t)desk_get32(m->data + 32);
        p->event.value2 = (int32_t)desk_get32(m->data + 36);
        p->event.lost_count = desk_get32(m->data + 40);
        if (!valid_event(p->opcode, &p->event)) return 0;
        n = 44;
        break;
    default: return 0;
    }
    return m->length == n;
}
