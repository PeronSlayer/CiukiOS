/* SPDX-License-Identifier: MIT */
#include "view.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static void layout(struct du_ui *u, void *ctx)
{
    (void)ctx; int w=u->canvas.width,h=u->canvas.height;
    du_get(u,M_TITLE)->box=(struct du_rect){12,28,w-24,20};
    du_get(u,M_TIME)->box=(struct du_rect){12,53,w-24,18};
    du_get(u,M_SEEK)->box=(struct du_rect){12,77,w-24,18};
    int ids[]={M_PREV,M_PLAY,M_PAUSE,M_STOP,M_NEXT};
    for (unsigned i=0;i<5;i++) du_get(u,ids[i])->box=(struct du_rect){12+(int)i*78,104,74,24};
    du_get(u,M_LEVEL)->box=(struct du_rect){12,139,110,18};
    du_get(u,M_VOLUME)->box=(struct du_rect){130,139,w-142,18};
    du_get(u,M_LIST)->box=(struct du_rect){12,170,w-24,h-202};
    du_get(u,M_LIST)->columns[0].width=w-44;
    du_get(u,M_STATUS)->box=(struct du_rect){0,h-22,w,22};
}
static void send(struct music_app *a, enum music_action action, const char *path, uint64_t value)
{
    struct music_command c={.action=action,.value=value};
    if (path) {
        if (strlen(path)>=sizeof(c.path)) { a->local_error=-ENAMETOOLONG; return; }
        strcpy(c.path,path);
    }
    a->local_error=a->backend->send(a->backend->ctx,&c);
    if (!a->local_error && (action==MUSIC_OPEN || action==MUSIC_SELECT || action==MUSIC_NEXT || action==MUSIC_PREV)) {
        a->snapshot.state=MUSIC_LOADING; a->snapshot.error=0;
        music_update(a,&a->snapshot);
    }
}
static void text_changed(struct du_ui *u, int id, const char *text)
{
    struct du_widget *w=du_get(u,id);
    if (strcmp(w->text,text)) { du_set_text(w,text); du_invalidate(u,w->box); }
}
static int seek_pixel(const struct du_widget *w, int value)
{
    /* Match the horizontal scrollbar's track and minimum thumb geometry. */
    int arrow=w->box.h; if (2*arrow>w->box.w) arrow=w->box.w/2;
    int track=w->box.w-2*arrow, thumb=track*w->page/w->total;
    if (thumb<8) thumb=track<8 ? track : 8;
    return (track-thumb)*value/(w->total-w->page);
}
void music_update(struct music_app *a, const struct music_snapshot *s)
{
    a->snapshot=*s; struct du_ui *u=a->ui;
    char text[DU_TEXT_MAX]; const char *name=s->playlist.count ? acc_basename(s->playlist.path[s->current]) : "Music Player";
    text_changed(u,M_TITLE,name);
    uint64_t seconds=s->rate ? s->position/s->rate : 0, total=s->rate ? s->duration/s->rate : 0;
    snprintf(text,sizeof(text),"%llu:%02llu / %llu:%02llu",(unsigned long long)(seconds/60),(unsigned long long)(seconds%60),
             (unsigned long long)(total/60),(unsigned long long)(total%60));
    if (!s->duration && s->state==MUSIC_PLAYING) snprintf(text,sizeof(text),"%llu:%02llu / --:--",
        (unsigned long long)(seconds/60),(unsigned long long)(seconds%60));
    text_changed(u,M_TIME,text);
    struct du_widget *seek=du_get(u,M_SEEK);
    if (seek->disabled!=!s->duration) { seek->disabled=!s->duration; du_invalidate(u,seek->box); }
    if (u->capture<0 && !a->seek_waiting) {
        int value=s->duration ? (int)(s->position*1000/s->duration) : 0;
        if (seek_pixel(seek,seek->value)!=seek_pixel(seek,value)) du_invalidate(u,seek->box);
        seek->value=value;
    }
    struct du_widget *list=du_get(u,M_LIST);
    if (list->count!=s->playlist.count || a->list_revision!=s->revision || a->list_current!=s->current) {
        for (unsigned i=0;i<s->playlist.count;i++) {
            a->rows[i].text[0]=acc_basename(a->snapshot.playlist.path[i]);
            a->rows[i].selected=(uint8_t)(i==s->current);
        }
        list->items=a->rows; list->count=s->playlist.count;
        a->list_revision=s->revision; a->list_current=s->current;
        du_invalidate(u,list->box);
    }
    if (list->active>=list->count) list->active=0;
    int error=a->local_error ? a->local_error : s->error;
    if (error) {
        const char *file=acc_basename(s->error_path[0] ? s->error_path : name);
        const char *reason=error==-EINVAL ? "Unsupported file or invalid playlist" : strerror(-error);
        char suffix[200]; snprintf(suffix,sizeof(suffix),"...: %s",reason);
        int room=u->canvas.width-16-du_text_width("Cannot open ")-du_text_width(suffix);
        size_t fit=desk_text_fit_chars(desk_theme->font_ui,file,room>0 ? room : 0);
        snprintf(text,sizeof(text),"Cannot open %.*s%s: %s",(int)fit,file,file[fit] ? "..." : "",reason);
    }
    else snprintf(text,sizeof(text),"%s   %u tracks   Space: play/pause, S: stop, PgUp/PgDn: previous/next",
                  s->buffering ? "Buffering..." : s->state==MUSIC_PLAYING ? "Playing" : s->state==MUSIC_PAUSED ? "Paused" : s->state==MUSIC_LOADING ? "Loading..." :
                  s->state==MUSIC_CLOSING ? "Closing" : "Stopped",s->playlist.count);
    text_changed(u,M_STATUS,text);
    snprintf(text,sizeof(text),a->volume_error ? "No mixer" : "Master: %d%%",a->volume);
    text_changed(u,M_LEVEL,text);
}
static void volume(struct music_app *a)
{
    unsigned value=0; int r=a->backend->master_get ? a->backend->master_get(a->backend->ctx,&value) : -ENODEV;
    a->volume_error=r;
    struct du_widget *w=du_get(a->ui,M_VOLUME);
    if (!r && !a->waiting && a->ui->capture<0) {
        a->volume=(int)value;
        if (w->value!=a->volume) { w->value=a->volume; du_invalidate(a->ui,w->box); }
    }
    if (w->disabled!=(r!=0)) { w->disabled=r!=0; du_invalidate(a->ui,w->box); }
    char text[64]; snprintf(text,sizeof(text),r ? "No mixer" : "Master: %d%%",a->volume);
    text_changed(a->ui,M_LEVEL,text);
}
void music_attach(struct music_app *a, struct du_ui *u, const struct music_ui_backend *backend)
{
    memset(a,0,sizeof(*a)); a->ui=u; a->backend=backend; u->user=a; u->event=music_event; u->layout=layout;
    struct du_menu_item file[]={
        {"&Open...",M_OPEN,0x18,DU_CTRL,0},{"&Add Files...",M_ADD,0,0,0},{"Add &Folder...",M_FOLDER,0,0,0},
        {"&Save Playlist...",M_SAVE,0x1f,DU_CTRL,0},{"&Clear Playlist",M_CLEAR,0,0,0},{"E&xit",M_EXIT,0,0,0}};
    struct du_menu_item play[]={ {"&Play",M_PLAY,0,0,0},{"&Pause",M_PAUSE,0,0,0},{"&Stop",M_STOP,0,0,0},
        {"P&revious",M_PREV,DU_KEY_PGUP,0,0},{"&Next",M_NEXT,DU_KEY_PGDN,0,0}};
    memcpy(a->file_items,file,sizeof(file)); memcpy(a->play_items,play,sizeof(play));
    a->help_items[0]=(struct du_menu_item){"&About Music Player",M_ABOUT,0,0,0};
    a->menus[0]=(struct du_menu){"&File",a->file_items,6}; a->menus[1]=(struct du_menu){"&Playback",a->play_items,5};
    a->menus[2]=(struct du_menu){"&Help",a->help_items,1}; u->menus=a->menus; u->menu_count=3;
    struct du_rect zero={0};
    du_add(u,DU_LABEL,M_TITLE,zero,""); du_add(u,DU_LABEL,M_TIME,zero,"");
    struct du_widget *seek=du_add(u,DU_SCROLLBAR,M_SEEK,zero,""); seek->vertical=0; seek->total=1010; seek->page=10;
    const char *labels[]={"< &Prev","&Play","Pa&use","&Stop","&Next >"}; int ids[]={M_PREV,M_PLAY,M_PAUSE,M_STOP,M_NEXT};
    for (unsigned i=0;i<5;i++) du_add(u,DU_BUTTON,ids[i],zero,labels[i]);
    du_add(u,DU_LABEL,M_LEVEL,zero,""); struct du_widget *v=du_add(u,DU_SCROLLBAR,M_VOLUME,zero,""); v->vertical=0; v->total=110; v->page=10;
    struct du_widget *list=du_add(u,DU_LISTVIEW,M_LIST,zero,""); list->view=DU_DETAILS; list->inactive_selection=1;
    list->column_count=1; list->columns[0]=(struct du_column){"Playlist",360,0};
    du_add(u,DU_STATUS,M_STATUS,zero,""); layout(u,a); music_update(a,&a->snapshot); volume(a); u->focus=-1;
    du_timer_start(u,M_POLL,1000000000ull,1);
}
void music_event(struct du_ui *u, const struct du_event *e, void *ctx)
{
    struct music_app *a=ctx;
    if (e->type==DU_EV_CONSUMER) {
        if (e->key==CIUKI_CONSUMER_PLAY_PAUSE) send(a,a->snapshot.state==MUSIC_PLAYING ? MUSIC_PAUSE : MUSIC_PLAY,NULL,0);
        else if (e->key==CIUKI_CONSUMER_NEXT) send(a,MUSIC_NEXT,NULL,0);
        else if (e->key==CIUKI_CONSUMER_PREVIOUS) send(a,MUSIC_PREV,NULL,0);
        else return;
    } else if (e->type==DU_EV_TICK) {
        if (e->id==M_POLL) volume(a);
        if (e->id==M_SEEK_APPLY && a->seek_waiting) { a->seek_waiting=0; send(a,MUSIC_SEEK,NULL,a->seek_target); }
        if (e->id==M_APPLY) {
            a->waiting=0; a->local_error=a->backend->master_set ? a->backend->master_set(a->backend->ctx,(unsigned)a->volume) : -ENODEV;
        }
    } else if (e->type==DU_EV_DIALOG && !e->index && e->text && *e->text) {
        if (e->id==M_D_OPEN) send(a,MUSIC_OPEN,e->text,0);
        else if (e->id==M_D_ADD || e->id==M_D_FOLDER) send(a,MUSIC_ADD,e->text,0);
        else if (e->id==M_D_SAVE) send(a,MUSIC_SAVE,e->text,0);
    } else if (e->type==DU_EV_DROPFILES) {
        /* fx-19 WM_DROPFILES: a dropped song or playlist plays now, as Windows
         * Media Player; further items and folders join the playlist. */
        if (u->dialog.kind) return;
        char path[MUSIC_PATH]; int folder=0, n=du_drop_entry(0,path,sizeof(path),&folder);
        if (n<=0 || !path[0]) return;
        if (!folder) send(a,MUSIC_OPEN,path,0);
        else { send(a,MUSIC_CLEAR,NULL,0); send(a,MUSIC_ADD,path,0); }
        for (int i=1;i<n && i<64 && !a->local_error;i++) if (du_drop_file((unsigned)i,path,sizeof(path))>0 && path[0]) send(a,MUSIC_ADD,path,0);
    } else if (e->type==DU_EV_OPEN && e->id==M_LIST && e->index>=0) send(a,MUSIC_SELECT,NULL,(unsigned)e->index);
    else if (e->type==DU_EV_CHANGE) {
        if (e->id==M_SEEK) {
            a->seek_target=a->snapshot.duration*(unsigned)du_get(u,M_SEEK)->value/1000; a->seek_waiting=1;
            du_timer_start(u,M_SEEK_APPLY,120000000ull,0);
        }
        else if (e->id==M_VOLUME) { a->volume=du_get(u,M_VOLUME)->value; a->waiting=1; du_timer_start(u,M_APPLY,120000000ull,0); }
    } else if (e->type==DU_EV_ACTIVATE) {
        switch (e->id) {
        case M_OPEN: case M_ADD:
            {
                static const struct du_file_filter types[]={{"Music and Playlists (*.wav;*.mp3;*.ogg;*.m3u)","*.wav;*.mp3;*.ogg;*.m3u"},{"All Files (*.*)","*.*"}};
                struct du_file_options o={.flags=DU_FILE_FILE_MUST_EXIST,.title=e->id==M_OPEN ? "Open Music or Playlist" : "Add Music or Playlist",
                    .folder="/Documents/Music",.name="",.filters=types,.filter_count=2};
                du_file_open(u,e->id==M_OPEN ? M_D_OPEN : M_D_ADD,&o);
            } break;
        case M_FOLDER: du_input_box(u,M_D_FOLDER,"Add Folder","Folder path (including subfolders):","/Documents/Music"); break;
        case M_SAVE:
            {
                static const struct du_file_filter types[]={{"Playlists (*.m3u)","*.m3u"},{"All Files (*.*)","*.*"}};
                struct du_file_options o={.flags=DU_FILE_SAVE|DU_FILE_OVERWRITE_PROMPT,.title="Save Playlist",.folder="/Documents/Music",
                    .name="playlist.m3u",.filters=types,.filter_count=2,.default_ext="m3u"};
                du_file_open(u,M_D_SAVE,&o);
            } break;
        case M_CLEAR: send(a,MUSIC_CLEAR,NULL,0); break;
        case M_EXIT: if (a->backend->quit) a->backend->quit(a->backend->ctx); break;
        case M_PLAY: send(a,MUSIC_PLAY,NULL,0); break;
        case M_PAUSE: send(a,MUSIC_PAUSE,NULL,0); break;
        case M_STOP: send(a,MUSIC_STOP,NULL,0); break;
        case M_PREV: send(a,MUSIC_PREV,NULL,0); break;
        case M_NEXT: send(a,MUSIC_NEXT,NULL,0); break;
        case M_ABOUT:
            du_message(u,M_ABOUT,"About Music Player","CiukiOS Music Player - Classic computing, one woof at a time.\n"
                       "WAV PCM, MP3 (minimp3, CC0), Ogg Vorbis (stb_vorbis, MIT).\nSpace: play/pause; S: stop; arrows: seek 5 seconds.\n"
                       "PgUp/PgDn: track; Ctrl+O: open; Ctrl+S: save playlist.",DU_DLG_ICON_INFO,NULL,0,0,0); break;
        default: return;
        }
        u->focus=-1;
    } else if (e->type==DU_EV_KEY) {
        if (e->ch==' ') send(a,a->snapshot.state==MUSIC_PLAYING ? MUSIC_PAUSE : MUSIC_PLAY,NULL,0);
        else if (e->ch=='s' || e->ch=='S') send(a,MUSIC_STOP,NULL,0);
        else if (e->key==DU_KEY_LEFT || e->key==DU_KEY_RIGHT) {
            uint64_t pos=a->snapshot.position, step=(uint64_t)a->snapshot.rate*5;
            send(a,MUSIC_SEEK,NULL,e->key==DU_KEY_RIGHT ? pos+step : pos>step ? pos-step : 0);
        } else return;
    } else return;
    music_update(a,&a->snapshot);
}
