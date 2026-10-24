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
static struct sound_scheme named;
static int have_named, validate_error;
static int validate(void *c,const char *path) { (void)c; assert(path[0]=='/'); return validate_error; }
static int schemes(void *c,char names[][SOUND_SCHEME_NAME],unsigned count)
{ (void)c; assert(count>0); if(have_named) strcpy(names[0],"Arcade"); return have_named; }
static int scheme(void *c,unsigned op,const char *name,struct sound_scheme *s)
{
    (void)c; if(strcmp(name,"Arcade")) return -1;
    if(op==SOUND_SCHEME_SAVE) { named=*s; have_named=1; }
    else if(op==SOUND_SCHEME_LOAD) { if(!have_named)return -1; *s=named; }
    else have_named=0;
    return 0;
}
static unsigned mm_open;
static unsigned cd_volume=75;
static int cd_line(void *ctx,struct ciuki_mixer_line *line,int set) {
    (void)ctx; assert(line->line==CIUKI_MIXER_CD);
    if (set) cd_volume=line->left;
    else { line->available=1; line->left=line->right=cd_volume; }
    return 0;
}
static int multimedia(void *c,unsigned which) { (void)c; mm_open=which+1; return 0; }
static int capture_caps(void *c,struct ciuki_audio_capture *out)
{ (void)c; out->available=1; return 0; }
int main(int argc,char **argv)
{
    assert(argc==2); static struct cp_app a; static struct du_ui u; static uint32_t pixels[CP_WIDTH*CP_HEIGHT];
    strcpy(saved,"enabled=1\nerror=custom.wav\n");saved_n=strlen(saved);
    a.host=(struct cp_host){.mixer=mixer,.apply=apply,.load_sounds=load,.save_sounds=save,.audio_diagnostics=diagnostics,.sound_validate=validate,.sound_schemes=schemes,.sound_scheme=scheme,.multimedia_open=multimedia,.capture_caps=capture_caps,.audio_line=cd_line};
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
    ev=(struct du_event){.type=DU_EV_ACTIVATE,.id=find(&u,"Browse...")->id}; u.event(&u,&ev,u.user);
    assert(u.dialog.kind==DU_DLG_OPEN); int browse=u.dialog.id; du_dialog_close(&u);
    ev=(struct du_event){.type=DU_EV_DIALOG,.id=browse,.index=0,.text="/Documents/My Sound.WAV"};
    validate_error=-1; u.event(&u,&ev,u.user); assert(!strcmp(a.sounds.file[SOUND_ERROR],"error.wav"));
    validate_error=0; u.event(&u,&ev,u.user); assert(!strcmp(a.sounds.file[SOUND_ERROR],ev.text));
    failed_save=1; ev.text="/Documents/Other.wav"; u.event(&u,&ev,u.user);
    assert(!strcmp(a.sounds.file[SOUND_ERROR],"/Documents/My Sound.WAV")); failed_save=0;
    ev=(struct du_event){.type=DU_EV_ACTIVATE,.id=find(&u,"Save As...")->id};u.event(&u,&ev,u.user);
    assert(u.dialog.kind==DU_DLG_INPUT); int save_id=u.dialog.id;du_dialog_close(&u);
    ev=(struct du_event){.type=DU_EV_DIALOG,.id=save_id,.index=0,.text="Arcade"};u.event(&u,&ev,u.user);
    assert(have_named && a.sound_name_count==1 && !strcmp(a.sound_named,"Arcade"));
    ev=(struct du_event){.type=DU_EV_ACTIVATE,.id=find(&u,"None")->id};u.event(&u,&ev,u.user);
    assert(!a.sounds.file[SOUND_ERROR][0] && !a.sound_named[0]);
    schemes=NULL;for(unsigned i=0;i<u.count;i++)if(u.widgets[i].items==a.scheme_items)schemes=&u.widgets[i];assert(schemes);
    ev=(struct du_event){.type=DU_EV_SELECT,.id=schemes->id,.index=3};u.event(&u,&ev,u.user);
    assert(!strcmp(a.sounds.file[SOUND_ERROR],"/Documents/My Sound.WAV"));
    ev=(struct du_event){.type=DU_EV_ACTIVATE,.id=find(&u,"Delete")->id};u.event(&u,&ev,u.user);
    assert(!have_named && !a.sound_name_count && !strcmp(a.sounds.file[SOUND_ERROR],"/Documents/My Sound.WAV"));
    cp_open(&a,CP_MULTIMEDIA); assert(cp_page_index("multimedia")==CP_MULTIMEDIA);
    assert(find(&u,"Preferred recording: system capture device (microphone / line in)"));
    ev=(struct du_event){.type=DU_EV_ACTIVATE,.id=find(&u,"Sound Recorder")->id};u.event(&u,&ev,u.user);assert(mm_open==2);
    struct du_widget *cd_slider=NULL;
    for (unsigned i=0;i<u.count;i++) if (u.widgets[i].kind==DU_SCROLLBAR) cd_slider=&u.widgets[i];
    assert(cd_slider && !cd_slider->disabled && cd_slider->value==75);
    cd_slider->value=40; ev=(struct du_event){.type=DU_EV_CHANGE,.id=cd_slider->id};u.event(&u,&ev,u.user);assert(cd_volume==40);
    cp_detach(&a);puts("PASS Sounds page: events/assignments, preview, hardware request, enable persistence/failure, render");
}
