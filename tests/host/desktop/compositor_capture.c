/* Deterministic production-compositor fixture; never guest evidence.
 * usage: capture portrait.xrgb out.xrgb mode [WIDTHxHEIGHT]
 * Modes plain, shell and about are pinned by test_desktop.py and
 * test_normal_boot.py at 640x480; the others render the desktop shell for
 * review (start, desktop, wall=<spec>, about-general, about-system, about-credits, run,
 * shutdown, context, volume, switcher, documents, settings).
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void programs(struct desktop *d)
{
    static const char *const names[] = {"ciuknote", "control", "demo", "dosbox", "files", "hello", "libc_smoke", "lua", "tasks"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        strcpy(d->programs[i].name, names[i]);
        snprintf(d->programs[i].path, sizeof(d->programs[i].path), "/bin/%s", names[i]);
    }
    d->program_count = sizeof(names) / sizeof(names[0]);
}
static void hover(struct desktop *d, int32_t x, int32_t y)
{
    struct ciuki_input_event e = {.type = CIUKI_INPUT_MOTION, .value = x - d->mouse_x, .value2 = y - d->mouse_y};
    desk_input(d, &e);
}
static void press(struct desktop *d, int button)
{
    struct ciuki_input_event e = {.type = CIUKI_INPUT_BUTTON, .code = button, .value = 1};
    desk_input(d, &e); e.value = 0; desk_input(d, &e);
}
static int fake_window(struct desktop *d, int slot, const char *title, uint32_t w, uint32_t h, uint32_t colour, int program)
{
    struct desk_client *c = &d->clients[slot];
    uint32_t *pixels = malloc((size_t)w * h * 4); assert(pixels);
    for (size_t k = 0; k < (size_t)w * h; k++) pixels[k] = colour;
    c->window = 1; c->pixels = pixels; c->info.width = w; c->info.height = h; c->program = program;
    c->window_id = ++d->next_window_id; c->channel = -1; c->internal = 0;
    snprintf(c->title, sizeof(c->title), "%s", title);
    c->frame = desk_frame(w, h, 0, DESK_BAR);
    desk_move(d, slot, 160 + slot * 40, DESK_BAR + 40 + slot * 30);
    desk_focus(d, slot);
    return slot;
}
int main(int argc,char **argv)
{
    assert(argc==4 || argc==5);
    unsigned W=640, H=480;
    if (argc==5) assert(sscanf(argv[4],"%ux%u",&W,&H)==2 && W>=320 && H>=240 && W<=2048 && H<=2048);
    struct desktop *d=calloc(1,sizeof(*d)); assert(d);
    desk_init(d,&fake_ops,W,H);
    d->now_ns=12345000000000ull; d->realtime_second=43210;
    d->mouse_x=(int32_t)W/2; d->mouse_y=(int32_t)H/2;
    uint32_t *portrait=malloc(256*256*4); assert(portrait);
    FILE *f=fopen(argv[1],"rb"); assert(f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    d->portrait=portrait;
    uint32_t *guarded=calloc((size_t)W*H+2,4); assert(guarded);
    guarded[0]=guarded[(size_t)W*H+1]=0xfeedface; d->pixels=guarded+1;
    const char *mode=argv[3];
    if (!strcmp(mode,"shell")) {
        desk_shell_enable(d);
        strcpy(d->programs[0].name,"demo"); strcpy(d->programs[0].path,"/bin/demo");
        d->program_count=1; d->menu_open=1;
    } else if (!strcmp(mode,"about")) {
        desk_shell_enable(d); strcpy(d->build_identity,"f2-30-host-preview"); desk_about(d);
    } else if (strcmp(mode,"plain")) {
        /* Review renders: a realistic evening on the 1024x768 profiles. */
        programs(d);
        d->realtime_second = 1791763200ull + 20*3600 + 46*60;   /* 2026-10-12 20:46 UTC */
        strcpy(d->build_identity,"host-preview"); strcpy(d->version,"0.9.0-prealpha");
        d->sh.mixer_state=2; d->sh.volume=70;
        d->sh.system_valid=1; d->sh.mem_total_k=511u*1024; d->sh.mem_free_k=480u*1024; d->sh.processes=6; d->sh.uptime_s=754;
        strcpy(d->sh.cpu_name,"i686");
        if (!strncmp(mode,"wall=",5)) { cfg_defaults(&d->sh.cfg); snprintf(d->sh.cfg.wallpaper,sizeof(d->sh.cfg.wallpaper),"%s",mode+5); }
        desk_shell_enable(d);
        desk_shell_wallpaper_update(d);
        if (!strcmp(mode,"start") || !strcmp(mode,"documents") || !strcmp(mode,"settings")) {
            hover(d,40,16); press(d,1);   /* the Ciuki button in the top bar */
            struct desk_box m=desk_menu_box(d);
            int row = !strcmp(mode,"start") ? 0 : !strcmp(mode,"documents") ? 1 : 2;
            hover(d,60,m.y+3+row*SH_START_ROW+14);
            if (!strcmp(mode,"start")) { hover(d,m.x+m.w+40,m.y+3+14); hover(d,m.x+m.w+40,m.y+3+14+SH_SUB_ROW); }
        } else if (!strcmp(mode,"about-general") || !strcmp(mode,"about-system") || !strcmp(mode,"about-credits")) {
            desk_about(d); d->sh.about_tab = !strcmp(mode,"about-general") ? 0 : !strcmp(mode,"about-system") ? 1 : 2;
        } else if (!strcmp(mode,"run")) {
            desk_internal_open(d,DESK_INTERNAL_RUN); strcpy(d->sh.run_text,"lua /system/tests/ciuki-f2.lua"); d->sh.run_length=(unsigned)strlen(d->sh.run_text);
        } else if (!strcmp(mode,"shutdown")) {
            desk_internal_open(d,DESK_INTERNAL_SHUTDOWN);
        } else if (!strcmp(mode,"context")) {
            hover(d,(int32_t)W/2+140,(int32_t)H/2+120); press(d,CIUKI_BUTTON_RIGHT);
        } else if (!strcmp(mode,"volume")) {
            fake_window(d,1,"CiukNote",360,220,0xffffffu,DESK_APP_NOTE);
            struct desk_box t={(int32_t)W-SH_TRAY_W-4,(int32_t)H-DESK_BAR+4,SH_TRAY_W,DESK_BAR-7};
            hover(d,t.x+8,t.y+10); press(d,1);
        } else if (!strcmp(mode,"switcher")) {
            fake_window(d,1,"Files",420,260,0xffffffu,DESK_APP_FILES);
            fake_window(d,2,"CiukNote",360,220,0xffffffu,DESK_APP_NOTE);
            fake_window(d,3,"Task Manager",300,240,0xf0f0f0u,0); d->clients[3].taskman=1;
            struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=0x38,.value=1}; desk_input(d,&e);
            e.code=0x0f; desk_input(d,&e); e.value=0; desk_input(d,&e);
        } else if (!strcmp(mode,"desktop")) {
            fake_window(d,1,"Files",420,260,0xffffffu,DESK_APP_FILES);
            d->sh.icon_selected=0; hover(d,(int32_t)W/2,(int32_t)H/2);
        }
    }
    desk_composite(d,(struct desk_box){0,0,(int32_t)W,(int32_t)H});
    f=fopen(argv[2],"wb"); assert(f && fwrite(d->pixels,4,(size_t)W*H,f)==(size_t)W*H && !fclose(f));
    assert(guarded[0]==0xfeedface && guarded[(size_t)W*H+1]==0xfeedface);
    free(guarded); free(portrait); free(d->sh.wall); free(d);
    return 0;
}
