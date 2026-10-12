/* SPDX-License-Identifier: MIT */
#include "control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char saved[SOUND_TEXT_MAX]; static size_t saved_n; static unsigned request; static int arg, failed_save;
static int load(void *c,char *text,size_t cap,size_t *n) { (void)c;assert(saved_n<cap);memcpy(text,saved,saved_n);*n=saved_n;return 0; }
static int save(void *c,const char *text,size_t n) { (void)c;if(failed_save)return -1;memcpy(saved,text,n);saved_n=n;return 0; }
static int apply(void *c,unsigned r,int v,int m) { (void)c;(void)m;request=r;arg=v;return 0; }
static int mixer(void *c,int *v,int *m) { (void)c;*v=70;*m=0;return 0; }
static unsigned sound_event_seen;
static void sound_event(void *ctx,unsigned e) { (void)ctx;sound_event_seen=e; }
static struct du_widget *find(struct du_ui *u,const char *text)
{ for(unsigned i=0;i<u->count;i++) if(!strcmp(u->widgets[i].text,text))return &u->widgets[i];assert(0);return NULL; }
int main(int argc,char **argv)
{
    assert(argc==2); static struct cp_app a; static struct du_ui u; static uint32_t pixels[CP_WIDTH*CP_HEIGHT];
    strcpy(saved,"enabled=1\nerror=custom.wav\n");saved_n=strlen(saved);
    a.host=(struct cp_host){.mixer=mixer,.apply=apply,.load_sounds=load,.save_sounds=save};
    du_init(&u,pixels,CP_WIDTH,CP_HEIGHT);u.focused_window=1;assert(!cp_attach(&a,&u,CP_SOUNDS));
    assert(!strcmp(a.sounds.file[SOUND_ERROR],"custom.wav"));
    struct du_widget *list=NULL;for(unsigned i=0;i<u.count;i++)if(u.widgets[i].items==a.sound_items)list=&u.widgets[i];
    assert(list && !list->hidden && list->count==SOUND_EVENTS && list->column_count==2);
    struct du_event ev={.type=DU_EV_SELECT,.id=list->id,.index=SOUND_ERROR};u.event(&u,&ev,u.user);
    assert(a.sound_selected==SOUND_ERROR);
    ev=(struct du_event){.type=DU_EV_ACTIVATE,.id=find(&u,"Test")->id};u.event(&u,&ev,u.user);
    assert(request==DESK_SETTINGS_SOUND_TEST && arg==SOUND_ERROR);
    ev.id=find(&u,"Test audio hardware")->id;u.event(&u,&ev,u.user);assert(request==DESK_SETTINGS_AUDIO_TEST && a.audio_testing);
    assert(find(&u,"Test audio hardware")->disabled);cp_audio_result(&a,0);assert(!a.audio_testing);
    ev=(struct du_event){.type=DU_EV_CHANGE,.id=find(&u,"Play system sounds")->id};u.event(&u,&ev,u.user);
    assert(!a.sounds.enabled && request==DESK_SETTINGS_SOUND_RELOAD);
    struct sound_scheme scheme;sound_defaults(&scheme);assert(!sound_parse(&scheme,saved,saved_n));
    assert(!scheme.enabled && !strcmp(scheme.file[SOUND_ERROR],"custom.wav"));
    failed_save=1;u.event(&u,&ev,u.user);assert(!a.sounds.enabled);failed_save=0;
    cp_audio_result(&a,0);
    du_render(&u);FILE *f=fopen(argv[1],"wb");assert(f && fwrite(pixels,4,CP_WIDTH*CP_HEIGHT,f)==CP_WIDTH*CP_HEIGHT);fclose(f);
    u.sound=sound_event;
    du_message(&u,100,"Error","Error",DU_DLG_ICON_ERROR,NULL,0,0,0);assert(sound_event_seen==SOUND_ERROR);du_dialog_close(&u);
    du_message(&u,101,"Info","Info",DU_DLG_ICON_INFO,NULL,0,0,0);assert(sound_event_seen==SOUND_INFORMATION);du_dialog_close(&u);
    du_message(&u,102,"Question","Question",DU_DLG_ICON_QUESTION,NULL,0,0,0);assert(sound_event_seen==SOUND_QUESTION);du_dialog_close(&u);
    static const struct du_menu_item menu[]={{.label="Select",.id=500}};
    du_popup(&u,menu,1,20,20,1);assert(sound_event_seen==SOUND_MENU_OPEN);du_menu_close(&u);
    cp_detach(&a);puts("PASS Sounds page: events/assignments, preview, hardware request, enable persistence/failure, render");
}
