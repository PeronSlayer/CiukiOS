/* SPDX-License-Identifier: MIT */
#include "../../../apps/libdeskui/clipstore.h"
#include "../../../apps/libdeskui/clipboard.h"
#include "../../../apps/libdeskui/clipfile.h"
#include "../../../apps/files/model.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int fm_system_clipboard_save(struct fm_model *);
int fm_system_clipboard_load(struct fm_model *);
int fm_system_clipboard_consume(struct fm_model *);
static struct clip_store store;
static uint32_t client=1;
static unsigned queries;
static int watch_ack;
static int transport(void *ctx,struct desk_clip_wire *w,const void *data,struct desk_clip_wire *r,void **out)
{ if (w->op==DESK_CLIP_QUERY) queries++; int e=clip_request(ctx,client,w,data,r,out); if (!e && w->op==DESK_CLIP_WATCH && watch_ack) r->bytes=1; return e; }
static int raw(unsigned who,unsigned op,unsigned f,unsigned flags,const void *data,unsigned n)
{
    struct desk_clip_wire w={.op=op,.format=f,.flags=flags,.bytes=n},r; void *out=NULL;
    int e=clip_request(&store,who,&w,data,&r,&out); free(out); return e;
}
static void test_protocol(void)
{
    assert(DESK_QUERY_END_SESSION==49 && DESK_END_SESSION==50 && DESK_SESSION_ANSWER==51);
    assert(DESK_CLIPBOARD==92 && DESK_CLIPBOARD_EVENT==93 && DESK_V3_CLIPBOARD==80 && DESK_V3_CLIPBOARD_EVENT==81);
    for (unsigned v=2;v<=3;v++) {
        struct desk_clip_wire w={.serial=123,.op=DESK_CLIP_OPEN,.token=0x3456}; struct ciuki_message m; struct desk_packet p;
        assert(!desk_clip_message(&m,v,1,&w,-1) && desk_parse(&m,1,NULL,&p)); assert(p.clip.token==0x3456);
        m.fd_count=1; m.fds[0]=9; assert(!desk_parse(&m,1,NULL,&p));
        w=(struct desk_clip_wire){.serial=123,.kind=DESK_CLIP_REPLY,.op=DESK_CLIP_GET,.format=13,.bytes=100};
        assert(!desk_clip_message(&m,v,0,&w,9) && desk_parse(&m,0,NULL,&p));
        m.fd_count=0; m.fds[0]=0; assert(!desk_parse(&m,0,NULL,&p));
        w=(struct desk_clip_wire){.serial=1,.op=DESK_CLIP_REGISTER,.count=2}; w.name[0]=(char)0xc0; w.name[1]=(char)0x80;
        assert(desk_clip_message(&m,v,1,&w,-1)==-EINVAL);
    }
    uint32_t width,height; desk_clip_dims(16777216,&width,&height); assert(width==2048 && height==2048);
    desk_clip_dims(9,&width,&height); assert(width*height*4>=9);
}
static void test_text(void)
{
    assert(!du_clip_open()); assert(raw(2,DESK_CLIP_OPEN,0,0,NULL,0)==-EBUSY);
    assert(!du_clip_empty()); assert(raw(2,DESK_CLIP_PUT,1,0,"bad",4)==-EPERM);
    assert(!du_clip_put(1,"caf\xe9\r\n",7)); assert(!du_clip_close());
    struct desk_clip_wire q; assert(!du_clip_query(&q)); assert(q.sequence && q.count==4);
    assert(q.formats[0]==1 && q.formats[1]==7 && q.formats[2]==13 && q.formats[3]==16);
    client=2; assert(!strcmp(du_system_clipboard_text_get(),"caf\xc3\xa9\n"));
    assert(!du_system_clipboard_text_set("Ciuki \xf0\x9f\x90\xb6\n\xc3\xa8",13));
    client=1; assert(!strcmp(du_system_clipboard_text_get(),"Ciuki \xf0\x9f\x90\xb6\n\xc3\xa8"));
    assert(!du_clip_open()); void *data=NULL; uint32_t n;
    assert(!du_clip_get(1,&data,&n)); assert(!strcmp(data,"Ciuki ?\r\n\xe8")); free(data); assert(!du_clip_close());
    assert(!du_clip_open() && !du_clip_empty());
    unsigned char locale[4]; desk_put32(locale,0x410); assert(!du_clip_put(16,locale,4));
    assert(!du_clip_put(7,"\x9b",2)); assert(!du_clip_close()); assert(!strcmp(du_system_clipboard_text_get(),"\xc3\xb8"));
    assert(du_system_clipboard_text_set("\xc0\x80",2)==-EINVAL);
    assert(!du_clip_open() && !du_clip_empty());
    unsigned char malformed[]={0,0xd8,'a',0,0,0};
    assert(!du_clip_put(13,malformed,sizeof(malformed)) && !du_clip_get(1,&data,&n));
    assert(!strcmp(data,"?a")); free(data); assert(!du_clip_close());
}
static void test_delayed_and_limits(void)
{
    assert(!du_clip_open() && !du_clip_empty());
    assert(!raw(1,DESK_CLIP_PUT,13,DESK_CLIP_DELAYED,NULL,0)); assert(!du_clip_close());
    client=2; assert(!du_clip_open()); assert(raw(2,DESK_CLIP_GET,13,0,NULL,0)==-EAGAIN);
    assert(raw(2,DESK_CLIP_PUT,13,DESK_CLIP_RENDERED,"x\0\0",4)==-EPERM);
    assert(!raw(1,DESK_CLIP_PUT,13,DESK_CLIP_RENDERED,"x\0\0",4));
    void *data=NULL; uint32_t n; assert(!du_clip_get(13,&data,&n) && n==4); free(data); assert(!du_clip_close());
    clip_disconnect(&store,1); assert(!strcmp(du_system_clipboard_text_get(),"x"));
    client=1; assert(!du_clip_open() && !du_clip_empty());
    assert(!raw(1,DESK_CLIP_PUT,0x200,DESK_CLIP_DELAYED,NULL,0)); assert(!du_clip_close());
    clip_disconnect(&store,1); assert(!du_clip_query(&(struct desk_clip_wire){0})); assert(!store.count);
    assert(!du_clip_open() && !du_clip_empty());
    unsigned char *large=calloc(1,DESK_CLIP_BYTES_MAX); assert(large);
    assert(!du_clip_put(0x200,large,DESK_CLIP_BYTES_MAX)); uint32_t before=store.sequence;
    struct desk_clip_wire stale={.op=DESK_CLIP_EMPTY,.token=before-1},reply; void *unused=NULL;
    assert(clip_request(&store,1,&stale,NULL,&reply,&unused)==-EAGAIN && store.sequence==before && store.count==1);
    assert(du_clip_put(0x201,"x",1)==-ENOMEM && store.sequence==before && store.count==1);
    free(large); assert(!du_clip_empty());
    for (unsigned i=0;i<32;i++) assert(!du_clip_put(0x200+i,"x",1));
    assert(du_clip_put(0x220,"x",1)==-ENOMEM); assert(!du_clip_close());
    assert(du_clip_register("Preferred DropEffect")==du_clip_register("PREFERRED DROPEFFECT"));
}
static void test_menu_cache(void)
{
    du_clip_bind(transport,&store); watch_ack=1; client=1;
    assert(!du_clip_open() && !du_clip_empty() && !du_clip_close()); unsigned before=queries;
    for (unsigned i=0;i<100;i++) assert(!du_clip_available(13));
    assert(queries==before+1);
    assert(!raw(2,DESK_CLIP_OPEN,0,0,NULL,0) && !raw(2,DESK_CLIP_EMPTY,0,0,NULL,0));
    assert(!raw(2,DESK_CLIP_PUT,13,0,"z\0\0",4) && !raw(2,DESK_CLIP_CLOSE,0,0,NULL,0));
    /* Availability may lag a queued notification; actual Paste is fresh. */
    assert(!du_clip_available(13));
    assert(!strcmp(du_system_clipboard_text_get(),"z"));
    du_clip_observe(store.sequence); assert(du_clip_available(13));
    watch_ack=0; du_clip_bind(transport,&store);
}
static void test_dib(void)
{
    uint32_t rgb[]={0xff0000,0x00ff00,0x0000ff,0x123456,0x765432,0xffffff}; void *dib=NULL; uint32_t n; unsigned w,h; uint32_t *pixels=NULL;
    assert(!clip_dib_encode(rgb,3,3,2,&dib,&n)); assert(n==64);
    assert(!clip_dib_decode(dib,n,&pixels,&w,&h) && w==3 && h==2 && !memcmp(rgb,pixels,sizeof(rgb))); free(pixels);
    assert(clip_dib_decode(dib,n-1,&pixels,&w,&h)==-EINVAL); free(dib);
    unsigned char indexed[52]={0}; desk_put32(indexed,40); desk_put32(indexed+4,2); desk_put32(indexed+8,1); indexed[12]=1; indexed[14]=1; indexed[46]=255; indexed[48]=0x40;
    assert(!clip_dib_decode(indexed,sizeof(indexed),&pixels,&w,&h)); assert(!pixels[0] && pixels[1]==0xff0000); free(pixels);
    unsigned char bitfields[56]={0}; desk_put32(bitfields,40); desk_put32(bitfields+4,1); desk_put32(bitfields+8,1); bitfields[12]=1; bitfields[14]=16; desk_put32(bitfields+16,3);
    desk_put32(bitfields+40,0xf800); desk_put32(bitfields+44,0x7e0); desk_put32(bitfields+48,31); bitfields[53]=0xf8;
    assert(!clip_dib_decode(bitfields,56,&pixels,&w,&h) && pixels[0]==0xff0000); free(pixels);
    desk_put32(bitfields+44,0xf800); assert(clip_dib_decode(bitfields,56,&pixels,&w,&h)==-EINVAL);
}
static void test_files_and_clp(void)
{
    static struct fm_model m,other; m.clipboard=calloc(2,sizeof(char *)); assert(m.clipboard);
    m.clipboard[0]=strdup("/Documents/Ciuki.txt"); m.clipboard[1]=strdup("/mnt/d/Caff\xc3\xa8.txt"); m.clipboard_count=2; m.cut=1;
    assert(!fm_system_clipboard_save(&m)); client=2; assert(!fm_system_clipboard_load(&other));
    assert(other.cut && other.clipboard_count==2 && !strcmp(other.clipboard[1],m.clipboard[1]));
    void *file=NULL; uint32_t n; assert(!du_clip_export(&file,&n));
    assert(((unsigned char *)file)[0]==0x50 && ((unsigned char *)file)[1]==0xc3);
    assert(!du_system_clipboard_text_set("replacement",11));
    assert(!fm_system_clipboard_consume(&other)); assert(!strcmp(du_system_clipboard_text_get(),"replacement"));
    assert(!du_clip_import(file,n)); assert(!fm_system_clipboard_load(&other) && other.cut && other.clipboard_count==2);
    uint32_t before=store.sequence; ((unsigned char *)file)[6]=0xff; ((unsigned char *)file)[7]=0xff;
    assert(du_clip_import(file,n)==-EINVAL && before==store.sequence); free(file);
    assert(!fm_system_clipboard_consume(&other)); assert(!fm_system_clipboard_load(&other) && !other.clipboard_count);
    fm_clipboard_clear(&m); fm_clipboard_clear(&other);
}
static unsigned native_render,native_destroy,native_all,native_locale;
static void native_producer(void *ctx,unsigned event,unsigned format)
{
    assert(ctx==&store);
    if (event==DESK_CLIP_RENDER) {
        if (format==16) { unsigned char locale[]={9,4,0,0}; assert(!du_clip_put(16,locale,4)); native_locale++; }
        else { native_render++; assert(!du_clip_put(format,"native delayed",15)); }
    }
    else if (event==DESK_CLIP_DESTROY) native_destroy++;
    else if (event==DESK_CLIP_RELEASE) { native_all++; assert(!du_clip_open()); assert(!du_clip_put(7,"exit",5)); assert(!du_clip_close()); }
}
static void test_g5b(void)
{
    du_clip_bind(transport,&store); client=71;
    int first=du_clip_register("\xc3\x89" "cole \xce\xa3 \xd0\x94 \xf0\x90\x90\x80");
    assert(first>0 && du_clip_register("\xc3\xa9" "COLE \xcf\x83 \xd0\xb4 \xf0\x90\x90\xa8")==first);
    assert(du_clip_register("Stra\xc3\x9f" "e")==du_clip_register("STRASSE"));
    du_clip_producer_bind(native_producer,&store);
    assert(!du_clip_open() && !du_clip_empty() && !du_clip_delay(7) && !du_clip_delay(16) && !du_clip_close());
    void *data=NULL; uint32_t bytes=0; char *decoded=NULL;
    assert(!du_clip_open() && !du_clip_get(13,&data,&bytes));
    assert(native_render==1 && native_locale==1 && bytes==30 && !clip_text_decode(data,bytes,&decoded) && !strcmp(decoded,"native delayed"));
    free(decoded); free(data); assert(!du_clip_close());
    assert(!du_clip_open() && !du_clip_empty() && !du_clip_delay(7) && !du_clip_close());
    du_clip_release(); assert(native_all==1 && !store.owner);
    client=72; assert(!du_clip_open() && !du_clip_get(7,&data,&bytes) && !strcmp(data,"exit")); free(data); assert(!du_clip_close());
    struct desk_clip_wire event={.kind=DESK_CLIP_DESTROY}; du_clip_event(&event); assert(native_destroy==1);
    assert(!du_clip_open() && !du_clip_empty());
    unsigned char dib[44]={0}; desk_put32(dib,40); desk_put32(dib+4,1); desk_put32(dib+8,1); dib[12]=1; dib[14]=24;
    assert(!raw(72,DESK_CLIP_PUT,2,DESK_CLIP_DELAYED,NULL,0));
    struct desk_clip_wire w={.op=DESK_CLIP_GET,.format=8},r; void *out=NULL;
    assert(clip_request(&store,72,&w,NULL,&r,&out)==-EAGAIN && r.format==2);
    assert(!raw(72,DESK_CLIP_PUT,2,DESK_CLIP_RENDERED,dib,sizeof(dib)));
    assert(!clip_request(&store,72,&w,NULL,&r,&out) && r.bytes==sizeof(dib)); free(out);
    assert(!raw(72,DESK_CLIP_PUT,0x80,DESK_CLIP_DELAYED,NULL,0));
    assert(!du_clip_put(9,"palette",8) && !du_clip_put(3,"metafile",9) && !du_clip_put(14,"enhanced",9));
    assert(!du_clip_close()); clip_disconnect(&store,72);
    client=73; assert(!du_clip_query(&r)); int ownerdisplay=0,palette=0;
    for (unsigned i=0;i<r.count;i++) { if (r.formats[i]==0x80) ownerdisplay++; if (r.formats[i]==9) palette++; }
    assert(!ownerdisplay && palette);
    assert(!du_clip_open() && !du_clip_get(9,&data,&bytes) && !strcmp(data,"palette")); free(data);
    assert(!du_clip_empty() && !du_clip_close());
    du_clip_bind(NULL,NULL);
    puts("PASS G5b Unicode registered names, native delayed/self rendering and render-all, delayed bitmap alias, owner-display exit and rendered handle payload lifetime");
}
int main(void)
{
    clip_init(&store); test_g5b(); clip_destroy(&store);

    client=1; clip_init(&store); du_clip_bind(transport,&store);
    test_protocol(); test_text(); test_delayed_and_limits(); test_menu_cache(); test_dib(); test_files_and_clp();
    du_clip_bind(NULL,NULL); clip_destroy(&store);
    puts("PASS G5 protocol IDs, descriptors, cross-client native/ANSI/Unicode/OEM+locale, exclusive access, owner death, delayed rendering, quotas, DIB, CF_HDROP move sequence, CLP round-trip and malformed files"); fflush(stdout);
    return 0;
}
