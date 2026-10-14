/* SPDX-License-Identifier: MIT */
#include "control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char saved[SOUND_TEXT_MAX]; static size_t saved_n; static unsigned request; static int arg, failed_save;
static int load(void *c,char *text,size_t cap,size_t *n) { (void)c;assert(saved_n<cap);memcpy(text,saved,saved_n);*n=saved_n;return 0; }
static int save(void *c,const char *text,size_t n) { (void)c;if(failed_save)return -1;memcpy(saved,text,n);saved_n=n;return 0; }
static int apply(void *c,unsigned r,int v,int m) { (void)c;(void)m;request=r;arg=v;return 0; }
static int master_level=70, master_mute;
static int mixer(void *c,int *v,int *m) { (void)c;*v=master_level;*m=master_mute;return 0; }
static unsigned sound_event_seen;
static struct ciuki_audio_diagnostics diag;
static int diag_error;
static int diagnostics(void *c,struct ciuki_audio_diagnostics *out)
{ (void)c;*out=diag;return diag_error; }
static void sound_event(void *ctx,unsigned e) { (void)ctx;sound_event_seen=e; }
static struct du_widget *find(struct du_ui *u,const char *text)
{ for(unsigned i=0;i<u->count;i++) if(!strcmp(u->widgets[i].text,text))return &u->widgets[i];assert(0);return NULL; }
int main(int argc,char **argv)
{
    assert(argc==2); static struct cp_app a; static struct du_ui u; static uint32_t pixels[CP_WIDTH*CP_HEIGHT];
    strcpy(saved,"enabled=1\nerror=custom.wav\n");saved_n=strlen(saved);
    a.host=(struct cp_host){.mixer=mixer,.apply=apply,.load_sounds=load,.save_sounds=save,.audio_diagnostics=diagnostics};
    diag=(struct ciuki_audio_diagnostics){.version=1,.size=64,.codec_id=0x43525931,.controller=0x80862485,
        .flags=CIUKI_AUDIO_DIAG_READY,.irq_line=11,.fetched_frames=10,.irqs=1,.power=15,.master=0x0404,.pcm=0x0808,.rate=48000};
    du_init(&u,pixels,CP_WIDTH,CP_HEIGHT);u.focused_window=1;assert(!cp_attach(&a,&u,CP_SOUNDS));
    assert(!strcmp(a.sounds.file[SOUND_ERROR],"custom.wav"));
    master_level=21;master_mute=1;cp_tick(&a);assert(a.volume==21&&a.muted&&u.dirty);
    u.capture=0;master_level=50;cp_tick(&a);assert(a.volume==21); /* do not fight slider capture */
    u.capture=-1;master_level=70;master_mute=0;cp_tick(&a);assert(a.volume==70&&!a.muted);
    struct du_widget *list=NULL;for(unsigned i=0;i<u.count;i++)if(u.widgets[i].items==a.sound_items)list=&u.widgets[i];
    assert(list && !list->hidden && list->count==SOUND_EVENTS && list->column_count==2);
    struct du_event ev={.type=DU_EV_SELECT,.id=list->id,.index=SOUND_ERROR};u.event(&u,&ev,u.user);
    assert(a.sound_selected==SOUND_ERROR);
    ev=(struct du_event){.type=DU_EV_ACTIVATE,.id=find(&u,"Preview")->id};u.event(&u,&ev,u.user);
    assert(request==DESK_SETTINGS_SOUND_TEST && arg==SOUND_ERROR);
    ev.id=find(&u,"Test audio hardware")->id;u.event(&u,&ev,u.user);assert(request==DESK_SETTINGS_AUDIO_TEST && a.audio_testing);
    assert(find(&u,"Test audio hardware")->disabled);
    diag.fetched_frames+=48000;diag.irqs+=187;cp_audio_result(&a,0);assert(!a.audio_testing);
    assert(strstr(u.dialog.text,"Codec: 43525931") && strstr(u.dialog.text,"DMA advanced: yes (48000 frames)") && strstr(u.dialog.text,"IRQs received: yes (187, line 11)"));
    du_dialog_close(&u);
    ev.id=find(&u,"Test audio hardware")->id;u.event(&u,&ev,u.user);cp_audio_result(&a,-5);
    assert(strstr(u.dialog.text,"DMA advanced: no (0 frames)") && strstr(u.dialog.text,"IRQs received: no (0, line 11)"));
    du_dialog_close(&u);
    diag.flags=CIUKI_AUDIO_DIAG_QUARANTINE;diag.last_error=-110;cp_audio_result(&a,-5);
    assert(strstr(u.dialog.text,"Kernel error: -110") && strstr(u.dialog.text,"unavailable, quarantined"));du_dialog_close(&u);
    diag_error=-19;cp_audio_result(&a,-19);assert(strstr(u.dialog.text,"diagnostics unavailable (error -19)"));du_dialog_close(&u);
    diag_error=0;diag.flags=CIUKI_AUDIO_DIAG_READY;diag.last_error=0;
    ev=(struct du_event){.type=DU_EV_CHANGE,.id=find(&u,"Play system sounds")->id};u.event(&u,&ev,u.user);
    assert(!a.sounds.enabled && request==DESK_SETTINGS_SOUND_RELOAD);
    struct sound_scheme scheme;sound_defaults(&scheme);assert(!sound_parse(&scheme,saved,saved_n));
    assert(!scheme.enabled && !strcmp(scheme.file[SOUND_ERROR],"custom.wav"));
    struct du_widget *schemes = NULL, *ui_volume = NULL;
    for (unsigned i=0;i<u.count;i++) {
        if (u.widgets[i].items==a.scheme_items) schemes=&u.widgets[i];
        if (u.widgets[i].kind==DU_SCROLLBAR && u.widgets[i].box.x==270) ui_volume=&u.widgets[i];
    }
    assert(schemes && ui_volume);
    ev=(struct du_event){.type=DU_EV_SELECT,.id=schemes->id,.index=0};u.event(&u,&ev,u.user);
    assert(a.sounds.enabled && !strcmp(a.sounds.file[SOUND_ERROR],"error.wav"));
    ev.index=1;u.event(&u,&ev,u.user);assert(!a.sounds.enabled && schemes->active==1);
    ev=(struct du_event){.type=DU_EV_CHANGE,.id=ui_volume->id,.index=35};u.event(&u,&ev,u.user);
    assert(a.sounds.volume==35 && a.volume==70 && request==DESK_SETTINGS_SOUND_RELOAD);
    sound_defaults(&scheme);assert(!sound_parse(&scheme,saved,saved_n));assert(scheme.volume==35 && !scheme.enabled);
    failed_save=1;u.event(&u,&ev,u.user);assert(!a.sounds.enabled);failed_save=0;
    ev=(struct du_event){.type=DU_EV_SELECT,.id=schemes->id,.index=0};u.event(&u,&ev,u.user);
    cp_audio_result(&a,0);
    du_dialog_close(&u);
    du_render(&u);FILE *f=fopen(argv[1],"wb");assert(f && fwrite(pixels,4,CP_WIDTH*CP_HEIGHT,f)==CP_WIDTH*CP_HEIGHT);fclose(f);
    u.sound=sound_event;
    du_message(&u,100,"Error","Error",DU_DLG_ICON_ERROR,NULL,0,0,0);assert(sound_event_seen==SOUND_ERROR);du_dialog_close(&u);
    du_message(&u,101,"Info","Info",DU_DLG_ICON_INFO,NULL,0,0,0);assert(sound_event_seen==SOUND_INFORMATION);du_dialog_close(&u);
    du_message(&u,102,"Question","Question",DU_DLG_ICON_QUESTION,NULL,0,0,0);assert(sound_event_seen==SOUND_QUESTION);du_dialog_close(&u);
    static const struct du_menu_item menu[]={{.label="Select",.id=500}};
    du_popup(&u,menu,1,20,20,1);assert(sound_event_seen==SOUND_MENU_OPEN);du_menu_close(&u);
    cp_detach(&a);puts("PASS Sounds page: events/assignments, preview, hardware request, enable persistence/failure, render");
}
