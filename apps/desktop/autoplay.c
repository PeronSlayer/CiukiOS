/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "autoplay.h"
#include <stdio.h>
#include <string.h>
const char *ap_action_name(int action)
{
    static const char *const names[]={"Open folder to view files","Play music","View pictures","Run program","Do nothing"};
    return action>=0 && action<AP_CHOICES ? names[action] : "Ask every time";
}
int desk_autoplay_enabled(const struct desktop *d,int a)
{
    const struct ap_content *c=&d->sh.autoplay.content;
    return a==AP_FOLDER || a==AP_NOTHING || (a==AP_MUSIC && c->music[0]) || (a==AP_PICTURES && c->picture[0]) || (a==AP_RUN && c->run_kind==1);
}
const char *desk_autoplay_label(const struct desktop *d,int a,char *out,size_t size)
{
    const struct ap_content *c=&d->sh.autoplay.content;
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
        a->waiting&=~(1u<<i); a->seen[i]=generation;
        if (generation) { a->pending[i]=v[i]; a->waiting|=1u<<i; if (d->media_sound) d->media_sound(d,1); }
    }
}
static void execute(struct desktop *d)
{
    struct desk_autoplay *a=&d->sh.autoplay; int action=a->selected;
    if (!desk_autoplay_enabled(d,action)) return;
    int pid=0;
    if (action==AP_FOLDER) pid=desk_task_launch(d,"files",a->content.root);
    if (action==AP_MUSIC) pid=desk_task_launch(d,"music",a->content.music);
    if (action==AP_PICTURES) pid=desk_task_launch(d,"viewer",a->content.picture);
    if (action==AP_RUN) { const char *args[]={a->content.command}; pid=desk_task_launch_args(d,"dosbox",args,1); }
    if (pid<0) { desk_shell_message(d,"AutoPlay","The selected program could not be started.",1); return; }
    if (a->always && a->active.id[0] && d->autoplay_preference && d->autoplay_preference(d,a->active.id,a->content.inf.label[0] ? a->content.inf.label : a->active.label,action)<0)
        desk_shell_message(d,"AutoPlay","The choice could not be saved. You will be asked again.",1);
}
void desk_autoplay_service(struct desktop *d)
{
    struct desk_autoplay *a=&d->sh.autoplay;
    if (!d->autoplay_fs || d->sh.gate || d->power.state) return;
    if (a->want_commit) {
        a->want_commit=0;
        if (a->open && a->active.drive<26 && a->seen[a->active.drive]==a->active.generation) execute(d);
        for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].internal==DESK_INTERNAL_AUTOPLAY) desk_drop_client(d,k);
        a->open=0;
    }
    if (a->open || !a->waiting) return;
    /* Media discovery can finish between menu/Run key edges. Opening a
     * prompt here would close the menu (desk_internal_open) and redirect the
     * rest of that command to AutoPlay or the previously focused DOS window.
     * Keep arrivals queued until input returns to the desktop, including
     * across a native client's asynchronous first frame. */
    if (d->focus>=0 || d->sh.levels || d->sh.switcher || d->action>=0 || d->sh.want_run[0] || d->keyboard_geometry || d->drag>=0 ||
        d->sh.volume_open || d->sh.network_open || d->sh.media_open) return;
    for (int k=0;k<DESK_CLIENTS;k++)
        if (d->clients[k].awaiting_frame) return;
    for (unsigned i=3;i<26;i++) if (a->waiting&(1u<<i)) {
        a->waiting&=~(1u<<i); a->active=a->pending[i]; a->selected=AP_FOLDER; a->always=0;
        if (ap_probe_workspace(d->autoplay_fs,i,&a->content,a->workspace)) {
            memset(&a->content,0,sizeof(a->content));
            snprintf(a->content.root,sizeof(a->content.root),"/mnt/%c",'a'+i);
        }
        int saved=a->active.id[0] && d->autoplay_preference ? d->autoplay_preference(d,a->active.id,NULL,-1) : -1;
        if (saved>=0 && saved<AP_CHOICES && desk_autoplay_enabled(d,saved)) { a->selected=saved; execute(d); continue; }
        a->open=1;
        if (desk_internal_open(d,DESK_INTERNAL_AUTOPLAY)<0) { a->open=0; a->waiting|=1u<<i; }
        break;
    }
}
