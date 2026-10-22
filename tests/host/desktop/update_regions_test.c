/* SPDX-License-Identifier: MIT
 * Production toolkit: separate widgets, clip/present, retained scroll,
 * textarea caret, menus/dialogs, pending damage and worker ownership. */
#include "../../../apps/libdeskui/deskui.h"
#include "../../../apps/libdeskui/textarea.h"
#include "../../../apps/libdeskui/job.h"
#include "resize_transport.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define W 400
#define H 300
static struct du_ui u;
static struct du_window w;
static uint32_t pixels[W*H],front[W*H],reference[W*H];
static int ox,oy;
static unsigned painted;
extern unsigned rt_compared_rows;
static uint64_t now(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)); return (uint64_t)t.tv_sec*1000000000ull+(unsigned)t.tv_nsec; }
static void paint(struct du_ui *ui,void *arg)
{
    (void)arg; struct du_rect r=ui->canvas.clip;
    for (int y=r.y;y<r.y+r.h;y++) for (int x=r.x;x<r.x+r.w;x++) {
        pixels[y*W+x]=(uint32_t)((x+ox+2048)*257+(y+oy+2048)*31)&0xffffff;
        painted++;
    }
}
static void init(void)
{
    memset(pixels,0,sizeof(pixels)); memset(front,0,sizeof(front));
    memset(&w,0,sizeof(w)); ox=oy=0;
    du_init(&u,pixels,W,H); u.paint=paint;
    w.ui=&u; w.back=pixels; w.pixels=front; w.info.width=W; w.info.height=H; w.present_rendered=1;
    du_render(&u); du_window_present(&w); w.damaged=0; w.damage_count=0; painted=0;
}
static void equivalent(const char *path)
{
    memcpy(reference,pixels,sizeof(pixels)); u.dirty=1; du_render(&u);
    if (memcmp(reference,pixels,sizeof(pixels))) fprintf(stderr,"Partial/full mismatch: %s\n",path);
    assert(!memcmp(reference,pixels,sizeof(pixels)));
}
static unsigned area(void)
{ unsigned n=0; for(unsigned i=0;i<u.frame_count;i++)n+=(unsigned)(u.frames[i].w*u.frames[i].h); return n; }
static void regions(void)
{
    init(); uint64_t start=now();
    struct du_rect a={2,40,4,5},b={350,240,3,3};
    du_invalidate(&u,a); du_invalidate(&u,b); du_invalidate(&u,a);
    assert(u.update_count==2 && u.dirty==3);
    du_render(&u); assert(u.frame_count==2 && painted==29 && area()==29);
    rt_compared_rows=0; du_window_present(&w); assert(rt_compared_rows==8);
    equivalent("separate rectangles");
    /* Hover only damages the old/new button, never a whole owner window. */
    u.paint=NULL; u.dirty=1; du_add(&u,DU_BUTTON,1,(struct du_rect){10,50,60,22},"One");
    du_add(&u,DU_BUTTON,2,(struct du_rect){300,240,60,22},"Two"); du_render(&u);
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=20,.value2=60}; du_input(&u,&e); du_render(&u); assert(area()==60*22);
    e.value=310;e.value2=250;du_input(&u,&e);du_render(&u);assert(u.frame_count==2 && area()==2*60*22);
    equivalent("hover");
    du_add(&u,DU_LABEL,3,(struct du_rect){5,10,80,20},"Label");du_add(&u,DU_CANVAS,4,(struct du_rect){100,100,120,120},"");du_render(&u);
    e.value=20;e.value2=15;du_input(&u,&e);du_render(&u); /* leave the button */
    e.value=130;e.value2=130;du_input(&u,&e);assert(!u.dirty);
    e.value=20;e.value2=15;du_input(&u,&e);assert(!u.dirty);
    /* Exhaustion retains coverage without allocating or dropping damage. */
    for(int i=0;i<80;i++)du_invalidate(&u,(struct du_rect){i*4,20+(i%3)*4,2,2});
    assert(u.update_count<=DU_REGIONS); du_render(&u); equivalent("capacity");
    printf("PATH toolkit regions/hover/capacity host_us=%.3f\n",(now()-start)/1000.0);
}
static void scrolling(void)
{
    uint64_t start=now();
    const int shifts[][2]={{0,-7},{0,8},{5,0},{-6,0},{3,-4},{-4,5},{500,0}};
    for (unsigned i=0;i<sizeof(shifts)/sizeof(*shifts);i++) {
        init(); int dx=shifts[i][0],dy=shifts[i][1]; ox-=dx;oy-=dy;
        du_scroll(&u,(struct du_rect){0,0,W,H},dx,dy);
        int retained=abs(dx)<W && abs(dy)<H;
        assert(u.copied_count==(unsigned)retained);
        du_render(&u);
        unsigned expected=retained ? W*H-(W-abs(dx))*(H-abs(dy)) : W*H;
        assert(painted==expected); du_window_present(&w); assert(!memcmp(front,pixels,sizeof(front)));
        equivalent("scroll");
    }
    init(); du_invalidate(&u,(struct du_rect){10,10,20,20}); ox=3;du_scroll(&u,(struct du_rect){0,0,W,H},-3,0);
    assert(!u.copied_count);du_render(&u);equivalent("pending repaint fallback");
    printf("PATH toolkit scroll directions/overshift/pending host_us=%.3f\n",(now()-start)/1000.0);
}
static void list_scrolling(void)
{
    static struct du_item items[40]; static char labels[40][32];
    uint64_t start=now();
    for(int view=0;view<3;view++) {
        init();u.paint=NULL;u.dirty=1;
        struct du_widget *list=du_add(&u,DU_LISTVIEW,1,(struct du_rect){8,24,250,220},"");
        for(unsigned i=0;i<40;i++) { snprintf(labels[i],sizeof(labels[i]),"Item %u",i);items[i]=(struct du_item){.text={labels[i]}}; }
        list->items=items;list->count=40;list->view=(enum du_view)view;list->column_count=1;list->columns[0]=(struct du_column){"Name",220,0};
        du_focus(&u,1);du_render(&u);
        struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=40,.value2=80};du_input(&u,&e);
        e=(struct ciuki_input_event){.type=CIUKI_INPUT_WHEEL,.value=-1};du_input(&u,&e);
        assert(list->scroll || list->hscroll);du_render(&u);equivalent("list scroll forward");
        e.value=1;du_input(&u,&e);du_render(&u);equivalent("list scroll backward");
    }
    printf("PATH toolkit list details/icons/columns scroll host_us=%.3f\n",(now()-start)/1000.0);
}
static void transients(void)
{
    init(); uint64_t start=now();
    struct du_menu_item items[]={{"Action",5,0,0,0}}; struct du_menu menus[]={ {"File",items,1} };
    u.menus=menus;u.menu_count=1;u.dirty=1;du_render(&u);
    du_menu_open(&u,0,0);du_render(&u);equivalent("menu open");
    du_menu_close(&u);du_render(&u);equivalent("menu close");
    du_message(&u,4,"Message","Hello",DU_DLG_ICON_INFO,NULL,0,0,0);du_render(&u);equivalent("message");
    du_dialog_close(&u);du_render(&u);equivalent("message close");
    du_progress(&u,5,"Copy","Copying",1,10);du_render(&u);equivalent("progress open");
    du_progress(&u,5,"Copy","Copying",2,10);du_render(&u);assert(area()<W*H);equivalent("progress change");
    du_dialog_close(&u);du_render(&u);equivalent("progress close");
    printf("PATH toolkit menus/dialogs/progress host_us=%.3f\n",(now()-start)/1000.0);
}
static void caret(void)
{
    init();u.paint=NULL;u.dirty=1;uint64_t start=now();struct du_textarea ta;assert(!du_ta_init(&ta,1024));
    assert(!du_ta_set_text(&ta,"abcd\nefgh\nijkl\n",15));
    struct du_widget *text=du_add(&u,DU_TEXTAREA,10,(struct du_rect){10,30,180,130},"");text->data=&ta;
    du_focus(&u,10);du_render(&u);
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=DU_KEY_RIGHT,.value=1};du_input(&u,&e);du_render(&u);
    assert(area()<=2*DU_TA_CELL_W*DU_TA_CELL_H);equivalent("caret right");
    e.value=0;du_input(&u,&e);e.code=DU_KEY_DOWN;e.value=1;du_input(&u,&e);du_render(&u);equivalent("caret down");
    char lines[500]; size_t n=0;
    for(unsigned i=0;i<25;i++)n+=(size_t)snprintf(lines+n,sizeof(lines)-n,"Line %u with text\n",i);
    assert(!du_ta_set_text(&ta,lines,n));du_invalidate(&u,text->box);du_render(&u);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=30,.value2=60};du_input(&u,&e);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_WHEEL,.value=-1};du_input(&u,&e);assert(ta.top>0);du_render(&u);equivalent("textarea scroll");
    e.value=1;du_input(&u,&e);du_render(&u);equivalent("textarea scroll back");
    ta.line_numbers=1;u.dirty=1;du_render(&u);e.value=-1;du_input(&u,&e);du_render(&u);equivalent("textarea gutter scroll");
    du_ta_free(&ta);u.count=0;printf("PATH toolkit textarea caret host_us=%.3f\n",(now()-start)/1000.0);
}
static void changed_paint(struct du_ui *ui,void *arg) { (void)arg;du_fill(&ui->canvas,ui->canvas.clip,0x123456); }
static void queued_damage(void)
{
    struct du_ui client; struct du_window window; rt_reset();
    assert(!du_window_open(&window,&client,3,W,H,"Regions")); window.configured=1;
    assert(du_window_turn(&window,0)>0); struct ciuki_message m;
    while(rt_pop(&m)) for(unsigned i=0;i<m.fd_count;i++)rt_close(m.fds[i]);
    client.paint=changed_paint;
    for(int i=0;i<32;i++) { int x=2+i*9,y=40;du_invalidate(&client,(struct du_rect){x,y,1,1}); }
    assert(du_window_turn(&window,1)>0 && window.damaged && window.damage_count==16);
    unsigned count=0;
    while(rt_pop(&m)) if(m.data[1]==DESK_DAMAGE) { assert(desk_get32(m.data+12)==1 && desk_get32(m.data+16)==1); count++; }
    assert(du_window_turn(&window,2)>0 && !window.damaged);
    while(rt_pop(&m)) if(m.data[1]==DESK_DAMAGE) { assert(desk_get32(m.data+12)==1 && desk_get32(m.data+16)==1); count++; }
    assert(count==32);du_window_destroy(&window);rt_reset();
}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static int entered,released,result;
static void work(void *arg)
{
    (void)arg;pthread_mutex_lock(&gate);entered=1;pthread_cond_signal(&condition);
    while(!released)pthread_cond_wait(&condition,&gate);
    pthread_mutex_unlock(&gate);result=42;
}
static void jobs(void)
{
    struct du_job job={0};assert(!du_job_start(&job,work,NULL));
    pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
    assert(du_job_start(&job,work,NULL)==-EBUSY);assert(!du_job_ready(&job));
    init(); for(unsigned i=0;i<100;i++)du_tick(&u,(uint64_t)i*10000000);assert(!u.dirty && !du_job_ready(&job));
    pthread_mutex_lock(&gate);released=1;pthread_cond_signal(&condition);pthread_mutex_unlock(&gate);
    for(unsigned i=0;i<1000 && !du_job_ready(&job);i++)nanosleep(&(struct timespec){.tv_nsec=1000000},NULL);
    assert(du_job_ready(&job));du_job_finish(&job);du_job_finish(&job);assert(result==42);
    pthread_cond_destroy(&condition);pthread_mutex_destroy(&gate);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);regions();scrolling();list_scrolling();transients();caret();queued_damage();jobs();
    puts("PASS F2-98a update regions: clipped present, scroll, hover, transients, caret, bounded overflow, worker ownership");
    return 0;
}
