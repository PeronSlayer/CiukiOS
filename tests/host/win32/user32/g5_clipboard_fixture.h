/* SPDX-License-Identifier: MIT. Transport adapter, clipboard policy is production. */
#include "../../../../apps/libdeskui/clipstore.h"
static struct clip_store fixture_clip_store;
static int clip_temp[64];
static struct desk_clip_wire delayed_get;
static uint32_t delayed_client;
static void *display_output; static unsigned display_bytes;
static void fixture_clip_reply(struct desk_clip_wire *r, void *data)
{
    int fd=-1;
    if (!r->result && r->op==DESK_CLIP_GET && r->bytes) {
        uint32_t w,h; desk_clip_dims(r->bytes,&w,&h); struct u32_surface s;
        CHECK(surface(w,h,&s)); memcpy(s.pixels,data,r->bytes); fd=s.fd;
        for (unsigned i=0;i<64;i++) if (surfaces[i].fd==fd) clip_temp[i]=1;
    }
    CHECK(in_tail-in_head<256);
    CHECK(!desk_clip_message(&incoming[in_tail++%256],3,0,r,fd));
}
static void fixture_clipboard(struct ciuki_message *sent)
{
    if (!fixture_clip_store.locale) clip_init(&fixture_clip_store);
    struct desk_packet p; CHECK(desk_parse(sent,1,NULL,&p));
    void *data=NULL,*out=NULL;
    if (sent->fd_count) for (unsigned i=0;i<64;i++) if (surfaces[i].fd==sent->fds[0]) data=surfaces[i].s.pixels;
    struct desk_clip_wire r;
    if (p.clip.op==DESK_CLIP_DISPLAY_REPLY) {
        free(display_output); display_output=malloc(p.clip.bytes); CHECK(display_output);
        memcpy(display_output,data,p.clip.bytes); display_bytes=p.clip.bytes;
        r=(struct desk_clip_wire){.kind=DESK_CLIP_REPLY,.op=p.clip.op,.serial=p.clip.serial};
        fixture_clip_reply(&r,NULL); return;
    }
    unsigned old=fixture_clip_store.owner,token=fixture_clip_store.token;
    int e=clip_request(&fixture_clip_store,ui_kernel_pid(),&p.clip,data,&r,&out);
    if (!e && p.clip.op==DESK_CLIP_EMPTY && old) {
        struct desk_clip_wire event={.kind=DESK_CLIP_DESTROY,.token=token}; fixture_clip_reply(&event,NULL);
    }
    if (e==-11 && p.clip.op==DESK_CLIP_GET) {
        delayed_get=p.clip; delayed_client=ui_kernel_pid();
        struct desk_clip_wire event={.kind=DESK_CLIP_RENDER,.format=r.format,.token=fixture_clip_store.token};
        fixture_clip_reply(&event,NULL); free(out); return;
    }
    fixture_clip_reply(&r,out); free(out);
    if (delayed_client && p.clip.op==DESK_CLIP_PUT && (p.clip.flags&DESK_CLIP_RENDERED)) {
        int e=clip_request(&fixture_clip_store,delayed_client,&delayed_get,NULL,&r,&out);
        if (e==-11) {
            struct desk_clip_wire event={.kind=DESK_CLIP_RENDER,.format=r.format,.token=fixture_clip_store.token};
            fixture_clip_reply(&event,NULL);
        } else { fixture_clip_reply(&r,out); delayed_client=0; }
        free(out);
    }
    if (p.clip.op==DESK_CLIP_CLOSE) {
        struct desk_clip_wire event={.kind=DESK_CLIP_CHANGED,.sequence=fixture_clip_store.sequence}; fixture_clip_reply(&event,NULL);
    }
}
