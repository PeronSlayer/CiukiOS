/* SPDX-License-Identifier: MIT */
#include "../../../apps/music/view.h"
#include "acc_test.h"
#include <errno.h>
static struct music_command last; static unsigned sends, master=65;
static int send(void *ctx, const struct music_command *c) { (void)ctx; last=*c; sends++; return 0; }
static int get(void *ctx, unsigned *v) { (void)ctx; *v=master; return 0; }
static int set(void *ctx, unsigned v) { (void)ctx; master=v; return 0; }
int main(int argc, char **argv)
{
    assert(argc==2); static struct music_app app; static struct du_ui ui; static uint32_t pixels[560*420];
    struct music_ui_backend backend={send,get,set,NULL,NULL}; du_init(&ui,pixels,560,420); music_attach(&app,&ui,&backend);
    du_render(&ui); at_save(argv[1],"music-empty",&ui);
    static struct music_snapshot s={.state=MUSIC_PLAYING,.rate=48000,.position=48000*23,.duration=48000*185,.current=1};
    strcpy(s.playlist.path[0],"/Documents/Music/First tone.wav"); strcpy(s.playlist.path[1],"/Documents/Music/Ciuki theme.ogg");
    strcpy(s.playlist.path[2],"/Documents/Music/Evening.mp3"); s.playlist.count=3;
    music_update(&app,&s); du_render(&ui); at_save(argv[1],"music-playing",&ui);
    music_update(&app,&s); assert(!ui.dirty); /* Identical snapshots are free. */
    s.position++; music_update(&app,&s); assert(!ui.dirty); /* Same second/pixel. */
    struct du_event poll={.type=DU_EV_TICK,.id=M_POLL}; music_event(&ui,&poll,&app);
    assert(!ui.dirty); /* Unchanged mixer polling must not repaint. */
    s.position+=24000; music_update(&app,&s);
    assert(ui.dirty==3 && ui.invalid.y==du_get(&ui,M_SEEK)->box.y &&
           ui.invalid.h==du_get(&ui,M_SEEK)->box.h);
    du_render(&ui);
    static uint32_t partial[560*420]; memcpy(partial,pixels,sizeof(partial));
    ui.dirty=1; du_render(&ui); assert(!memcmp(partial,pixels,sizeof(partial)));
    s.buffering=1; music_update(&app,&s);
    assert(strstr(du_get(&ui,M_STATUS)->text,"Buffering..."));
    assert(ui.dirty==3 && ui.invalid.y==du_get(&ui,M_STATUS)->box.y);
    du_render(&ui); s.buffering=0; s.position=48000*23;

    s.state=MUSIC_LOADING; music_update(&app,&s);
    assert(strstr(du_get(&ui,M_STATUS)->text,"Loading..."));
    du_render(&ui); at_save(argv[1],"music-loading",&ui);
    s.state=MUSIC_PLAYING; music_update(&app,&s);
    at_click_widget(&ui,M_PAUSE); assert(last.action==MUSIC_PAUSE);
    ui.focus=-1; at_tap(&ui,DU_KEY_SPACE); assert(last.action==MUSIC_PAUSE);
    at_tap(&ui,DU_KEY_PGDN); assert(last.action==MUSIC_NEXT);
    at_tap(&ui,DU_KEY_PGUP); assert(last.action==MUSIC_PREV);
    at_tap(&ui,DU_KEY_RIGHT); assert(last.action==MUSIC_SEEK && last.value==48000*28);
    at_tap(&ui,0x1f); assert(last.action==MUSIC_STOP);
    struct du_event open={.type=DU_EV_OPEN,.id=M_LIST,.index=2}; music_event(&ui,&open,&app); assert(last.action==MUSIC_SELECT && last.value==2);
    du_get(&ui,M_VOLUME)->value=40; struct du_event volume={.type=DU_EV_CHANGE,.id=M_VOLUME}; music_event(&ui,&volume,&app);
    volume.type=DU_EV_TICK; volume.id=M_APPLY; music_event(&ui,&volume,&app); assert(master==40);
    s.state=MUSIC_PAUSED; music_update(&app,&s); du_render(&ui); at_save(argv[1],"music-paused",&ui);
    s.state=MUSIC_ERROR; s.error=-ENODEV; strcpy(s.error_path,"/mnt/e/Original tone.mp3");
    music_update(&app,&s); assert(strstr(du_get(&ui,M_STATUS)->text,"Cannot open Original tone.mp3:"));
    du_render(&ui); at_save(argv[1],"music-unavailable",&ui);
    static uint32_t small[420*300]; du_resize(&ui,small,420,300); du_render(&ui); at_save(argv[1],"music-small",&ui);
    assert(du_get(&ui,M_LIST)->box.h>0 && du_get(&ui,M_NEXT)->box.x+du_get(&ui,M_NEXT)->box.w<420);
    memset(s.error_path,'a',sizeof(s.error_path)-1); s.error_path[sizeof(s.error_path)-1]=0;
    music_update(&app,&s);
    assert(strstr(du_get(&ui,M_STATUS)->text,"...:") && du_text_width(du_get(&ui,M_STATUS)->text)<420);
    assert(sends>=7); du_release(&ui); puts("PASS music UI: controls, shortcuts, master volume, resize and renders"); return 0;
}
