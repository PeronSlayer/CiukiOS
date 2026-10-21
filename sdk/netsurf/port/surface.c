/* SPDX-License-Identifier: MIT
 * Byte-encoded desktop v3 binding, testable without a kernel or framebuffer. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "surface.h"
#include "apps/libdeskui/keymap.h"

static void drop(struct ciuki_browser_surface *s,struct ciuki_browser_storage *b)
{
    if (b->map) s->ops.unmap(b->map,b->info.allocation_bytes);
    if (b->fd>=0) s->ops.transport.close(b->fd);
    free(b->back); memset(b,0,sizeof(*b)); b->fd=-1;
}
static int allocate(struct ciuki_browser_surface *s,struct ciuki_browser_storage *b,int w,int h)
{
    if(w<300 || w>1024 || h<200+CIUKI_BROWSER_TOP+CIUKI_BROWSER_STATUS || h>768) { errno=EINVAL; return -1; }
    memset(b,0,sizeof(*b)); b->fd=-1;
    b->fd=s->ops.create(w,h,CIUKI_SURFACE_XRGB8888);
    if(b->fd<0) return -1;
    b->info.size=sizeof(b->info);
    if(s->ops.transport.info(b->fd,&b->info) || !desk_surface_valid(&b->info) ||
       b->info.width!=(unsigned)w || b->info.height!=(unsigned)h) goto fail;
    b->map=s->ops.map(b->fd,PROT_READ|PROT_WRITE);
    if(b->map==MAP_FAILED) b->map=NULL;
    if(!b->map) goto fail;
    b->back=calloc((size_t)w*h,4);
    if(!b->back) goto fail;
    return 0;
fail:
    drop(s,b); return -1;
}
int ciuki_browser_open(struct ciuki_browser_surface *s,const struct ciuki_browser_ops *ops,int channel,int w,int h)
{
    memset(s,0,sizeof(*s)); s->ops=*ops;
    s->current.fd=s->pending.fd=-1; s->connection.channel=channel;
    s->window=1; s->generation=1;
    if(allocate(s,&s->current,w,h)) goto fail;
    if(du3_connect(&s->connection,channel,&s->ops.transport)) goto fail;
    uint8_t b[100]={0}; const char *title="NetSurf - CiukiOS";
    desk_put32(b,DESK_V3_OVERLAPPED); desk_put32(b+12,40); desk_put32(b+16,40);
    desk_put32(b+20,w); desk_put32(b+24,h); desk_put32(b+28,1);
    desk_put32(b+32,strlen(title)); memcpy(b+36,title,strlen(title));
    s->create_request=du3_request(&s->connection,DESK_CREATE_WINDOW,s->window,s->generation,b,36+strlen(title),s->current.fd);
    if(!s->create_request) goto fail;
    return 0;
fail:
    ciuki_browser_close(s); return -1;
}
void ciuki_browser_close(struct ciuki_browser_surface *s)
{
    if(s->ready && !s->connection.disconnected)
        du3_request(&s->connection,DESK_CLOSE,s->window,s->generation,NULL,0,-1);
    drop(s,&s->pending); drop(s,&s->current);
    if(s->connection.channel>=0) s->ops.transport.close(s->connection.channel);
    s->connection.channel=-1; s->connection.disconnected=1; s->closed=true;
}
void ciuki_browser_damage(struct ciuki_browser_surface *s,nsfb_bbox_t b)
{
    int w=s->current.info.width,h=s->current.info.height;
    if(b.x0<0)b.x0=0; if(b.y0<0)b.y0=0;
    if(b.x1>w)b.x1=w; if(b.y1>h)b.y1=h;
    if(b.x1<=b.x0 || b.y1<=b.y0)return;
    if(!s->dirty)s->damage=b;
    else {
        if(b.x0<s->damage.x0)s->damage.x0=b.x0; if(b.y0<s->damage.y0)s->damage.y0=b.y0;
        if(b.x1>s->damage.x1)s->damage.x1=b.x1; if(b.y1>s->damage.y1)s->damage.y1=b.y1;
    }
    s->dirty=true;
}
int ciuki_browser_present(struct ciuki_browser_surface *s)
{
    if(!s->ready || !s->dirty || s->swap_request || s->damaged)return 0;
    nsfb_bbox_t b=s->damage;
    unsigned stride=s->current.info.stride/4;
    for(int y=b.y0;y<b.y1;y++)
        memcpy(s->current.map+(size_t)y*stride+b.x0,s->current.back+(size_t)y*stride+b.x0,(size_t)(b.x1-b.x0)*4);
    uint8_t wire[16]; desk_put32(wire,b.x0); desk_put32(wire+4,b.y0);
    desk_put32(wire+8,b.x1-b.x0); desk_put32(wire+12,b.y1-b.y0);
    uint32_t request=du3_request(&s->connection,DESK_DAMAGE,s->window,s->generation,wire,16,-1);
    if(!request)return -1;
    /* The server copies/consumes the shared region before returning RESULT. */
    s->damage_request=request;s->damaged=true; s->dirty=false; return 0;
}
static void resize_request(struct ciuki_browser_surface *s)
{
    if(s->swap_request || !s->configure || !s->ready)return;
    if(allocate(s,&s->pending,s->want_width,s->want_height))return;
    uint8_t b[12]; desk_put32(b,s->want_width); desk_put32(b+4,s->want_height); desk_put32(b+8,s->configure);
    s->swap_request=du3_request(&s->connection,DESK_V3_RESIZE,s->window,s->generation,b,12,s->pending.fd);
    if(!s->swap_request)drop(s,&s->pending);
    else s->configure=0;
}
static void push(struct ciuki_browser_surface *s,nsfb_event_t e)
{
    if(s->count==CIUKI_BROWSER_EVENTS) { s->closed=true; return; }
    s->events[(s->head+s->count++)%CIUKI_BROWSER_EVENTS]=e;
}
bool ciuki_browser_event(struct ciuki_browser_surface *s,nsfb_event_t *out)
{
    if(!s->count)return false;
    *out=s->events[s->head]; s->head=(s->head+1)%CIUKI_BROWSER_EVENTS; s->count--; return true;
}
static unsigned key(unsigned code)
{
    switch(code) {
    case 0x1:return NSFB_KEY_ESCAPE; case 0xe:return NSFB_KEY_BACKSPACE;
    case 0xf:return NSFB_KEY_TAB; case 0x1c:return NSFB_KEY_RETURN;
    case 0x2a:return NSFB_KEY_LSHIFT; case 0x36:return NSFB_KEY_RSHIFT;
    case 0x1d:return NSFB_KEY_LCTRL; case 0x11d:return NSFB_KEY_RCTRL;
    case 0x38:return NSFB_KEY_LALT; case 0x138:return NSFB_KEY_RALT;
    case 0x147:return NSFB_KEY_HOME; case 0x148:return NSFB_KEY_UP;
    case 0x149:return NSFB_KEY_PAGEUP; case 0x14b:return NSFB_KEY_LEFT;
    case 0x14d:return NSFB_KEY_RIGHT; case 0x14f:return NSFB_KEY_END;
    case 0x150:return NSFB_KEY_DOWN; case 0x151:return NSFB_KEY_PAGEDOWN;
    case 0x152:return NSFB_KEY_INSERT; case 0x153:return NSFB_KEY_DELETE;
    default:return 0;
    }
}
void ciuki_browser_input(struct ciuki_browser_surface *s,const struct ciuki_input_event *in)
{
    nsfb_event_t e={0};
    switch(in->type) {
    case CIUKI_INPUT_RESYNC: ciuki_browser_release(s);return;
    case CIUKI_INPUT_TEXT:
        if(in->code<32 || in->code>0x10ffff || (in->code>=0xd800 && in->code<=0xdfff))return;
        e.type=NSFB_EVENT_TEXT; e.value.keycode=in->code; push(s,e);return;
    case CIUKI_INPUT_KEY: {
        if(in->code<0 || in->code>0x200)return;
        unsigned code=key(in->code);
        /* Ctrl shortcuts have no desktop TEXT event. */
        if(!code && (s->keys[0x1d] || s->keys[0x11d])) code=du_keymap(DU_LAYOUT_US,in->code,0);
        if(!in->value)code=s->keys[in->code];
        s->keys[in->code]=in->value?code:0;
        if(!code)return;
        e.type=in->value?NSFB_EVENT_KEY_DOWN:NSFB_EVENT_KEY_UP; e.value.keycode=code; push(s,e);return;
    }
    case CIUKI_INPUT_MOTION:
        s->mouse_x=in->value; s->mouse_y=in->value2;
        if(!s->buttons && (s->mouse_y<CIUKI_BROWSER_TOP || s->mouse_y>=(int)s->current.info.height-CIUKI_BROWSER_STATUS))return;
        e.type=NSFB_EVENT_MOVE_ABSOLUTE; e.value.vector.x=s->mouse_x;
        e.value.vector.y=s->mouse_y-CIUKI_BROWSER_TOP; push(s,e);return;
    case CIUKI_INPUT_BUTTON:
        if(in->code<1 || in->code>3)return;
        if(!in->value && !(s->buttons&(1u<<in->code)))return;
        if(in->value && (s->mouse_y<CIUKI_BROWSER_TOP || s->mouse_y>=(int)s->current.info.height-CIUKI_BROWSER_STATUS))return;
        if(in->value)s->buttons|=1u<<in->code; else s->buttons&=~(1u<<in->code);
        e.type=in->value?NSFB_EVENT_KEY_DOWN:NSFB_EVENT_KEY_UP;
        e.value.keycode=in->code==1?NSFB_KEY_MOUSE_1:in->code==2?NSFB_KEY_MOUSE_3:NSFB_KEY_MOUSE_2;
        push(s,e);return;
    case CIUKI_INPUT_WHEEL:
        if(s->mouse_y<CIUKI_BROWSER_TOP || s->mouse_y>=(int)s->current.info.height-CIUKI_BROWSER_STATUS)return;
        e.type=NSFB_EVENT_KEY_DOWN; e.value.keycode=in->value>0?NSFB_KEY_MOUSE_4:NSFB_KEY_MOUSE_5;
        if(in->value) { push(s,e); e.type=NSFB_EVENT_KEY_UP; push(s,e); } return;
    default:return;
    }
}
void ciuki_browser_release(struct ciuki_browser_surface *s)
{
    for(unsigned k=0;k<0x201;k++)if(s->keys[k]) {
        push(s,(nsfb_event_t){.type=NSFB_EVENT_KEY_UP,.value.keycode=s->keys[k]}); s->keys[k]=0;
    }
    for(unsigned k=1;k<=3;k++)if(s->buttons&(1u<<k)) {
        unsigned code=k==1?NSFB_KEY_MOUSE_1:k==2?NSFB_KEY_MOUSE_3:NSFB_KEY_MOUSE_2;
        push(s,(nsfb_event_t){.type=NSFB_EVENT_KEY_UP,.value.keycode=code});
    }
    s->buttons=0;
}
int ciuki_browser_packet(struct ciuki_browser_surface *s,const struct desk_packet *p)
{
    if(p->window_id!=s->window || p->generation!=s->generation)return 0;
    if(p->opcode==DESK_V3_RESULT) {
        if(p->request==s->create_request && p->index==DESK_CREATE_WINDOW) {
            s->create_request=0; if(p->result) { s->closed=true; return -1; }
            s->ready=true;
            ciuki_browser_damage(s,(nsfb_bbox_t){0,0,s->current.info.width,s->current.info.height});
        } else if(p->request==s->swap_request && p->index==DESK_V3_RESIZE) {
            s->swap_request=0;
            if(!p->result) {
                drop(s,&s->current); s->current=s->pending; memset(&s->pending,0,sizeof(s->pending));s->pending.fd=-1;
                s->resized=true; s->dirty=false; s->damaged=false;
            } else drop(s,&s->pending);
        } else if(p->index==DESK_DAMAGE && p->request==s->damage_request) { s->damaged=false;s->damage_request=0; }
    } else if(p->opcode==DESK_CONFIGURE) {
        if(!s->last_configure || (int32_t)(p->serial-s->last_configure)>0) {
            s->last_configure=s->configure=p->serial; s->want_width=p->width; s->want_height=p->height;
        }
    } else if(p->opcode==DESK_PING) s->ping=p->serial;
    else if(p->opcode==DESK_CLOSE_REQUEST || p->opcode==DESK_CLOSED)s->closed=true;
    else if((p->opcode==DESK_V3_ACTIVATE && !p->focused) ||
            (p->opcode==DESK_V3_POINTER_STATE && !(p->flags&DESK_V3_CAPTURE)))ciuki_browser_release(s);
    resize_request(s);return 0;
}
