/* SPDX-License-Identifier: MIT
 * Real shared dialog and native observer boundary, with copied media snapshots. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include "../../../apps/libdeskui/deskui.h"
#pragma clang diagnostic pop
static int places_open(const char *,int,...);
static int places_close_fd(int);
#define DU_NATIVE_MEDIA
#define open places_open
#define close places_close_fd
#define ciuki_devctl places_devctl
#include "../../../apps/libdeskui/dialog.c"
#undef ciuki_devctl
#undef close
#undef open
static unsigned opens, closes, state_reads, volume_reads;
static uint32_t mounted=(1u<<3)|(1u<<4), transport=12;
static int unavailable, changed_mid_snapshot, bad_volume, floppy_present, optical_empty;
static const char *usb_label="CIUKI USB";
static int places_open(const char *path,int flags,...)
{
    assert(!strcmp(path,"/dev/media") && flags==(O_RDONLY|O_CLOEXEC)); opens++;
    if (unavailable) { errno=ENODEV; return -1; }
    return 57;
}
static int places_close_fd(int fd) { assert(fd==57); closes++; return 0; }
int places_devctl(int fd,uint32_t op,void *record,uint32_t bytes)
{
    assert(fd==57); uint32_t *h=record; assert(h[0]==CIUKI_MEDIA_VERSION && h[1]==bytes);
    if (unavailable) { errno=ENODEV; return -1; }
    if (op==MEDIA_GET_STATE) {
        assert(bytes==sizeof(struct ciuki_media_state)); state_reads++;
        struct ciuki_media_state *s=record; s->removable_mask=mounted;
        if (floppy_present) s->type[0]=CIUKI_MEDIA_FLOPPY; /* fx-17b: drive A:, no disk */
        if (optical_empty) s->type[3]=CIUKI_MEDIA_OPTICAL; /* fx-emptydrive: drive D:, no disc */
        for (unsigned d=3;d<26;d++) if (mounted&(1u<<d)) {
            s->type[d]=d==3 ? CIUKI_MEDIA_OPTICAL : CIUKI_MEDIA_USB;
            s->generation[d]=transport;
        }
    } else {
        assert(op==MEDIA_GET_VOLUME && bytes==sizeof(struct ciuki_media_volume)); volume_reads++;
        struct ciuki_media_volume *v=record; assert(v->drive>=3 && v->drive<26 && !v->flags && !v->generation);
        if (bad_volume) { errno=EIO; return -1; }
        v->flags=v->drive==3 ? CIUKI_MEDIA_OPTICAL : CIUKI_MEDIA_REMOVABLE;
        v->generation=87; /* Namespace generation is NOT the transport generation. */
        snprintf(v->label,sizeof(v->label),"%s",v->drive==3 ? "Original tones" : usb_label);
        if (changed_mid_snapshot) { transport++; changed_mid_snapshot=0; }
    }
    return 0;
}
static int list(void *ctx,const char *path,struct du_file_entry *out,unsigned cap,unsigned *count)
{
    (void)ctx; assert(cap);
    if (!strcmp(path,"/Documents/Pictures")) return -EIO;
    if (optical_empty && !strcmp(path,"/mnt/d")) return -ENXIO; /* the kernel's "not ready" */
    *count=1; *out=(struct du_file_entry){0};
    snprintf(out->name,sizeof(out->name),"%s",!strcmp(path,"/mnt/e") ? "Original tone with spaces.mp3" : "Example.txt");
    return 0;
}
static void render(struct du_ui *u,const char *out,const char *name)
{
    du_render(u); char path[1024]; snprintf(path,sizeof(path),"%s/%s.xrgb",out,name);
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(u->canvas.pixels,4,(size_t)u->canvas.width*u->canvas.height,f)==(size_t)u->canvas.width*u->canvas.height); fclose(f);
    snprintf(path,sizeof(path),"%s/%s.size",out,name); f=fopen(path,"w"); assert(f);
    fprintf(f,"%d %d\n",u->canvas.width,u->canvas.height); fclose(f);
}
static void click_place(struct du_ui *u,unsigned index)
{
    struct du_widget *w=&u->dialog.items[F_PLACES]; struct du_rect b=du_list_body(w);
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=b.x+40,.value2=b.y+((int)index-w->scroll)*DU_ROW+8}; du_input(u,&e);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1}; du_input(u,&e);
    e.value=0; du_input(u,&e);
}
int main(int argc,char **argv)
{
    assert(argc==2); static struct du_ui u; static uint32_t pixels[560*420];
    du_init(&u,pixels,560,420); u.focused_window=1;
    assert(!du_file_dialog(&u,1,0,"Open Music","/Documents/Music","",list,NULL));
    struct du_dialog *d=&u.dialog; struct du_dialog_places *p=d->places;
    assert(p && p->count==7 && opens==1 && state_reads==2 && volume_reads==2);
    assert(!strcmp(p->entries[0].label,"Desktop") && !strcmp(p->entries[0].path,"/Documents/Desktop"));
    assert(!strcmp(p->entries[5].label,"Original tones (D:)") && !strcmp(p->entries[6].label,"CIUKI USB (E:)"));
    assert(p->rows[2].selected && !p->rows[1].selected && !p->rows[4].selected);
    assert(d->items[F_PLACES].box.x+d->items[F_PLACES].box.w<d->items[F_LIST].box.x);
    assert(du_next_deadline(&u)==500000000ull);
    render(&u,argv[1],"places-open");
    const struct du_item *rows=d->items[F_PLACES].items;
    d->items[F_PLACES].scroll=1;
    u.now=500000000ull; du_dialog_tick(&u);
    assert(!u.dirty && p->count==7 && d->items[F_PLACES].items==rows && d->items[F_PLACES].scroll==1);
    assert(p->rows[2].selected && p->rows[6].text[0]==p->entries[6].label);
    usb_label="Renamed USB"; u.now=1000000000ull; du_dialog_tick(&u);
    assert(u.dirty==3 && !memcmp(&u.invalid,&d->items[F_PLACES].box,sizeof(u.invalid)));
    assert(!strcmp(p->rows[6].text[0],"Renamed USB (E:)")); du_render(&u);
    usb_label="CIUKI USB"; u.now=1500000000ull; du_dialog_tick(&u); du_render(&u);
    d->items[F_PLACES].scroll=0;
    /* Restore the original test clock after exercising three poll intervals. */
    u.now=0; p->next_poll=500000000ull;
    click_place(&u,6); assert(!strcmp(d->folder,"/mnt/e") && p->rows[6].selected);
    render(&u,argv[1],"places-usb");
    click_place(&u,3); assert(!strcmp(d->folder,"/mnt/e") && p->rows[6].selected); /* failed folder navigation */
    d->focus=F_PLACES; du_dialog_key(&u,DU_KEY_UP); assert(!strcmp(d->folder,"/mnt/d"));
    du_dialog_key(&u,DU_KEY_HOME); assert(!strcmp(d->folder,"/Documents/Desktop"));
    mounted=0; du_tick(&u,500000000ull); assert(p->count==5 && p->rows[0].selected);
    render(&u,argv[1],"places-removed");
    mounted=(1u<<4)|(1u<<25); du_tick(&u,1000000000ull); assert(p->count==7);
    d->focus=F_PLACES; du_dialog_key(&u,DU_KEY_END); assert(!strcmp(d->folder,"/mnt/z"));
    changed_mid_snapshot=1; du_tick(&u,1500000000ull); assert(p->count==5);
    du_tick(&u,1600000000ull); assert(p->count==7);
    bad_volume=1; du_tick(&u,2100000000ull); assert(p->count==5); bad_volume=0;
    unavailable=1; du_tick(&u,2600000000ull); assert(p->count==5 && p->fd==-1 && closes==1);
    unavailable=0; du_tick(&u,3100000000ull); assert(p->count==7 && opens==2);
    du_dialog_close(&u); assert(closes==2 && !d->places && du_next_deadline(&u)==UINT64_MAX);
    /* Save uses the same Places pane, constrained to the small client. */
    du_resize(&u,pixels,400,300); mounted=(1u<<4);
    assert(!du_file_dialog(&u,2,1,"Save Playlist","/mnt/e","tones.m3u",list,NULL));
    assert(d->places->count==6 && d->places->rows[5].selected);
    render(&u,argv[1],"places-save-small");
    du_message(&u,3,"Question","Replace playlist?",0,NULL,0,0,0); assert(closes==3 && !d->places);
    du_dialog_close(&u);
    /* fx-17b: a floppy drive is a Place with or without a diskette. */
    floppy_present=1; mounted=0;
    assert(!du_file_dialog(&u,4,0,"Open","/Documents","",list,NULL));
    assert(d->places->count==6 && !strcmp(d->places->entries[5].label,"3.5 Floppy (A:)") &&
           !strcmp(d->places->entries[5].path,"/mnt/a") && d->places->rows[5].icon16);
    du_dialog_close(&u); floppy_present=0;
    /* fx-emptydrive: an empty CD-ROM drive is a Place (Windows 98 lists the
     * drive without a disc); opening it asks for one and stays put. */
    optical_empty=1; mounted=0;
    assert(!du_file_dialog(&u,5,0,"Open","/Documents","",list,NULL));
    assert(d->places->count==6 && !strcmp(d->places->entries[5].label,"Compact Disc (D:)") &&
           !strcmp(d->places->entries[5].path,"/mnt/d") && d->places->rows[5].icon16);
    click_place(&u,5);
    assert(!strcmp(d->folder,"/Documents") && strstr(FILE_STATE(d)->sub_text,"Please insert a disk into drive D:"));
    render(&u,argv[1],"places-empty-cd");
    du_dialog_close(&u); optical_empty=0; du_release(&u);
    puts("PASS Places: native media labels/generations, floppy A: place, render, mouse/keyboard navigation, hotplug, query errors and close");
    puts("PASS fx-emptydrive Places: empty CD-ROM D: listed as Compact Disc (D:), opening it asks for a disc");
    puts("PASS F2-86 Places: unchanged polls render=0; renamed label invalidates pane only; retained row pointers/selection/scroll");
}
