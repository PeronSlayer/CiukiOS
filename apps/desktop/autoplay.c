/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "autoplay.h"
#include "shell_link.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __i386__
#include <ciuki/raw.h>
#endif
enum { AP_JOB_PROBE=1, AP_JOB_SAVE };
/* One slot remains owned by the worker from submission through collection.
 * Only the frame thread changes shell state. I/O never holds the mailbox. */
struct ap_worker {
    pthread_mutex_t lock; pthread_cond_t changed; pthread_t thread;
    struct desktop *desktop;
    const struct fm_vfs *fs;
    struct ap_workspace *workspace;
    int (*preference)(struct desktop *,const char *,const char *,int);
    struct ciuki_media_volume volume;
    struct ap_content content;
    int job, busy, ready, stopping, action, saved, error;
    const char *activity;
};
static int submit(struct desk_autoplay *,int);
static void activity(struct ap_worker *w,const char *name)
{ __atomic_store_n(&w->activity,name,__ATOMIC_RELEASE); }
static void probe_activity(void *ctx,const char *name) { activity(ctx,name); }
static void budget_log(const struct ap_worker *w)
{
    char line[128]; int n=snprintf(line,sizeof(line),"[desktop] autoplay drive=%c budget=%s entries=%u ms=%u",
        'A'+w->volume.drive,w->content.budget,w->content.entries,w->content.ms);
    if (n<=0 || (size_t)n>=sizeof(line)) return;
#ifdef __i386__
    /* The diagnostic sink also reaches BOOT.LOG, without FILE/VFS locks. */
    (void)ciuki_raw_debug_write((uint32_t)(uintptr_t)line,(uint32_t)n,0,0,0,0);
#else
    fprintf(stderr,"%s\n",line);
#endif
}
static void *autoplay_worker(void *arg)
{
    struct ap_worker *w=arg;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        activity(w,"mailbox-wait");
        while (!w->job && !w->stopping) pthread_cond_wait(&w->changed,&w->lock);
        if (w->stopping) break;
        int job=w->job; w->job=0; w->busy=1;
        pthread_mutex_unlock(&w->lock);
        w->error=0; w->saved=-1;
        if (job==AP_JOB_PROBE) {
            if (ap_probe_workspace(w->fs,w->volume.drive,&w->content,w->workspace)) {
                memset(&w->content,0,sizeof(w->content));
                snprintf(w->content.root,sizeof(w->content.root),"/mnt/%c",'a'+w->volume.drive);
            }
            if (w->content.budget) budget_log(w);
            activity(w,"preference-read");
            if (!w->content.budget && w->volume.id[0] && w->preference)
                w->saved=w->preference(w->desktop,w->volume.id,NULL,-1);
        } else {
            activity(w,"preference-write");
            const char *label=w->content.inf.label[0] ? w->content.inf.label : w->volume.label;
            if (w->preference(w->desktop,w->volume.id,label,w->action)<0) w->error=1;
        }
        pthread_mutex_lock(&w->lock);
        w->busy=0; w->ready=1;
    }
    pthread_mutex_unlock(&w->lock); return NULL;
}
int desk_autoplay_prepare(struct desktop *d)
{
    struct desk_autoplay *a=&d->sh.autoplay;
    if (a->worker || !d->autoplay_fs || d->sh.gate) return 0;
    if (!a->workspace) return ENOMEM;
    struct ap_worker *w=calloc(1,sizeof(*w)); if (!w) return ENOMEM;
    w->desktop=d; w->fs=d->autoplay_fs; w->workspace=a->workspace;
    ap_workspace_observe(w->workspace,probe_activity,w);
    w->preference=d->autoplay_preference; activity(w,"starting");
    int e=pthread_mutex_init(&w->lock,NULL);
    if (e) { free(w); return e; }
    e=pthread_cond_init(&w->changed,NULL);
    if (e) { pthread_mutex_destroy(&w->lock); free(w); return e; }
    e=pthread_create(&w->thread,NULL,autoplay_worker,w);
    if (e) { pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock); free(w); return e; }
    a->worker=w; return 0;
}
int desk_autoplay_busy(struct desktop *d)
{
    struct desk_autoplay *a=&d->sh.autoplay;
    struct ap_worker *w=a->worker;
    if (!w) return 0;
    /* Shutdown stops ordinary service, but an accepted preference still
     * needs submission. Retrying here does not wait for its disk write. */
    if (a->phase==AP_SAVE_PENDING && submit(a,AP_JOB_SAVE)) a->phase=AP_SAVING;
    if (pthread_mutex_trylock(&w->lock)) return 1;
    int busy=w->job || w->busy || a->phase==AP_SAVE_PENDING;
    pthread_mutex_unlock(&w->lock); return busy;
}
void desk_autoplay_stop(struct desktop *d)
{
    struct ap_worker *w=d->sh.autoplay.worker;
    if (!w) return;
    pthread_mutex_lock(&w->lock); w->stopping=1;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread,NULL); pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock);
    ap_workspace_observe(w->workspace,NULL,NULL);
    free(w); d->sh.autoplay.worker=NULL; d->sh.autoplay.phase=AP_IDLE;
}
const char *desk_autoplay_activity(const struct desktop *d)
{
    struct ap_worker *w=d ? d->sh.autoplay.worker : NULL;
    return w ? __atomic_load_n(&w->activity,__ATOMIC_ACQUIRE) : "unavailable";
}
static int submit(struct desk_autoplay *a,int job)
{
    struct ap_worker *w=a->worker;
    if (!w || pthread_mutex_trylock(&w->lock)) return 0;
    int available=!w->job && !w->busy && !w->ready && !w->stopping;
    if (available) {
        w->volume=a->active; w->action=a->selected;
        if (job==AP_JOB_SAVE) w->content=a->content;
        w->job=job; pthread_cond_signal(&w->changed);
    }
    pthread_mutex_unlock(&w->lock); return available;
}
static int take(struct desk_autoplay *a,int *error)
{
    struct ap_worker *w=a->worker;
    if (!w || pthread_mutex_trylock(&w->lock)) return 0;
    int ready=w->ready;
    if (ready) {
        if (a->phase==AP_PROBING) { a->content=w->content; a->saved=w->saved; }
        *error=w->error; w->ready=0;
    }
    pthread_mutex_unlock(&w->lock); return ready;
}
const char *ap_action_name(int action)
{
    static const char *const names[]={"Open folder to view files","Play music","View pictures","Run program","Do nothing"};
    return action>=0 && action<AP_CHOICES ? names[action] : "Ask every time";
}
int desk_autoplay_enabled(const struct desktop *d,int a)
{
    const struct ap_content *c=&d->sh.autoplay.content;
    if (c->cd_audio) return a==AP_MUSIC || a==AP_NOTHING;
    return a==AP_FOLDER || a==AP_NOTHING || (a==AP_MUSIC && c->music[0]) || (a==AP_PICTURES && c->picture[0]) || (a==AP_RUN && c->run_kind==1);
}
const char *desk_autoplay_label(const struct desktop *d,int a,char *out,size_t size)
{
    const struct ap_content *c=&d->sh.autoplay.content;
    if (c->cd_audio && a==AP_MUSIC) return "Play audio CD";
    if (a!=AP_RUN) return ap_action_name(a);
    const char *name=strrchr(c->program,'/'); name=name ? name+1 : c->inf.open;
    snprintf(out,size,"Run %.40s%s",name,c->run_kind==2 ? " (Windows; available in F5)" : c->run_kind!=1 ? " (unavailable)" : ""); return out;
}
void desk_autoplay_update(struct desktop *d,const struct ciuki_media_volume v[26])
{
    if (!d->autoplay_fs || d->sh.gate) return;
    struct desk_autoplay *a=&d->sh.autoplay;
    for (unsigned i=3;i<26;i++) {
        uint32_t generation=v[i].flags ? v[i].generation : 0;
        if (a->seen[i]==generation) continue;
        if (a->seen[i] && d->media_sound) d->media_sound(d,0);
        if (a->open && a->active.drive==i) {
            for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].internal==DESK_INTERNAL_AUTOPLAY) desk_drop_client(d,k);
            a->open=0; a->want_commit=0;
        }
        if (a->phase==AP_READY && a->active.drive==i) a->phase=AP_IDLE;
        a->waiting&=~(1u<<i); a->seen[i]=generation;
        if (generation) { a->pending[i]=v[i]; a->waiting|=1u<<i; if (d->media_sound) d->media_sound(d,1); }
    }
}
void desk_autoplay_cd_update(struct desktop *d,const struct ciuki_media_cd_state s[4])
{
    if (!d->autoplay_fs || d->sh.gate) return;
    struct desk_autoplay *a=&d->sh.autoplay;
    for (unsigned i=0;i<4;i++) {
        uint32_t gen=s[i].present && s[i].state==CIUKI_CD_READY && s[i].audio_ready ? s[i].generation : 0;
        if (gen==a->cd_seen[i]) continue;
        a->cd_seen[i]=gen; a->cd_waiting&=~(1u<<i);
        if (a->content.cd_audio && a->content.cd_slot==i) {
            for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].internal==DESK_INTERNAL_AUTOPLAY) desk_drop_client(d,k);
            a->open=0; a->want_commit=0;
            if (a->phase==AP_READY || a->phase==AP_LAUNCH_PENDING) a->phase=AP_IDLE;
        }
        if (gen) a->cd_waiting|=1u<<i;
    }
}
static int current_media(const struct desk_autoplay *a)
{
    if (a->content.cd_audio) return a->content.cd_slot<4 && a->cd_seen[a->content.cd_slot]==a->active.generation;
    return a->active.drive<26 && a->seen[a->active.drive]==a->active.generation;
}
static int input_owned(const struct desktop *d)
{
    if (d->focus>=0 || d->sh.levels || d->sh.switcher || d->action>=0 || d->sh.want_run[0] || d->keyboard_geometry || d->drag>=0 ||
        d->sh.volume_open || d->sh.network_open || d->sh.media_open) return 1;
    for (int k=0;k<DESK_CLIENTS;k++)
        if (d->clients[k].awaiting_frame || d->clients[k].launch_frame_pending) return 1;
    return 0;
}
static void remember(struct desktop *d)
{
    struct desk_autoplay *a=&d->sh.autoplay;
    if (a->always && a->active.id[0] && d->autoplay_preference) a->phase=AP_SAVE_PENDING;
}
static void execute_later(struct desktop *d)
{
    if (d->sh.autoplay.selected==AP_NOTHING) remember(d);
    else d->sh.autoplay.phase=AP_LAUNCH_PENDING;
}
int desk_autoplay_launch(struct desktop *d)
{
    struct desk_autoplay *a=&d->sh.autoplay; int action=a->selected;
    if (a->phase!=AP_LAUNCH_PENDING || d->power.state || input_owned(d)) return 0;
    a->phase=AP_IDLE;
    if (!current_media(a) || !desk_autoplay_enabled(d,action)) return 0;
    int pid=0;
    if (action==AP_FOLDER) pid=desk_task_launch(d,"files",a->content.root);
    if (action==AP_MUSIC && a->content.cd_audio) {
        struct desk_launch_record *r=calloc(1,sizeof(*r));
        if (!r) return -ENOMEM;
        snprintf(r->label,sizeof(r->label),"CD Player");
        snprintf(r->target,sizeof(r->target),"C:\\WINDOWS\\CDPLAYER.EXE");
        snprintf(r->arguments,sizeof(r->arguments),"--slot=%u",a->content.cd_slot);
        r->show=1;
        pid=d->v3_launch ? d->v3_launch(d,r) : -ENOSYS;
        free(r);
    } else if (action==AP_MUSIC) pid=desk_task_launch(d,"music",a->content.music);
    if (action==AP_PICTURES) pid=desk_task_launch(d,"viewer",a->content.picture);
    if (action==AP_RUN) { const char *args[]={a->content.command}; pid=desk_task_launch_args(d,"dosbox",args,1); }
    if (pid<0) desk_shell_message(d,"AutoPlay","The selected program could not be started.",1);
    else remember(d);
    return pid;
}
void desk_autoplay_service(struct desktop *d)
{
    struct desk_autoplay *a=&d->sh.autoplay;
    if (!d->autoplay_fs || d->sh.gate || d->power.state || !d->shell || !d->presents || !a->worker) return;
    if (a->phase==AP_LAUNCH_PENDING) return;
    if (a->phase==AP_SAVE_PENDING) {
        if (submit(a,AP_JOB_SAVE)) a->phase=AP_SAVING;
        return;
    }
    if (a->phase==AP_SAVING) {
        int error;
        if (!take(a,&error)) return;
        a->phase=AP_IDLE;
        if (error) desk_shell_message(d,"AutoPlay","The choice could not be saved. You will be asked again.",1);
        return;
    }
    if (a->want_commit) {
        a->want_commit=0;
        if (a->open && current_media(a) && desk_autoplay_enabled(d,a->selected)) execute_later(d);
        for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].internal==DESK_INTERNAL_AUTOPLAY) desk_drop_client(d,k);
        a->open=0;
        return;
    }
    if (a->phase==AP_PROBING) {
        int error;
        if (!take(a,&error)) return;
        a->phase=AP_IDLE;
        if (a->active.drive>=26 || a->seen[a->active.drive]!=a->active.generation) return;
        a->phase=AP_READY;
    }
    if (a->open || (!a->waiting && !a->cd_waiting && a->phase!=AP_READY)) return;
    /* Media discovery can finish between menu/Run key edges. Opening a
     * prompt here would close the menu (desk_internal_open) and redirect the
     * rest of that command to AutoPlay or the previously focused DOS window.
     * Keep arrivals queued until input returns to the desktop, including
     * across a native client's asynchronous first frame. */
    if (input_owned(d)) return;
    if (a->phase==AP_READY) {
        a->phase=AP_IDLE;
        if (a->saved>=0 && a->saved<AP_CHOICES && desk_autoplay_enabled(d,a->saved)) {
            a->selected=a->saved; execute_later(d); return;
        }
        if (desk_internal_open(d,DESK_INTERNAL_AUTOPLAY)<0) a->phase=AP_READY;
        else a->open=1;
        return;
    }
    if (a->cd_waiting) for (unsigned i=0;i<4;i++) if (a->cd_waiting&(1u<<i)) {
        memset(&a->content,0,sizeof(a->content)); memset(&a->active,0,sizeof(a->active));
        a->content.cd_audio=1; a->content.cd_slot=i;
        a->active.drive=26+i; a->active.generation=a->cd_seen[i]; a->active.flags=CIUKI_MEDIA_OPTICAL;
        a->selected=AP_MUSIC; a->always=0; a->saved=-1;
        a->cd_waiting&=~(1u<<i); a->phase=AP_READY;
        return;
    }
    for (unsigned i=3;i<26;i++) if (a->waiting&(1u<<i)) {
        a->active=a->pending[i]; a->selected=AP_FOLDER; a->always=0;
        if (submit(a,AP_JOB_PROBE)) {
            a->waiting&=~(1u<<i); a->phase=AP_PROBING;
        }
        break;
    }
}
