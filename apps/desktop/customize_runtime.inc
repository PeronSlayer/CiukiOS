/* SPDX-License-Identifier: MIT — G20 file operations on a bounded worker. */
#include "customize_io.h"
struct custom_worker {
    pthread_mutex_t lock; pthread_cond_t changed; pthread_t thread;
    int started,stop,pending,busy,ready,error,operation,seeded;
    unsigned seed_count;struct desk_program seed_programs[DESK_MENU_MAX];
    unsigned recent_serial;
    struct ciuki_settings cfg;
    struct sh_edit_request edit;
    char open[CFG_PATH_MAX];
    struct sh_folder_snapshot *result;
    struct desktop *desktop;
};
static struct custom_worker custom_worker;
static int custom_seed(struct custom_worker *w)
{
    /* Once only: deleted default shortcuts must stay deleted after a restart. */
    char marker[CFG_PATH_MAX]; sh_join(marker,sizeof(marker),SH_START_FOLDER,".g20-seeded");
    struct stat st; if (!stat(marker,&st)) return 0;
    int error=sh_mkdirs(SH_PROGRAMS_FOLDER);
    for (unsigned k=0;k<w->seed_count && !error;k++) {
        const char *category=desk_program_category(w->seed_programs[k].name);
        const char *group=!strcmp(category,"accessories") ? "Accessories" : !strcmp(category,"system-tools") ? "Accessories/System Tools" : !strcmp(category,"developer") ? "Developer" : "";
        char folder[CFG_PATH_MAX];
        if (sh_join(folder,sizeof(folder),SH_PROGRAMS_FOLDER,group)) { error=-ENAMETOOLONG;break; }
        int r=sh_shortcut_add(folder,w->seed_programs[k].path,desk_program_menu_label(w->seed_programs[k].name),NULL);
        if (r && r!=-EEXIST) error=r;
    }
    if (!error) {
        int r=sh_shortcut_add(SH_PROGRAMS_FOLDER "/Accessories","C:\\WINDOWS\\CDPLAYER.EXE","CD Player",NULL);
        if (r && r!=-EEXIST) error=r;
    }
    if (!error) error=sh_mkdirs(SH_PROGRAMS_FOLDER "/Games");
    for (unsigned k=0;k<DESK_GAME_COUNT && !error;k++) {
        char args[512];snprintf(args,sizeof(args),"\"--name=%s\" \"%s\"",desk_games[k].title,desk_games[k].command);
        int r=sh_shortcut_add(SH_PROGRAMS_FOLDER "/Games","/Programs/dosbox",desk_games[k].label,args);
        if (r && r!=-EEXIST) error=r;
    }
    if (!error) error=sh_mkdirs(SH_PROGRAMS_FOLDER "/StartUp");
    if (!error) error=sh_mkdirs(SH_QUICK_FOLDER);
    if (!error) error=sh_mkdirs(SH_LINKS_FOLDER);
    if (!error) error=sh_write_atomic(marker,"1\n",2);
    return error;
}
static int custom_open_path(const char *path)
{
    struct desk_launch_record *r=calloc(1,sizeof(*r));
    if (!r) return -ENOMEM;
    int error=0;size_t n=strlen(path);
    if (n>4 && !strcasecmp(path+n-4,".lnk")) error=sh_shortcut_read(path,r);
    else {
        snprintf(r->target,sizeof(r->target),"%s",path);
        snprintf(r->label,sizeof(r->label),"%.*s",63,sh_basename(path));r->show=1;
    }
    if (!error) {
        char native[260];snprintf(native,sizeof(native),"%s",r->target);
        if ((native[0]=='C' || native[0]=='c') && native[1]==':') memmove(native,native+2,strlen(native+2)+1);
        for (char *p=native;*p;p++) if (*p=='\\') *p='/';
        struct stat st;
        const char *program=NULL;size_t length=strlen(native);
        if (length>4 && !strcasecmp(native+length-4,".url")) {
            error=sh_url_read(native,r->target,sizeof(r->target));
            if (!error) program="netsurf";
        }
        if (!strncasecmp(r->target,"http://",7) || !strncasecmp(r->target,"https://",8) || !strncasecmp(r->target,"ftp://",6)) program="netsurf";
        else if (!stat(native,&st) && S_ISDIR(st.st_mode)) program="files";
        else {
            const struct desk_catalog_entry *app=desk_catalog_file(native);
            if (app) program=app->name;
        }
        if (program) {
            if (strchr(r->target,'"')) error=-EINVAL;
            else {
                snprintf(r->arguments,sizeof(r->arguments),"\"%s\"",!strcmp(program,"netsurf") ? r->target : native);
                snprintf(r->target,sizeof(r->target),"/Programs/%s",program);
            }
        }
        if (!error) error=ciuki_win32_shell_launch(r);
    }
    free(r);return error;
}
static void *custom_thread(void *arg)
{
    struct custom_worker *w=arg;
    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->stop && !w->pending) pthread_cond_wait(&w->changed,&w->lock);
        if (w->stop) { pthread_mutex_unlock(&w->lock);break; }
        w->pending=0;w->busy=1;pthread_mutex_unlock(&w->lock);
        int seed_error=w->seeded ? 0 : custom_seed(w);w->seeded=1;
        int error=sh_edit_apply(&w->edit);
        if (!error && seed_error) error=seed_error;
        if (!error && w->open[0]) error=custom_open_path(w->open);
        struct sh_folder_snapshot *s=calloc(1,sizeof(*s));
        if (!s && !error) error=-ENOMEM;
        if (s) sh_snapshot_load(s,&w->cfg);
        if (!error && w->edit.operation==SH_EDIT_CLEAR) { w->cfg.recent_count=0;memset(w->cfg.recent,0,sizeof(w->cfg.recent)); }
        else sh_recent_merge(&w->cfg,SH_RECENT_FILE);
        pthread_mutex_lock(&w->lock);
        w->result=s;w->error=error;w->operation=w->edit.operation;w->ready=1;w->busy=0;
        desk_wake(w->desktop);pthread_mutex_unlock(&w->lock);
    }
    return NULL;
}
static inline int custom_prepare(struct desktop *d)
{
    struct custom_worker *w=&custom_worker;
    if (w->started) return w->desktop==d ? 0 : -EBUSY;
    memset(w,0,sizeof(*w));w->desktop=d;w->seed_count=d->program_count;
    memcpy(w->seed_programs,d->programs,sizeof(w->seed_programs));
    int e=pthread_mutex_init(&w->lock,NULL);if (e) return -e;
    e=pthread_cond_init(&w->changed,NULL);
    if (e) { pthread_mutex_destroy(&w->lock);return -e; }
    e=pthread_create(&w->thread,NULL,custom_thread,w);
    if (e) { pthread_cond_destroy(&w->changed);pthread_mutex_destroy(&w->lock);return -e; }
    w->started=1;d->sh.custom_refresh=1;return 0;
}
static void custom_service(struct desktop *d)
{
    struct custom_worker *w=&custom_worker;struct desk_shell *s=&d->sh;
    if (s->gate || w->desktop!=d) return;
    if (!w->started) {
#ifdef __i386__
        /* Power preparation closes file workers; Cancel reopens them. */
        static uint64_t retry;
        if (d->power.state || d->now_ns<retry) return;
        retry=d->now_ns+2000000000ull;
        int error=custom_prepare(d);
        if (error) { desk_shell_notify_error(d,"Shell worker unavailable",error);return; }
#else
        return;
#endif
    }
    if (pthread_mutex_trylock(&w->lock)) return;
    if (w->ready && (s->custom_drag>=0 || (s->levels && s->menus[0].kind==SH_MENU_START_ITEM))) {
        pthread_mutex_unlock(&w->lock);return;
    }
    if (w->ready) {
        struct sh_folder_snapshot *old=s->custom;
        s->custom=w->result;w->result=NULL;w->ready=0;
        if (old && s->custom) for (int l=0;l<s->levels;l++) if (s->menus[l].kind>=SH_MENU_FOLDER_BASE) {
            unsigned root=(unsigned)(s->menus[l].kind-SH_MENU_FOLDER_BASE),found=SH_FOLDERS;
            if (root<old->folders_count) for (unsigned k=0;k<s->custom->folders_count;k++)
                if (!strcmp(old->folders[root],s->custom->folders[k])) { found=k;break; }
            if (found==SH_FOLDERS) { desk_menu_close(d);break; }
            s->menus[l].kind=SH_MENU_FOLDER_BASE+(int)found;
        }
        free(old);s->custom_refresh=0;desk_shell_custom_updated(d);
        if (w->error) desk_shell_notify_error(d,"Taskbar & Start Menu",w->error);
        else {
            if (w->operation==SH_EDIT_TOOLBAR) {
                unsigned k=0;for (;k<CFG_TOOLBARS;k++) if (!s->cfg.toolbar_folder[k][0]) break;
                if (k==CFG_TOOLBARS) desk_shell_notify_error(d,"New Toolbar",ENOSPC);
                else { strcpy(s->cfg.toolbar_folder[k],w->edit.path);s->want_save=1;s->custom_refresh=1; }
            }
            if (w->recent_serial==s->recent_serial || w->operation==SH_EDIT_CLEAR) {
                memcpy(s->cfg.recent,w->cfg.recent,sizeof(s->cfg.recent));s->cfg.recent_count=w->cfg.recent_count;
                desk_shell_custom_updated(d);
            } else s->custom_refresh=1;
            if (w->operation==SH_EDIT_CLEAR) s->want_save=1;
        }
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    }
    if ((!d->power.state || s->edit.operation) && !w->busy && !w->pending && (s->custom_refresh || s->edit.operation || s->custom_open[0])) {
        w->cfg=s->cfg;w->recent_serial=s->recent_serial;w->edit=s->edit;strcpy(w->open,s->custom_open);
        s->custom_refresh=0;s->edit.operation=0;s->custom_open[0]=0;w->pending=1;
        pthread_cond_signal(&w->changed);
    }
    pthread_mutex_unlock(&w->lock);
}
static int custom_busy(void)
{
    struct custom_worker *w=&custom_worker;
    if (!w->started) return 0;
    if (pthread_mutex_trylock(&w->lock)) return 1;
    int busy=w->busy || w->pending;
    pthread_mutex_unlock(&w->lock);return busy;
}
static void custom_stop(void)
{
    struct custom_worker *w=&custom_worker;if (!w->started) return;
    pthread_mutex_lock(&w->lock);w->stop=1;pthread_cond_signal(&w->changed);pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread,NULL);free(w->result);w->result=NULL;
    pthread_cond_destroy(&w->changed);pthread_mutex_destroy(&w->lock);w->started=0;
}
